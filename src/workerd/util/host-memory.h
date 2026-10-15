// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/util/memory-class.h>

#include <kj/common.h>
#include <kj/debug.h>

#include <cstddef>
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

// Test-only hook for the guest kernel's end-to-end check (experimental/guest-kernel/
// host-memory-test.sh): the address of one host-memory tenant, which a request to
// /__gk_probe_host reads from inside a JS turn (see maybeForceGuestFaultForTest in
// api/global-scope.c++), expecting the guest fault a ring-3 read of host memory takes. A tenant
// registers itself only when WORKERD_GK_TEST_PROBE is in the environment; there is none otherwise.
void setProbeTargetForTest(const void* target);
kj::Maybe<const void*> probeTargetForTest();

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

}  // namespace workerd
