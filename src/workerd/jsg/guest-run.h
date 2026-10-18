// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/util/memory-class.h>

#include <kj/function.h>

namespace workerd::jsg {

// Experimental guest-kernel isolation (see experimental/guest-kernel/gk.h). runInGuest() runs
// `body` -- one JS turn, or a worker's top-level execution -- at guest ring 3 via
// gk_run_here_user() when guest-kernel isolation is enabled (isGuestKernelEnabled()), and calls
// it directly otherwise. It returns what `body` returns.
//
// `body` must run under the isolate lock, whose jsg::Lock has already made the isolate's arena
// the thread's active one (GuestArenaScope), so the guest enters with that arena's page-table
// root and can address this isolate's sandbox and no other's. At ring 3, `body` (V8, JIT, and
// the C++ runtime it calls) cannot reload CR3, run privileged instructions, or touch gk's
// supervisor/refused pages: any such attempt faults and gk_run_here_user() returns GK_EFAULT.
// That confines it to this isolate's arena and keeps it out of gk's control state and of host
// memory (util/host-memory.h, the memory no guest root maps, where runtime secrets go), but not
// out of the rest of the shared runtime memory (the C++/KJ heap, glibc, and this thread's stack,
// which `body` runs on and which holds the host frames below the guest boundary): those are
// user-mapped, so an escape to arbitrary code execution can still corrupt shared state and,
// through a host return address on the stack, potentially reach host execution. See gk.h;
// moving the rest of the runtime's mutable state into host memory is remaining work.
//
// The guest boundary is not a C++ call frame, so an exception thrown inside `body` cannot unwind
// across it. The trampoline that runs `body` in the guest catches everything and records it,
// together with `body`'s return value, in a heap-allocated turn record that the host side reads
// back once the guest has returned, re-throwing the exception so that callers observe the same
// exceptions as without the guest. The record is written by code running in the guest, so the
// host treats it as data, not as C++ objects: see GuestTurnOutcome.
//
// Host-frame wall (see gk_run_here_user in gk.h): for the length of the call, the caller's stack
// frames above the guest boundary are read-only to `body`. `body` and everything it calls must
// therefore write only below its own entry frame or off the stack (the C++ heap, the isolate,
// thread-local storage), never to a local of any frame above runInGuest(). Two consequences for
// callers: a v8::TryCatch that is to catch an exception `body` throws must be declared inside
// `body`, because V8 records the exception by writing into the TryCatch; and any out-parameter
// `body` fills in must live on the heap. The turn record exists so that `body`'s own outcome
// needs no such caller-owned slot. The closure object itself is read from the caller's frame,
// so it must not be a mutable lambda that assigns its captures.
//
// If the guest faults, control returns here without `body`'s frames ever returning: their
// destructors never run and the RAII state they held leaks, and the isolate is unusable from
// then on -- V8 was interrupted at an arbitrary point (possibly mid-allocation or mid-JIT), and
// its per-thread state (HandleScope chain, v8::TryCatch chain, JS entry frames) still points
// into the abandoned frames. `onFault` is then called host-side with the exception describing
// the fault, so that the caller can fence the isolate off (Worker::Isolate::condemn()) and
// cancel whatever depended on `body`, and that exception is thrown. The process and every other
// isolate carry on.
//
// A nested call from inside `body` (for example, a destructor run during JS may enter another
// IoContext's scope and run a turn there) already executes in the guest and runs directly; it
// must belong to an isolate with the same arena, because switching the guest's page-table root
// from inside the guest is not supported. Such mismatches are detected and counted, not
// supported.
//
// TODO(guest-kernel): RAII state held by the abandoned frames leaks, and no general bookkeeping
//   could release it: the frames are not enumerable and the state is reached through whatever
//   each frame happens to hold. What the fault path does release is the state that is
//   thread-wide, because that outlives the condemned isolate and so costs more than the turn.
//   Thread-wide state is reachable without enumerating anything, by saving it in the host frame
//   that runs the turn and putting it back whether the turn returns or faults, and every piece
//   of it that has an owner in jsg or above is handled that way: the thread-locals the host
//   reads after a turn (GuestTurnThreadLocals in io/guest-turn-thread-locals.h), the isolate's
//   jsg data slots, the count of jsg::Locks this thread holds, the flag that says this thread is
//   inside a CppgcShim destructor and the scope that disallows destroying kj::AsyncObjects (the
//   last four in runTrampolineInGuest, guest-run.c++), the turn's resolve journal and the event
//   loop's arm guard (runJsTurn and runTrampolineInGuest), and the running mark a timeout
//   callback's TimeoutState holds, which ~TimeoutState asserts on when the IoContext is
//   destroyed (runJsTurn in io/io-context.c++). That save and restore is also what keeps a turn
//   from choosing the value the host reads, so each of those is one remedy for two problems;
//   experimental/guest-kernel/memory-classes.md, rule R7, is the inventory. The restore is
//   unconditional, which is what covers a turn that leaves such a thread-local cleared as
//   well as one that leaves it dangling at an abandoned frame: every IoOwn disposal enters
//   an AllowAsyncDestructorsScope of its own, so a fault inside one leaves the check
//   disabled rather than dangling, and the host frame puts the host's value back either
//   way.
//
//   What makes thread-wide state reachable at all, rather than only under an unfixed placement
//   bug: a destructor running inside a turn can enter another IoContext's scope and run a nested
//   JS turn there (see runInContextScope in io/io-context.c++), so user JS runs synchronously
//   under whatever the frames below it hold, and an escape faults at its own site inside it.
//   Wrappable::maybeDeferDestruction is the frame that matters, because it enters the
//   disallow-async-destructors scope around every destruction of a JS heap object the isolate
//   lock is held for, and those destructions are where the nested turns come from.
//
//   Known leaks per faulted turn:
//   - for a JS turn, the IoContext::PendingEvent that IoContext::runImpl's turn registers, so
//     that IoContext is never seen as idle by hang detection (moot once it is aborted and
//     destroyed), and the limit enforcer's enterJs() scope, so it never sees the matching exit.
//     The running mark a timeout callback's TimeoutState holds is the one such leak the fault
//     path does clear, because ~TimeoutState asserts on it when the IoContext is destroyed and
//     that would take the process down (see runJsTurn in io/io-context.c++);
//   - any kj::Own / jsg::Ref / IoOwn / file descriptor / KJ mutex lock the C++ frames held at
//     the moment of the fault (a held KJ mutex would deadlock its next locker). The test hook
//     in ServiceWorkerGlobalScope::request() faults at the top of that function, before any
//     such state is taken, so no test exercises this case; a turn that faults where a sandbox
//     escape would, inside the handler it called, abandons whatever the frames below it hold.
//     The neuterable stream wrappers are that case: request()'s request body, and connect()'s
//     connection and channel, each wrap a stream the HTTP or datagram layer owns and neuter it
//     from a kj::defer and a KJ_ON_SCOPE_FAILURE in the frame that faults, so a fault leaves
//     the wrapper pointing at a stream that layer then frees. Nothing reads it, for two
//     reasons that are invariants rather than accidents: the fault condemns the isolate, so no
//     JS ever runs on the readers reachable from the Request or the Socket; and the turn holds
//     the isolate lock to its fault without yielding, so the IoContext's own in-flight reads
//     are cancelled by its teardown before the exception reaches the layer that frees the
//     stream. A change that lets any reader outlive the fault -- evicting a condemned isolate,
//     or resuming a parked read between the fault and the teardown -- turns all three sites
//     into host-side use-after-frees. The fix then is to give each wrapper an owner that
//     outlives the turn (IoContext::addObject before the handler runs) rather than to neuter
//     from the fault path, which has no handle to per-request state in api/. A lock is the one
//     member of this bullet whose cost is not confined to the faulted request: its next locker
//     blocks, whatever isolate that locker serves. The costliest one is not a KJ mutex but V8's
//     ThreadIsolation::jit_pages_mutex_, a process-wide base::Mutex held by a MutexGuard on the
//     stack across every registration, split and lookup of the JIT-page index -- which a turn
//     reaches whenever it allocates code -- and whose next locker is any isolate on any thread,
//     including this one on its own next allocation. Releasing it from the fault path would be
//     worse than leaking it, which is the general shape of this item: the frame was abandoned
//     part-way through mutating what the lock protects (RegisterJitPage faults walking the
//     index's nodes with the lock held, which is how the index's own misplacement showed up;
//     see jsg/thread-isolated-allocator.h), so unlocking hands the next locker a half-mutated
//     structure. Only the sites can answer it, by not holding a lock across code a fault can
//     reach, and nothing could tell whether such a site list were complete, since any call
//     inside the window may take a lock of its own;
//   - the guard of any function-local static the frames were initializing, which stays owned by
//     the thread, so this one costs more than the turn: the next turn to reach that static dies
//     on "__cxa_guard_acquire detected recursive initialization", taking the process with it, if
//     it runs on the same thread, and waits forever for an initialization that will never finish
//     if it runs on another. Aborting the guard instead would let the next caller construct the
//     static again over whatever a half-run constructor left in its storage, so there is nothing
//     the fault path can do about it. The guard belongs to libc, which exposes no way to release
//     or abandon one, and the only remedy is for no fault to be possible inside an
//     initialization: experimental/guest-kernel/atexit-arena-test.sh covers the one such window
//     that is reached on every turn, the __cxa_atexit registration a static with a non-trivial
//     destructor makes while it holds its own guard, and reports the abort when it sees it;
//   - the state SQLite keeps for the call a turn is inside. Two pieces are confined to the
//     faulted isolate's database, since that is what holds them: the running statement and the
//     regulator SqliteDatabase::Query::nextRow sets, which leave the next nextRow() on that
//     database asserting. The third is thread-wide: `currentVfsRoot` (util/sqlite.c++), which
//     NativeVfsCall sets for the length of one native VFS call and whose constructor asserts
//     the thread has none, so a fault inside such a call leaves every later native VFS call on
//     the thread failing that assert, for any actor the thread serves, and leaves the guest
//     kernel's turn directory naming the faulted database's directory. Its bookkeeping would be
//     the save and restore above plus a gk_turn_dir_set(GK_TURN_DIR_NONE) in the fault path;
//     what is missing is a reason to expect a fault there, since no JS runs inside a native VFS
//     call. `vfsErrorListener`, the other thread-local of that file, needs nothing: every
//     consumer is inside a SqliteCallScope, whose constructor sets the thread-local before any
//     use, so a stale one is overwritten rather than read;
//   - on the V8 side, the abandoned HandleScopes' handles, and the isolate itself, which is
//     never freed because nothing evicts a condemned isolate (see Worker::Isolate::condemn).
//
//   What divides the two halves of this list is not how wide the state is but whether its value
//   stands on its own. A flag, a count, a pointer or a thread-local can be put back, because the
//   saved value is the whole of the state and nothing else has to agree with it; the fault path
//   restores every such piece it can name. A lock, a guard variable or a half-run constructor
//   cannot, because what the frame was abandoned in the middle of is a multi-word invariant that
//   the saved value says nothing about, and restoring the one word publishes the broken
//   invariant. For those the remedy is at the site -- not holding the thing across code a fault
//   can reach -- and no mechanism here can be complete, because what a fault can reach inside
//   such a window is every call the window makes.
template <typename Func>
auto runInGuest(Lock& js,
    Func&& body,
    kj::FunctionParam<void(const kj::Exception&)> onFault) -> decltype(body());

// Test-only (see maybeForceGuestFaultForTest in api/global-scope.c++): `hook` runs once, inside
// the guest, as the next turn's last act -- after `body` has returned and its outcome has been
// recorded, before control returns to the host -- so a test can leave something in memory the
// host reads after the turn with nothing of the turn's own left to read it first. Without the
// guest kernel nothing runs it.
void setGuestTurnTailHookForTest(kj::Function<void()> hook);

namespace _ {  // private

// Runs, and clears, the hook set by setGuestTurnTailHookForTest, if any. In-guest.
void runGuestTurnTailHookForTest();

}  // namespace _

#ifdef WORKERD_HAS_GUEST_KERNEL
namespace _ {  // private

// What `body` did, written in-guest by the trampoline and read host-side after the guest
// returns. Lives on the heap (see the host-frame wall above), which guest execution can write,
// so the host trusts nothing in it: the status is checked against the closed set below, and an
// exception crosses as plain bytes -- its type and a copy of its description in a fixed buffer,
// the length clamped host-side -- from which the host constructs a new kj::Exception. No
// kj::String, disposer, context chain or detail array crosses the boundary; the exception's
// context chain, details, trace and origin file:line are therefore lost, which is accepted.
struct WD_MEMORY_CLASS_BOUNDARY GuestTurnOutcome {
  enum Status : uint32_t {
    // The trampoline has not completed; a record read with this status is an error.
    INCOMPLETE = 0,
    // `body` returned.
    RETURNED = 1,
    // `body` threw JsExceptionThrown; the pending exception stays on the isolate.
    JS_EXCEPTION_THROWN = 2,
    // `body` threw something else, carried in `exceptionType` and `description`.
    EXCEPTION = 3,
  };
  static constexpr uint32_t MAX_DESCRIPTION = 4096;

