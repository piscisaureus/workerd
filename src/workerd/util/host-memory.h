// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/util/memory-class.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/function.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace workerd {

// Host memory: memory for runtime state that only host code touches, which no guest page-table
// root maps (memory class H in src/workerd/experimental/guest-kernel/memory-classes.md).
//
// With guest-kernel isolation compiled in and active (gk_init() has run, which workerd does
// under WORKERD_EXPERIMENTAL_GUEST_KERNEL before any isolate exists), the bytes come from
// gk_host_alloc(): a JS turn at guest ring 3, and any escape from one with a read or write
// primitive, cannot reach them -- a guest access faults at the accessed address rather than
// reading zeroes -- while host code reads and writes them as ordinary memory. Without the guest
// kernel (another platform, or the environment variable unset) the bytes come from the ordinary
// heap and everything here behaves the same, so code that uses host memory reads identically in
// both builds and is exercised by the ordinary test suite.
//
// Allocation is host-side only, and never on a path the guest kernel calls from a fault or
// syscall handler. It may throw (out of host memory); it never silently hands out ordinary
// memory when the guest kernel is active. Memory is zero-filled when allocated and scrubbed when
// freed.
namespace hostMemory {

// A zero-filled block of `size` bytes, aligned to at least `alignment` (at most 4 KiB).
void* allocate(size_t size, size_t alignment);

// Returns a block from allocate(), with the same `size` and `alignment`. The block is scrubbed
// first.
void deallocate(void* ptr, size_t size, size_t alignment);

// Whether `ptr` lies in guest-kernel host memory. False whenever the guest kernel is not active,
// including when `ptr` came from allocate(), since it is then ordinary heap memory.
bool contains(const void* ptr);

// Host slots: a block of host memory named by a handle rather than a pointer, so that the owner
// keeps nothing outside host memory that the host would dereference. The table that maps handles
// to blocks is the guest kernel's (gk_host_slot_alloc in experimental/guest-kernel/gk.h): it
// lives in gk's supervisor control block, beyond a guest's reach, has a fixed capacity of
// SLOT_CAPACITY entries, and validates a handle completely -- bounds, liveness and generation --
// before resolving it, reading nothing through the handle's value. Without the guest kernel a
// table of the same shape and the same checks backs the handles on the ordinary heap, so the
// handle semantics, including the refusal of a stale or foreign handle, are the same in both
// builds and are exercised by the ordinary test suite.
//
// A handle is 0 only when it names nothing. allocateSlot() throws when the table or host memory
// is exhausted. resolveSlot() returns nullptr for any handle that does not name a live slot --
// a freed one, including after the slot is reused, or a value that never came from the table --
// and the guest kernel reports such a handle on stderr. freeSlot() refuses such a handle the
// same way and frees nothing. The pointer resolveSlot() returns is for immediate use; it is
// resolved again for each use rather than kept.
using SlotHandle = uint64_t;
constexpr size_t SLOT_CAPACITY = 4096;
SlotHandle allocateSlot(size_t size, size_t alignment);
void* resolveSlot(SlotHandle handle);
void freeSlot(SlotHandle handle);

// Test-only hooks for the guest kernel's end-to-end check (experimental/guest-kernel/
// host-memory-test.sh). A tenant registers itself only when WORKERD_GK_TEST_PROBE is in the
// environment; there is none otherwise.
//
// The probe target is the address of one host-memory tenant, which a request to
// /__gk_probe_host reads from inside a JS turn (see maybeForceGuestFaultForTest in
// api/global-scope.c++), expecting the guest fault a ring-3 read of host memory takes.
void setProbeTargetForTest(const void* target);
kj::Maybe<const void*> probeTargetForTest();

// The handle probe is where the tenant's owner keeps the handle that names the tenant (the
// sizeof(SlotHandle) bytes a HostSlot occupies) together with a host-side use of the tenant
// through that handle, which logs whether the use resolved the tenant and at what address or was
// refused. A request to /__gk_corrupt_handle overwrites the handle from inside a JS turn with the
// address of a buffer the turn chose, as a write primitive would a pointer, then runs the use on
// the event loop: it must be refused rather than reach the buffer. The handle is restored and
// the use run again afterwards.
struct HandleProbeForTest {
  void* handleStorage;
  kj::Function<void()> useOnHost;
};
void setHandleProbeForTest(HandleProbeForTest probe);
kj::Maybe<HandleProbeForTest&> handleProbeForTest();

}  // namespace hostMemory

