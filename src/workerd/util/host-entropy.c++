// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "host-entropy.h"

#include <workerd/util/host-memory.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <kj/debug.h>
#include <kj/mutex.h>

#include <cstdlib>

#ifdef __unix__
#include <unistd.h>
#endif

namespace workerd {

namespace {

// Everything a draw reads or writes, in one host-memory block: the unconsumed bytes, the cursor,
// and the lock that serializes draws, so that no part of it is in memory a guest can reach.
struct WD_MEMORY_CLASS_HOST State {
  kj::byte store[hostEntropy::BUFFER_SIZE];
  // The unconsumed bytes are the last `remaining` of `store`.
  size_t remaining = 0;
#ifdef __unix__
  // The process that filled the buffer; a draw from another one (a fork) discards it.
  pid_t pid = 0;
#endif
};

kj::Maybe<kj::Function<void(kj::ArrayPtr<kj::byte>)>>& sourceForTest() {
  static kj::Maybe<kj::Function<void(kj::ArrayPtr<kj::byte>)>> source;
  return source;
}

void fill(kj::ArrayPtr<kj::byte> buffer) {
  KJ_IF_SOME(source, sourceForTest()) {
    source(buffer);
    return;
  }
  static_assert(hostEntropy::BUFFER_SIZE <= 256, "the system generator serves at most 256 bytes");
  RAND_get_system_entropy_for_custom_prng(buffer.begin(), buffer.size());
}

// The block, allocated on the first draw. The static holds only the handle, which the guest
// kernel validates against its own table on every use (see HostSlot in host-memory.h); the
// host dereferences nothing kept here. The first draw is host-side and after the guest kernel
// is initialized, so the block is host memory whenever there is a guest at all.
kj::MutexGuarded<State>& state() {
  static HostSlot<kj::MutexGuarded<State>> slot = []() {
    auto slot = hostSlot<kj::MutexGuarded<State>>();
    if (getenv("WORKERD_GK_TEST_PROBE") != nullptr) {
      // Test-only (see hostMemory::setProbeTargetForTest): publish where the buffer lives, so the
      // guest kernel's end-to-end check can read it from inside a JS turn and expect the fault.
      const void* buffer = slot->getWithoutLock().store;
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: host entropy buffer",
          kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(buffer))),
          hostMemory::contains(buffer));
      hostMemory::setProbeTargetForTest("host-entropy", buffer);
    }
    return slot;
  }();
  return *slot;
}

}  // namespace

void getHostEntropy(kj::ArrayPtr<kj::byte> output) {
  auto lock = state().lockExclusive();

#ifdef __unix__
  pid_t pid = getpid();
  if (lock->pid != pid) {
    lock->pid = pid;
    lock->remaining = 0;
  }
#endif

  while (output.size() > 0) {
    if (lock->remaining == 0) {
      fill(kj::arrayPtr(lock->store));
      lock->remaining = hostEntropy::BUFFER_SIZE;
    }
    kj::ArrayPtr<kj::byte> unconsumed =
        kj::arrayPtr(lock->store).slice(hostEntropy::BUFFER_SIZE - lock->remaining);
    size_t n = kj::min(unconsumed.size(), output.size());
    output.write(unconsumed.first(n));  // advances `output` past the written bytes
    OPENSSL_cleanse(unconsumed.begin(), n);
    lock->remaining -= n;
  }
}

namespace hostEntropy {

void setSourceForTest(kj::Function<void(kj::ArrayPtr<kj::byte>)> source) {
  sourceForTest() = kj::mv(source);
}

void clearSourceForTest() {
  sourceForTest() = kj::none;
}

void discardForTest() {
  auto lock = state().lockExclusive();
  OPENSSL_cleanse(lock->store, BUFFER_SIZE);
  lock->remaining = 0;
}

}  // namespace hostEntropy

}  // namespace workerd
