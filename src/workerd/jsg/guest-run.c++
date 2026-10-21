// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "guest-run.h"

#include <workerd/util/alloc-census.h>

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>

#include <kj/async.h>
#include <kj/debug.h>

#include <atomic>
#include <cstring>

namespace workerd::jsg::_ {
namespace {

// Depth of guest-kernel turns on this thread. A nested turn already executes in the guest and
// must not call gk_run_here_user() again from inside it. Thread-local storage is memory the
// guest can write, so the outermost turn restores the depth from the value it saved in its own
// host frame (read-only under the wall) rather than by decrementing.
thread_local uint guestTurnDepth = 0;

// The arena the outermost guest turn on this thread entered the guest with. A nested turn runs
// inside that same guest entry, so it must belong to an isolate with the same arena: switching
// the guest's page-table root from inside the guest is not supported yet, and a nested turn for
// another isolate would run with that isolate's sandbox unaddressable. Such turns are detected
// and counted here rather than supported.
//
// This is thread-local storage the turn can write, and that is acceptable: the only reader is
// the comparison below, made at ring 3 inside the turn, and the host never dereferences the
// value. A turn that rewrites it can at most make a mismatched nested turn go undetected, and
// such a turn then faults on the sandbox it cannot address -- the turn's own loss. After the
// outermost turn the host stores a constant here rather than trusting what the turn left.
thread_local gk_arena* guestTurnArena = nullptr;
std::atomic<uint> nestedGuestTurnArenaMismatches{0};

gk_arena* guestArenaOf(Lock& js) {
  KJ_IF_SOME(arena, IsolateBase::from(js.v8Isolate).getGuestArena()) {
    return arena.get();
  }
  return nullptr;
}

// Stack traces stringified inside a turn carry addresses only. KJ's full mode symbolizes a trace
// by piping it through an llvm-symbolizer child process, which a turn cannot do: the syscall
// filter (guestKernelSyscallFilter in server/workerd.c++) denies pipe2, fork and execve, so the
// attempt could only ever fail. The callback lives on the host frame that runs the turn, so
// a turn cannot outlast it; the thread-local that KJ reaches it through is guest-writable, and a
// turn that redirects it to the full mode gets the failed attempt that the symbolizer reports
// as unsymbolized, with the cost borne by that turn alone.
class GuestTurnExceptionCallback final: public kj::ExceptionCallback {
 public:
  StackTraceMode stackTraceMode() override {
    return StackTraceMode::ADDRESS_ONLY;
  }
};

}  // namespace

void runTrampolineInGuest(
    Lock& js, long (*fn)(void*), void* arg, kj::FunctionParam<void(const kj::Exception&)> onFault) {
  gk_arena* arena = guestArenaOf(js);
  if (guestTurnDepth > 0) {
    if (arena != guestTurnArena) {
      uint count = ++nestedGuestTurnArenaMismatches;
      KJ_LOG(FATAL,
          "guest-kernel: nested JS turn for an isolate in a different arena than the enclosing "
          "guest turn; its sandbox is not addressable from the guest",
          arena, guestTurnArena, count);
    }
    // Already at guest ring 3 under the enclosing turn's gk_run_here_user().
    fn(arg);
    return;
  }

  uint depth = guestTurnDepth;
  guestTurnDepth = depth + 1;
  KJ_DEFER(guestTurnDepth = depth);
  guestTurnArena = arena;
  KJ_DEFER(guestTurnArena = nullptr);
  GuestTurnExceptionCallback exceptionCallback;

  // The isolate's jsg data slots (SetDataIndex in jsg.h): the current jsg::Lock, which Lock::from
  // returns, the IsolateBase, the type wrapper, the Worker::Isolate, the cage base and the Rust
  // realm. They are embedder slots in the C++ isolate object, in the shared runtime heap, so a
  // turn can write them, while host code holding the lock reads them after the turn through
  // Lock::from and its siblings and calls through what it finds. So they are saved here, in this
  // host frame (read-only to the turn under the host-frame wall), and put back once the guest has
  // returned, whatever the turn left in them -- the same remedy the host-side thread-locals get
  // (GuestTurnThreadLocals in io/). A nested turn (above) is at ring 3 and does not do this; the
  // outermost turn's restore runs last. This holds against this thread's turn; a turn on another
  // thread can write the slots at any time, including after this restore, as it can any shared
  // heap memory (see gk.h).
  v8::Isolate* isolate = js.v8Isolate;
  void* dataSlots[SET_DATA_SLOTS_IN_USE];
  for (uint i = 0; i < SET_DATA_SLOTS_IN_USE; i++) {
    dataSlots[i] = isolate->GetData(i);
  }
  // The count of jsg::Locks alive on this thread gets the same treatment for the same reason:
  // it is thread-local storage the turn can write, and the Lock constructor reads it after the
  // turn to tell a recursive lock from a bare v8::Locker (see saveLockDepthForGuestTurn).
  uint lockDepth = saveLockDepthForGuestTurn();
  // The flag that says this thread is inside a CppgcShim destructor gets it too (see
  // HeapTracer::saveInCppgcDestructorForGuestTurn): a CppgcShim destructor runs inside the turn,
  // so a turn's frame sets it, and the host reads it after the turn -- Data's destructor skips
  // resetting a TracedReference while it is set, which leaks the node, and clearWrappers()
  // asserts it is clear when any isolate on this thread is torn down.
  bool inCppgcDestructor = HeapTracer::saveInCppgcDestructorForGuestTurn();

  // The scope that disallows destroying kj::AsyncObjects (DISALLOW_KJ_IO_DESTRUCTORS_SCOPE in
  // wrappable.h) is reached through a thread-local pointer to the scope object on the stack, and
  // Wrappable::maybeDeferDestruction enters one around every destruction of a JS heap object the
  // isolate lock is held for -- inside the turn, where the pointer names a frame of the turn's.
  // A fault abandons that frame, and the pointer then names reused stack memory, which the next
  // ~AsyncObject on this thread -- the next promise node the event loop destroys -- dereferences
  // from a noexcept function and aborts the process on. The turn can also write the pointer
  // itself, so the host would dereference an address of the turn's choosing.
  //
  // kj::AllowAsyncDestructorsScope is the save/restore pair for it: its constructor clears the
  // thread-local and its destructor puts back what it found, and this one is in a host frame, so
  // it runs whether the turn returns or faults. For the turn's duration the thread-local is
  // clear, so a scope entered *above* the guest boundary -- a host-side destruction that enters
  // an IoContext and runs a nested turn -- does not disallow destructors inside that turn; the
  // scopes the turn's own frames enter are unaffected, which is every site that runs in a turn.
  // (WORKERD_GK_NEG_DISALLOW_DTORS is a negative-test build, never shipped: the pointer the turn
  // left stands, and experimental/guest-kernel/fault-leak-test.sh must then fail -- the process
  // dies once the fault's unwinding destroys an async object.)
#ifndef WORKERD_GK_NEG_DISALLOW_DTORS
  kj::AllowAsyncDestructorsScope asyncDestructorScope;
#endif

  // The pointer this thread finds that loop through (threadLocalEventLoop in kj/async.c++) is
  // thread-local storage the turn can write, and it stands at the head of every host-side lookup
  // of event-loop state: the loop an event is armed on, the executor getCurrentThreadExecutor
  // hands out, and the local map EventLoop::getLocal reads, where the async lock queue keeps the
  // waiter that proves an isolate lock was taken. Turning the loop is not one of them -- WaitScope
  // holds its loop by reference and asserts the pointer still names it -- but nothing the host
  // does reach through the pointer is bounded by an assert, so a turn that leaves it naming an
  // EventLoop of its own has the host operating on that object, and one that leaves it cleared
  // fails those lookups instead, from noexcept paths among others. kj exposes no accessor for the
  // pointer, which is what
  // patches/capnp-cpp/0004-Expose-the-thread-s-current-event-loop-pointer.patch adds. The
  // pointer is left naming the real loop for the turn's duration -- in-turn code arms its own
  // events and reaches the loop through it -- and put back once the guest has returned, whatever
  // the turn left in it.
  kj::EventLoop* eventLoop = kj::EventLoop::saveCurrentLoopPointer();
  // The event this thread is firing (threadLocalCurrentlyFiring, same file) is the other half of
  // that state, and it is still set while this turn runs: EventLoop::turn() sets it around
  // Event::fire(), and the frame that enters the guest is somewhere inside that fire(). kj clears
  // it when fire() returns, and with a constant rather than a saved value, so nothing a turn
  // leaves here outlives the fire() that ran the turn -- but the rest of that fire() is host code
  // that runs on what the turn left. ~Event() compares the pointer with every event destroyed on
  // this thread, from a noexcept function that aborts the process on a match, and the host
  // destroys promise nodes in that window, the fault path's abort() most of all. So a turn could
  // pick the moment the process dies. (kj::getAsyncTrace(), the one reader that dereferences the
  // pointer and calls through its vtable, has no caller in this tree.)
  kj::_::Event* firingEvent = kj::EventLoop::saveCurrentlyFiringEvent();

  allocCensus::enterTurn();
  long result = gk_run_here_user(fn, arg);
#ifndef WORKERD_GK_NEG_LOOP_POINTER
  // Put back before anything else the host does here: on the fault path below it condemns the
  // isolate and aborts the request, which arms events on this thread's loop.
  // (WORKERD_GK_NEG_LOOP_POINTER is a negative-test build, never shipped: the pointer the turn
  // left stands, and experimental/guest-kernel/event-loop-test.sh must then fail -- the host
  // finds the loop the turn chose.)
  kj::EventLoop::restoreCurrentLoopPointer(eventLoop);
  kj::EventLoop::restoreCurrentlyFiringEvent(firingEvent);
#else
  (void)eventLoop;
  (void)firingEvent;
#endif
  allocCensus::leaveTurn();

  for (uint i = 0; i < SET_DATA_SLOTS_IN_USE; i++) {
    isolate->SetData(i, dataSlots[i]);
  }
#ifndef WORKERD_GK_NEG_LOCK_DEPTH
  // WORKERD_GK_NEG_LOCK_DEPTH is a negative-test build, never shipped: the count the turn left
  // stands, which is what it did before the count was saved here. lock-slot-test.sh must then
  // fail -- the host reads the turn's sentinel back after the turn.
  restoreLockDepthAfterGuestTurn(lockDepth);
#else
  (void)lockDepth;
#endif
#ifndef WORKERD_GK_NEG_CPPGC_FLAG
  // WORKERD_GK_NEG_CPPGC_FLAG is a negative-test build, never shipped: the flag the turn left
  // stands, and fault-leak-test.sh must then fail -- the host reads the turn's value back.
  HeapTracer::restoreInCppgcDestructorAfterGuestTurn(inCppgcDestructor);
#else
  (void)inCppgcDestructor;
#endif
  if (result != 0) {
    // The guest faulted (or gk otherwise could not complete the turn) and control is back here
    // without the turn's frames ever returning. gk_run_here_user() is an ordinary host-side call
    // frame, so everything above it unwinds normally; everything the turn had on the stack below
    // the guest boundary is abandoned and its destructors never run. See runInGuest() for what
    // that means for the isolate and for the caller.
    auto e = KJ_EXCEPTION(FAILED,
        "guest-kernel: JS turn faulted inside the guest; the isolate has been condemned", result,
        gk_fault_addr());
    onFault(e);
    kj::throwFatalException(kj::mv(e));
  }
}

void GuestTurnOutcome::setException(const kj::Exception& e) {
  auto description = e.getDescription();
  uint32_t length = static_cast<uint32_t>(kj::min(description.size(), MAX_DESCRIPTION));
  memcpy(this->description, description.begin(), length);
  descriptionLength = length;
  exceptionType = static_cast<uint32_t>(e.getType());
  status = EXCEPTION;
}

void throwIfGuestTurnFailed(const GuestTurnOutcome& outcome) {
  switch (outcome.status) {
    case GuestTurnOutcome::RETURNED:
      return;
    case GuestTurnOutcome::JS_EXCEPTION_THROWN:
      throw JsExceptionThrown();
    case GuestTurnOutcome::EXCEPTION: {
      auto type = kj::Exception::Type::FAILED;
      if (outcome.exceptionType <= static_cast<uint32_t>(kj::Exception::Type::UNIMPLEMENTED)) {
        type = static_cast<kj::Exception::Type>(outcome.exceptionType);
      }
      uint32_t length = kj::min(outcome.descriptionLength, GuestTurnOutcome::MAX_DESCRIPTION);
      kj::throwFatalException(
          kj::Exception(type, __FILE__, __LINE__, kj::heapString(outcome.description, length)));
    }
    default:
      KJ_FAIL_REQUIRE("guest-kernel: JS turn reported an invalid status", outcome.status);
  }
}

}  // namespace workerd::jsg::_
#endif  // WORKERD_HAS_GUEST_KERNEL

namespace workerd::jsg {
namespace {

// Test-only (see setGuestTurnTailHookForTest). The hook object is on the heap: it is set from
// inside a turn and runs inside the next turn, and nothing host-side reads it.
kj::Function<void()>* guestTurnTailHookForTest = nullptr;

}  // namespace

void setGuestTurnTailHookForTest(kj::Function<void()> hook) {
  KJ_REQUIRE(guestTurnTailHookForTest == nullptr);
  guestTurnTailHookForTest = new kj::Function<void()>(kj::mv(hook));
}

void _::runGuestTurnTailHookForTest() {
  if (guestTurnTailHookForTest != nullptr) {
    auto hook = kj::mv(*guestTurnTailHookForTest);
    delete guestTurnTailHookForTest;
    guestTurnTailHookForTest = nullptr;
    hook();
  }
}

}  // namespace workerd::jsg