  uint32_t status = INCOMPLETE;
  uint32_t exceptionType = 0;      // kj::Exception::Type, checked host-side
  uint32_t descriptionLength = 0;  // bytes of `description` in use, clamped host-side
  char description[MAX_DESCRIPTION];

  // Records `e` as the turn's outcome (in-guest).
  void setException(const kj::Exception& e);
};

// Acts on the outcome a turn reported, host-side: returns if `body` returned, rethrows
// JsExceptionThrown, or throws a kj::Exception built from the record's bytes. A status outside
// the closed set is an error of its own.
void throwIfGuestTurnFailed(const GuestTurnOutcome& outcome);

template <typename Result>
struct GuestTurnResult {
  kj::Maybe<Result> value;
};
template <>
struct GuestTurnResult<void> {};

template <typename Func>
struct GuestTurn: public GuestTurnOutcome {
  using Result = decltype(kj::instance<Func&>()());

  Func& body;
  GuestTurnResult<Result> result;

  explicit GuestTurn(Func& body): body(body) {}

  static long trampoline(void* ptr) {
    auto& turn = *reinterpret_cast<GuestTurn*>(ptr);
    try {
      if constexpr (kj::isSameType<Result, void>()) {
        turn.body();
      } else {
        turn.result.value.emplace(turn.body());
      }
      turn.status = GuestTurnOutcome::RETURNED;
    } catch (const JsExceptionThrown&) {
      // The pending exception stays on the isolate; only the C++ signal must be re-raised.
      turn.status = GuestTurnOutcome::JS_EXCEPTION_THROWN;
    } catch (...) {
      turn.setException(kj::getCaughtExceptionAsKj());
    }
    runGuestTurnTailHookForTest();
    return 0;
  }
};

// Runs fn(arg) at guest ring 3, or directly when already inside the guest. On a guest fault,
// calls onFault with the exception describing it and then throws that exception.
void runTrampolineInGuest(
    Lock& js, long (*fn)(void*), void* arg, kj::FunctionParam<void(const kj::Exception&)> onFault);

}  // namespace _
#endif  // WORKERD_HAS_GUEST_KERNEL

template <typename Func>
auto runInGuest(Lock& js,
    Func&& body,
    kj::FunctionParam<void(const kj::Exception&)> onFault) -> decltype(body()) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (isGuestKernelEnabled()) {
    using Turn = _::GuestTurn<kj::Decay<Func>>;
    auto turn = kj::heap<Turn>(body);
    _::runTrampolineInGuest(js, &Turn::trampoline, turn.get(), kj::mv(onFault));
    _::throwIfGuestTurnFailed(*turn);
    if constexpr (!kj::isSameType<typename Turn::Result, void>()) {
      return kj::mv(KJ_ASSERT_NONNULL(turn->result.value));
    } else {
      return;
    }
  }
#endif
  return body();
}

}  // namespace workerd::jsg
