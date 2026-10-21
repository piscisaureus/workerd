// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "thread-scopes.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <kj/debug.h>

#include <atomic>
#include <cstdlib>

namespace workerd {

using kj::uint;

namespace {

thread_local uint allowV8BackgroundThreadScopeCount = 0;

bool multiTenantProcess = false;
bool predictableMode = false;
bool gcStressMode = false;

// This variable is read in signal handlers, so use atomic stores and compiler barriers as
// needed in regular code. Atomic loads are unnecessary, because we're not synchronizing with
// other threads. Used only without the guest kernel; see the class comment in thread-scopes.h.
thread_local ThreadProgressCounter* activeProgressCounter = nullptr;

#ifdef WORKERD_HAS_GUEST_KERNEL
// The guest kernel's thread words that hold the active counter's state (see thread-scopes.h):
// the keys this file owns of the GK_THREAD_WORDS per thread. Kept adjacent so that one call
// reads or writes all three. Any other user of thread words takes keys from PROGRESS_END up.
enum ProgressWord : unsigned {
  PROGRESS_SELF = 0,     // the active ThreadProgressCounter, for identity only, never dereferenced
  PROGRESS_COUNTER = 1,  // the address of its uint64_t
  PROGRESS_SAVED = 2,    // the value saved at construction or acknowledgeProgress()
  PROGRESS_END = 3,
};
static_assert(PROGRESS_END <= GK_THREAD_WORDS);

struct ProgressWords {
  uint64_t self = 0;
  uint64_t counter = 0;
  uint64_t saved = 0;
};

// Host-side: whether the state is in thread words. A word in supervisor memory decides, not a
// heap flag a turn could flip to send the signal handler back to the thread-local.
bool progressInThreadWords() {
  return gk_initialized();
}

ProgressWords readProgressWords() {
  ProgressWords w;
  uint64_t v[3] = {0, 0, 0};
  if (gk_thread_words_get(PROGRESS_SELF, 3, v) == 0) {
    w.self = v[0];
    w.counter = v[1];
    w.saved = v[2];
  }
  return w;
}

void writeProgressWords(const ProgressWords& w) {
  uint64_t v[3] = {w.self, w.counter, w.saved};
  KJ_ASSERT(gk_thread_words_set(PROGRESS_SELF, 3, v) == 0);
}
#endif

}  // namespace

AllowV8BackgroundThreadsScope::AllowV8BackgroundThreadsScope() {
  ++allowV8BackgroundThreadScopeCount;
}

AllowV8BackgroundThreadsScope::~AllowV8BackgroundThreadsScope() noexcept(false) {
  --allowV8BackgroundThreadScopeCount;
}

bool AllowV8BackgroundThreadsScope::isActive() {
  return allowV8BackgroundThreadScopeCount > 0;
}

bool isMultiTenantProcess() {
  return multiTenantProcess;
}

void setMultiTenantProcess() {
  multiTenantProcess = true;
}

bool isPredictableModeForTest() {
  return predictableMode;
}

void setPredictableModeForTest() {
  predictableMode = true;
}

bool isGcStressModeForTest() {
  // Also honor the WORKERD_GC_STRESS environment variable so that gc-stress mode can be enabled
  // in binaries that don't have a --gc-stress CLI flag (e.g., edgeworker's prod subcommand).
  // The env var is checked once and cached; thread-safe via C++ static local initialization.
  static const bool fromEnv = [] {
    const char* val = std::getenv("WORKERD_GC_STRESS");
    return val != nullptr && val[0] != '0' && val[0] != '\0';
  }();
  return gcStressMode || fromEnv;
}

void setGcStressModeForTest() {
  gcStressMode = true;
}

#ifdef WORKERD_ASAN
bool isAllocGcStressModeForTest() {
  // Honor the WORKERD_GC_STRESS_ALLOC environment variable. Checked once and cached; thread-safe
  // via C++ static local initialization.
  static const bool fromEnv = [] {
    const char* val = std::getenv("WORKERD_GC_STRESS_ALLOC");
    return val != nullptr && val[0] != '0' && val[0] != '\0';
  }();
  return fromEnv;
}
#endif

ThreadProgressCounter::ThreadProgressCounter(uint64_t& counter)
    : savedValue(__atomic_load_n(&counter, __ATOMIC_RELAXED)),
      counter(counter) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  // Inside a turn (as gk's own hint says; a lie costs only the liar, since the branch below
  // reads supervisor memory and faults at ring 3) this is a reentrant counter under the lock
  // the turn runs on, which holds the active one.
  if (gk_in_guest_hint()) return;
  if (progressInThreadWords()) {
    if (readProgressWords().self == 0) {
      writeProgressWords({.self = reinterpret_cast<uint64_t>(this),
        .counter = reinterpret_cast<uint64_t>(&counter),
        .saved = savedValue});
    }
    return;
  }
#endif
  if (activeProgressCounter == nullptr) {
    // Release compiler barrier guarantees we're initialized before signal handlers can see us.
    std::atomic_signal_fence(std::memory_order_release);
    __atomic_store_n(&activeProgressCounter, this, __ATOMIC_RELAXED);
  } else {
    // Another progress counter is active on this thread, likely meaning we reentered.
  }
}

