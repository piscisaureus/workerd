// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "host-memory.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <kj/mutex.h>

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

// The handle table used when the guest kernel is not active: the same shape and the same checks
// as gk's (see gk_host_slot_alloc in gk.h), over blocks from allocate(). A handle is the slot's
// index in its low 16 bits and the generation it was allocated with above them; a generation is
// taken from a counter every allocation advances, so a reused slot never repeats a handle.
constexpr unsigned SLOT_INDEX_BITS = 16;
static_assert(SLOT_CAPACITY <= (size_t{1} << SLOT_INDEX_BITS));

struct FallbackSlot {
  void* ptr = nullptr;
  size_t size = 0;
  size_t alignment = 0;
  uint64_t gen = 0;
  uint32_t nextFree = 0;  // free-list link, as index + 1; 0 ends the list
  bool live = false;
};

struct FallbackTable {
  FallbackSlot slots[SLOT_CAPACITY];
  size_t taken = 0;       // slots ever taken
  uint32_t freeHead = 0;  // free-list head, as index + 1; 0 when empty
  uint64_t gen = 0;

  FallbackSlot* lookup(SlotHandle handle) {
    uint64_t idx = handle & ((uint64_t{1} << SLOT_INDEX_BITS) - 1);
    uint64_t gen = handle >> SLOT_INDEX_BITS;
    if (idx >= SLOT_CAPACITY) return nullptr;
    FallbackSlot& slot = slots[idx];
    if (!slot.live || slot.gen != gen) return nullptr;
    return &slot;
  }
};

kj::MutexGuarded<FallbackTable>& fallbackTable() {
  static kj::MutexGuarded<FallbackTable> table;
  return table;
}

SlotHandle fallbackAllocateSlot(size_t size, size_t alignment) {
  auto lock = fallbackTable().lockExclusive();
  uint32_t idx;
  if (lock->freeHead != 0) {
    idx = lock->freeHead - 1;
    lock->freeHead = lock->slots[idx].nextFree;
  } else {
    KJ_REQUIRE(lock->taken < SLOT_CAPACITY, "host slot table is full", SLOT_CAPACITY);
    idx = lock->taken++;
  }
  void* ptr;
  KJ_ON_SCOPE_FAILURE({
    lock->slots[idx].nextFree = lock->freeHead;
    lock->freeHead = idx + 1;
  });
  ptr = allocate(size, alignment);
  uint64_t gen = ++lock->gen;
  lock->slots[idx] = FallbackSlot{
    .ptr = ptr, .size = size, .alignment = alignment, .gen = gen, .nextFree = 0, .live = true};
  return (gen << SLOT_INDEX_BITS) | idx;
}

void* fallbackResolveSlot(SlotHandle handle) {
  auto lock = fallbackTable().lockExclusive();
  FallbackSlot* slot = lock->lookup(handle);
  if (slot == nullptr) {
    KJ_LOG(ERROR, "host slot handle does not name a live slot; it resolves to nothing", handle);
    return nullptr;
  }
  return slot->ptr;
}

void fallbackFreeSlot(SlotHandle handle) {
  auto lock = fallbackTable().lockExclusive();
  FallbackSlot* slot = lock->lookup(handle);
  if (slot == nullptr) {
    KJ_LOG(ERROR, "host slot handle does not name a live slot; nothing is freed", handle);
    return;
  }
  void* ptr = slot->ptr;
  size_t size = slot->size;
  size_t alignment = slot->alignment;
  uint32_t idx = slot - lock->slots;
  slot->ptr = nullptr;
  slot->size = 0;
  slot->alignment = 0;
  slot->live = false;
  slot->nextFree = lock->freeHead;
  lock->freeHead = idx + 1;
  deallocate(ptr, size, alignment);
}

}  // namespace

SlotHandle allocateSlot(size_t size, size_t alignment) {
  KJ_REQUIRE(size > 0);
  KJ_REQUIRE(alignment <= 4096);
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    static_assert(SLOT_CAPACITY == GK_HOST_SLOTS, "the fallback table mirrors gk's capacity");
    SlotHandle handle = gk_host_slot_alloc(gkSize(size, alignment));
    KJ_REQUIRE(handle != 0, "guest-kernel: out of host slots or host memory", size);
    return handle;
  }
#endif
  return fallbackAllocateSlot(size, alignment);
}

void* resolveSlot(SlotHandle handle) {
  if (handle == 0) return nullptr;
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    return gk_host_slot_get(handle);
  }
#endif
  return fallbackResolveSlot(handle);
}

void freeSlot(SlotHandle handle) {
  if (handle == 0) return;
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    gk_host_slot_free(handle);
    return;
  }
#endif
  fallbackFreeSlot(handle);
}

namespace {

// The registrations are few and named by string literals, so a short fixed table suffices.
constexpr size_t MAX_PROBES = 4;

struct ProbeTarget {
  kj::StringPtr name;
  const void* target = nullptr;
};
ProbeTarget probeTargets[MAX_PROBES];

struct HostUse {
  kj::StringPtr name;
  kj::Maybe<kj::Function<void()>> use;  // none marks a free entry
};
kj::ArrayPtr<HostUse> hostUses() {
  static HostUse uses[MAX_PROBES];
  return uses;
}

kj::Maybe<HandleProbeForTest>& handleProbe() {
  static kj::Maybe<HandleProbeForTest> probe;
  return probe;
}

}  // namespace

void setProbeTargetForTest(kj::StringPtr name, const void* target) {
  for (auto& entry: probeTargets) {
    if (entry.target == nullptr || entry.name == name) {
      entry = {.name = name, .target = target};
      return;
    }
  }
  KJ_FAIL_REQUIRE("too many probe targets registered", name);
}

kj::Maybe<const void*> probeTargetForTest(kj::StringPtr name) {
  for (auto& entry: probeTargets) {
    if (entry.target != nullptr && entry.name == name) return entry.target;
  }
  return kj::none;
}

void setHandleProbeForTest(HandleProbeForTest probe) {
  handleProbe() = kj::mv(probe);
}

kj::Maybe<HandleProbeForTest&> handleProbeForTest() {
  return handleProbe();
}

void setHostUseForTest(kj::StringPtr name, kj::Function<void()> use) {
  for (auto& entry: hostUses()) {
    if (entry.use == kj::none || entry.name == name) {
      entry = HostUse{.name = name, .use = kj::mv(use)};
      return;
    }
  }
  KJ_FAIL_REQUIRE("too many host uses registered", name);
}

kj::Maybe<kj::Function<void()>&> hostUseForTest(kj::StringPtr name) {
  for (auto& entry: hostUses()) {
    if (entry.use != kj::none && entry.name == name) return entry.use;
  }
  return kj::none;
}

bool contains(const void* ptr) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  return guestKernelActive() && gk_host_owns(ptr) != 0;
#else
  return false;
#endif
}

}  // namespace workerd::hostMemory
