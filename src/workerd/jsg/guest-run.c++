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
