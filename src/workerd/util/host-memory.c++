// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "host-memory.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <cstring>
#include <new>

namespace workerd::hostMemory {

namespace {

// Zeroes a block in a way the compiler cannot elide: a freed block held a secret for as long as
// anything did, and the ordinary-heap fallback has no other scrub.
void scrub(void* ptr, size_t size) {
  memset(ptr, 0, size);
  __asm__ __volatile__("" : : "r"(ptr) : "memory");
}

// The size asked of gk_host_alloc for a block of `size` bytes aligned to `alignment`: gk aligns
// a block to its size class, so a small block that needs more alignment is asked for larger.
size_t gkSize(size_t size, size_t alignment) {
  return size < alignment ? alignment : size;
}

bool guestKernelActive() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  return gk_initialized() != 0;
#else
  return false;
#endif
}

}  // namespace

void* allocate(size_t size, size_t alignment) {
  KJ_REQUIRE(size > 0);
  KJ_REQUIRE(alignment <= 4096);
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    // A block's alignment is the smaller of its size rounded up to a power of two and 4 KiB, so
    // asking for at least `alignment` bytes is enough (see gkSize).
    void* ptr = gk_host_alloc(gkSize(size, alignment));
    KJ_REQUIRE(ptr != nullptr, "guest-kernel: out of host memory", size);
    return ptr;
  }
#endif
  void* ptr = ::operator new(size, std::align_val_t(alignment));
  memset(ptr, 0, size);
  return ptr;
}

void deallocate(void* ptr, size_t size, size_t alignment) {
  if (ptr == nullptr) return;
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    // gk_host_free scrubs the block itself, and refuses a pointer that is not one of its blocks.
    gk_host_free(ptr, gkSize(size, alignment));
    return;
  }
#endif
  scrub(ptr, size);
  ::operator delete(ptr, std::align_val_t(alignment));
}

namespace {
const void* probeTarget = nullptr;
}  // namespace

void setProbeTargetForTest(const void* target) {
  probeTarget = target;
}

kj::Maybe<const void*> probeTargetForTest() {
  if (probeTarget == nullptr) return kj::none;
  return probeTarget;
}

bool contains(const void* ptr) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  return guestKernelActive() && gk_host_owns(ptr) != 0;
#else
  return false;
#endif
}

}  // namespace workerd::hostMemory
