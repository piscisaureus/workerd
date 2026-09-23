// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "guest-run.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>

#include <kj/debug.h>

#include <atomic>

namespace workerd::jsg::_ {
namespace {

// Depth of guest-kernel turns on this thread. A nested turn already executes in the guest and
// must not call gk_run_here_user() again from inside it.
thread_local uint guestTurnDepth = 0;

// The arena the outermost guest turn on this thread entered the guest with. A nested turn runs
// inside that same guest entry, so it must belong to an isolate with the same arena: switching
// the guest's page-table root from inside the guest is not supported yet, and a nested turn for
// another isolate would run with that isolate's sandbox unaddressable. Such turns are detected
// and counted here rather than supported.
thread_local gk_arena* guestTurnArena = nullptr;
std::atomic<uint> nestedGuestTurnArenaMismatches{0};

gk_arena* guestArenaOf(Lock& js) {
  KJ_IF_SOME(arena, IsolateBase::from(js.v8Isolate).getGuestArena()) {
    return arena.get();
  }
  return nullptr;
}

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

  ++guestTurnDepth;
  KJ_DEFER(--guestTurnDepth);
  guestTurnArena = arena;
  KJ_DEFER(guestTurnArena = nullptr);

  long result = gk_run_here_user(fn, arg);
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

}  // namespace workerd::jsg::_
#endif  // WORKERD_HAS_GUEST_KERNEL
