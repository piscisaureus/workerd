// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "guest-run.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>

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

  long result = gk_run_here_user(fn, arg);

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