ThreadProgressCounter::~ThreadProgressCounter() noexcept(false) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (gk_in_guest_hint()) return;
  if (progressInThreadWords()) {
    auto words = readProgressWords();
    KJ_ASSERT(words.self != 0, "~ProgressCounter() with no active progress counter.");
    if (words.self == reinterpret_cast<uint64_t>(this)) writeProgressWords({});
    return;
  }
#endif
  auto& self = KJ_ASSERT_NONNULL(
      activeProgressCounter, "~ProgressCounter() with no active progress counter.");
  if (&self == this) {
    // Acquire compiler barrier to prevent any teardown from leaking above this nullification.
    KJ_DEFER({
      __atomic_store_n(&activeProgressCounter, nullptr, __ATOMIC_RELAXED);
      std::atomic_signal_fence(std::memory_order_acquire);
    });
  } else {
    // Nothing to do, tearing down reentered progress counter.
  }
}

bool ThreadProgressCounter::hasProgress() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (progressInThreadWords()) {
    auto words = readProgressWords();
    if (words.self == 0) return false;
    // The address came from supervisor memory, so it is the one the host stored; what it
    // points at is the isolate's counter in the heap, read and compared only.
    auto currentValue =
        __atomic_load_n(reinterpret_cast<const uint64_t*>(words.counter), __ATOMIC_RELAXED);
    return words.saved != currentValue;
  }
#endif
  KJ_IF_SOME(progressCounter, activeProgressCounter) {
    // The counter itself may be incremented by any thread, but there's no real synchronization
    // concern, so we can use relaxed memory ordering. If the machine is so bogged down that a
    // stale value causes a false positive, then crashing seems reasonable.
    auto currentValue = __atomic_load_n(&progressCounter.counter, __ATOMIC_RELAXED);

    // `savedValue` is only ever accessed by our own thread, so no need for atomics here.
    if (progressCounter.savedValue != currentValue) {
      return true;
    }
  }

  return false;
}

void ThreadProgressCounter::acknowledgeProgress() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (progressInThreadWords()) {
    auto words = readProgressWords();
    if (words.self == 0) return;
    words.saved =
        __atomic_load_n(reinterpret_cast<const uint64_t*>(words.counter), __ATOMIC_RELAXED);
    writeProgressWords(words);
    return;
  }
#endif
  KJ_IF_SOME(progressCounter, activeProgressCounter) {
    progressCounter.savedValue = __atomic_load_n(&progressCounter.counter, __ATOMIC_RELAXED);
  }
}

// ======================================================================================

namespace {
thread_local uint warnAboutIsolateLockScopeCount = 0;
}  // namespace

WarnAboutIsolateLockScope::WarnAboutIsolateLockScope() {
  ++warnAboutIsolateLockScopeCount;
}

WarnAboutIsolateLockScope::~WarnAboutIsolateLockScope() noexcept(false) {
  if (!released) release();
}

WarnAboutIsolateLockScope::WarnAboutIsolateLockScope(WarnAboutIsolateLockScope&& other)
    : released(other.released) {
  other.released = true;
}

void WarnAboutIsolateLockScope::release() {
  if (!released) {
    --warnAboutIsolateLockScopeCount;
    released = true;
  }
}

void WarnAboutIsolateLockScope::maybeWarn() {
  if (warnAboutIsolateLockScopeCount > 0) {
    KJ_LOG(WARNING, "taking isolate lock at a bad time", kj::getStackTrace());
  }
}

ThreadScopeCounts saveThreadScopeCountsForGuestTurn() {
  return {
    .allowV8BackgroundThreads = allowV8BackgroundThreadScopeCount,
    .warnAboutIsolateLock = warnAboutIsolateLockScopeCount,
  };
}

void restoreThreadScopeCountsAfterGuestTurn(ThreadScopeCounts counts) {
  allowV8BackgroundThreadScopeCount = counts.allowV8BackgroundThreads;
  warnAboutIsolateLockScopeCount = counts.warnAboutIsolateLock;
}

}  // namespace workerd