// A T constructed in host memory, owned like a kj::Own: created by hostOwned<T>(...), destroyed
// (and its block scrubbed and freed) when the HostOwn goes away. Move-only.
//
// What this protects is the object's bytes: with the guest kernel active no guest access reaches
// them. The pointer to the object lives wherever the HostOwn itself lives, and the host
// dereferences it, so by the provenance rule in memory-classes.md the HostOwn belongs in a Host
// object too -- a HostOwn held in ordinary heap memory keeps the guest from reading the object,
// but not from swapping the pointer for one of its choosing. Freeing a swapped pointer is
// refused by the guest kernel as a fatal integrity error rather than written through.
template <typename T>
class WD_MEMORY_CLASS_HOST HostOwn {
 public:
  HostOwn(): ptr(nullptr) {}
  HostOwn(decltype(nullptr)): ptr(nullptr) {}
  HostOwn(HostOwn&& other) noexcept: ptr(other.ptr) {
    other.ptr = nullptr;
  }
  HostOwn& operator=(HostOwn&& other) noexcept {
    if (this != &other) {
      reset();
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }
  ~HostOwn() noexcept(false) {
    reset();
  }
  KJ_DISALLOW_COPY(HostOwn);

  T* get() {
    return ptr;
  }
  const T* get() const {
    return ptr;
  }
  T& operator*() {
    return *checked();
  }
  const T& operator*() const {
    return *checked();
  }
  T* operator->() {
    return checked();
  }
  const T* operator->() const {
    return checked();
  }
  bool operator==(decltype(nullptr)) const {
    return ptr == nullptr;
  }

 private:
  T* ptr;

  explicit HostOwn(T* ptr): ptr(ptr) {}

  T* checked() const {
    KJ_REQUIRE(ptr != nullptr, "HostOwn is empty");
    return ptr;
  }

  void reset() {
    if (ptr != nullptr) {
      T* p = ptr;
      ptr = nullptr;
      p->~T();
      hostMemory::deallocate(p, sizeof(T), alignof(T));
    }
  }

  template <typename U, typename... Params>
  friend HostOwn<U> hostOwned(Params&&... params);
};

// Constructs a T in host memory, as kj::heap<T>(...) would on the heap.
template <typename T, typename... Params>
HostOwn<T> hostOwned(Params&&... params) {
  static_assert(alignof(T) <= 4096, "host memory blocks are aligned to at most 4 KiB");
  void* mem = hostMemory::allocate(sizeof(T), alignof(T));
  bool constructed = false;
  KJ_DEFER({
    if (!constructed) hostMemory::deallocate(mem, sizeof(T), alignof(T));
  });
  T* obj = new (mem) T(kj::fwd<Params>(params)...);
  constructed = true;
  return HostOwn<T>(obj);
}

// A T constructed in host memory and owned through a host slot handle (hostMemory::allocateSlot)
// rather than a pointer: created by hostSlot<T>(...), destroyed (and its block scrubbed and
// freed) when the HostSlot goes away. Move-only. This is the form a host-memory tenant takes when
// its owner is not itself in host memory: the handle is the HostSlot's only member, so the owner
// holds nothing the host dereferences, and every access resolves the handle through the guest
// kernel's table, which checks it against the table alone. A handle the guest has rewritten
// resolves to nothing, and an access through it throws rather than reading or writing wherever
// the rewritten value pointed; the destructor then frees nothing. What remains is the ownership
// bound the table cannot close: a handle rewritten to another live tenant's handle names that
// tenant's object. The HostOwn above is the cheaper choice for an owner that is itself in host
// memory, where the pointer is as unreachable as the object.
template <typename T>
class WD_MEMORY_CLASS_HOST HostSlot {
 public:
  HostSlot(): handle(0) {}
  HostSlot(decltype(nullptr)): handle(0) {}
  HostSlot(HostSlot&& other) noexcept: handle(other.handle) {
    other.handle = 0;
  }
  HostSlot& operator=(HostSlot&& other) noexcept {
    if (this != &other) {
      reset();
      handle = other.handle;
      other.handle = 0;
    }
    return *this;
  }
  ~HostSlot() noexcept(false) {
    reset();
  }
  KJ_DISALLOW_COPY(HostSlot);

  // The object, or nullptr when the HostSlot is empty or its handle no longer names a live slot.
  T* get() {
    return resolve();
  }
  const T* get() const {
    return resolve();
  }
  T& operator*() {
    return *checked();
  }
  const T& operator*() const {
    return *checked();
  }
  T* operator->() {
    return checked();
  }
  const T* operator->() const {
    return checked();
  }
  bool operator==(decltype(nullptr)) const {
    return handle == 0;
  }

 private:
  hostMemory::SlotHandle handle;

  explicit HostSlot(hostMemory::SlotHandle handle): handle(handle) {}

  T* resolve() const {
    if (handle == 0) return nullptr;
    return static_cast<T*>(hostMemory::resolveSlot(handle));
  }

  T* checked() const {
    KJ_REQUIRE(handle != 0, "HostSlot is empty");
    T* obj = static_cast<T*>(hostMemory::resolveSlot(handle));
    if (obj == nullptr) {
      KJ_FAIL_REQUIRE("HostSlot handle does not name a live host slot", kj::hex(handle));
    }
    return obj;
  }

  void reset() {
    if (handle != 0) {
      hostMemory::SlotHandle h = handle;
      handle = 0;
      // A handle that no longer resolves names nothing of ours: the destructor is not run and
      // freeSlot() refuses the handle, so nothing is written through it.
      T* obj = static_cast<T*>(hostMemory::resolveSlot(h));
      if (obj != nullptr) obj->~T();
      hostMemory::freeSlot(h);
    }
  }

  template <typename U, typename... Params>
  friend HostSlot<U> hostSlot(Params&&... params);
};
static_assert(sizeof(HostSlot<int>) == sizeof(hostMemory::SlotHandle),
    "a HostSlot holds the handle and nothing else");

// Constructs a T in host memory owned through a host slot, as hostOwned<T>(...) does through a
// pointer.
template <typename T, typename... Params>
HostSlot<T> hostSlot(Params&&... params) {
  static_assert(alignof(T) <= 4096, "host memory blocks are aligned to at most 4 KiB");
  hostMemory::SlotHandle handle = hostMemory::allocateSlot(sizeof(T), alignof(T));
  bool constructed = false;
  KJ_DEFER({
    if (!constructed) hostMemory::freeSlot(handle);
  });
  void* mem = hostMemory::resolveSlot(handle);
  KJ_ASSERT(mem != nullptr, "a fresh host slot handle must resolve");
  new (mem) T(kj::fwd<Params>(params)...);
  constructed = true;
  return HostSlot<T>(handle);
}

}  // namespace workerd
