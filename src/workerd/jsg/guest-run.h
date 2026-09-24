// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

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
// That confines it to this isolate's arena and keeps it out of gk's control state, but not out
// of the shared runtime memory (the C++/KJ heap, glibc, and this thread's stack, which `body`
// runs on and which holds the host frames below the guest boundary): those are user-mapped, so
// an escape to arbitrary code execution can still corrupt shared state and, through a host
// return address on the stack, potentially reach host execution. See gk.h; containing that is
// remaining work.
//
// The guest boundary is not a C++ call frame, so an exception thrown inside `body` cannot unwind
// across it. The trampoline that runs `body` in the guest catches everything and records it,
// together with `body`'s return value, in a heap-allocated turn record that the host side reads
// back once the guest has returned, re-throwing the exception so that callers observe the same
// exceptions as without the guest.
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
// TODO(guest-kernel): RAII state held by the abandoned frames leaks; there is no bookkeeping
//   that could release it. Known leaks per faulted turn:
//   - for a JS turn, the IoContext::PendingEvent that IoContext::runImpl's turn registers, so
//     that IoContext is never seen as idle by hang detection (moot once it is aborted and
//     destroyed), and the limit enforcer's enterJs() scope, so it never sees the matching exit;
//   - any kj::Own / jsg::Ref / IoOwn / file descriptor / KJ mutex lock the C++ frames held at
//     the moment of the fault (a held KJ mutex would deadlock its next locker). The test hook
//     in ServiceWorkerGlobalScope::request() faults before any such state is taken, so the test
//     does not exercise that case;
//   - on the V8 side, the abandoned HandleScopes' handles, and the isolate itself, which is
//     never freed because nothing evicts a condemned isolate (see Worker::Isolate::condemn).
template <typename Func>
auto runInGuest(Lock& js,
    Func&& body,
    kj::FunctionParam<void(const kj::Exception&)> onFault) -> decltype(body());

#ifdef WORKERD_HAS_GUEST_KERNEL
namespace _ {  // private

// What `body` did, written in-guest by the trampoline and read host-side after the guest
// returns. Lives on the heap: see the host-frame wall above.
struct GuestTurnOutcome {
  kj::Maybe<kj::Exception> exception;
  bool jsExceptionThrown = false;
};

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
    } catch (const JsExceptionThrown&) {
      // The pending exception stays on the isolate; only the C++ signal must be re-raised.
      turn.jsExceptionThrown = true;
    } catch (...) {
      turn.exception = kj::getCaughtExceptionAsKj();
    }
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
    if (turn->jsExceptionThrown) {
      throw JsExceptionThrown();
    }
    KJ_IF_SOME(e, turn->exception) {
      kj::throwFatalException(kj::mv(e));
    }
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
