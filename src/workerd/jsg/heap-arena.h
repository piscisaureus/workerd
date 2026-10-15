// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/common.h>
#include <kj/memory.h>
#include <kj/mutex.h>

// Per-isolate C++ heap arenas exist when workerd is built with both the guest kernel
// (WORKERD_HAS_GUEST_KERNEL, x86-64 Linux) and jemalloc as its allocator (WORKERD_HAS_JEMALLOC,
// --//src/workerd/server:allocator=jemalloc).
#if defined(WORKERD_HAS_GUEST_KERNEL) && defined(WORKERD_HAS_JEMALLOC)
#define WORKERD_HAS_HEAP_ARENA 1
#endif

namespace workerd::jsg {

class HeapArena;

#ifdef WORKERD_HAS_HEAP_ARENA
// A jemalloc arena whose memory is a region of one isolate's guest-kernel arena (see GuestArena
// in setup.h). While a thread is switched onto it (HeapArenaScope, which the isolate lock enters
// through GuestArenaScope), plain malloc() and operator new on that thread allocate from it, so
// the C++ objects created for the isolate under its lock -- V8's malloc'd per-turn state, JSG
// wrappers, IoOwn'd objects, the KJ allocations that carry request data -- live in the isolate's
// arena and are unaddressable from any other isolate's guest execution, like its V8 heap. What
// V8 allocates while creating the isolate, the isolate object included, stays in the shared heap
// (see newIsolate() in setup.c++).
//
// Memory layout and cross-arena frees. The arena is created with jemalloc's
// experimental.arenas_create_ext and metadata_use_hooks = false, so only the extents that hold
// allocated objects come from the isolate's arena (through the extent hooks, which carve them
// from the region): the arena's own bookkeeping -- the arena_t with its bins and locks, and the
// edata_t per extent, which holds the slab bitmap a free() marks -- is ordinary process memory,
// mapped under every page-table root, as is jemalloc's global radix tree that maps an address to
// its extent. A free() of an object in this arena therefore touches only shared memory and may
// run from any thread and under any isolate's lock, in the guest or not: jemalloc never reads or
// writes the freed object itself. This relies on the thread cache being disabled, see
// heapArenasAvailable(): with it, a freed pointer is handed back to the freeing thread's next
// allocation of that size whatever arena it belongs to.
//
// The region is handed out by a bump pointer and never returns memory: the dalloc, decommit and
// purge hooks refuse, and decay is off, so jemalloc keeps freed extents retained for reuse
// instead of calling the hooks on them at arbitrary times from arbitrary threads, where the
// arena may not be mapped. jemalloc calls the hooks with its own locks held, so they allocate
// nothing. Destroying the HeapArena discards every allocation still in it, and a later free()
// of one of them is undefined: nothing that outlives the isolate may be allocated under its
// lock without a SharedHeapScope (below).
class HeapArena final {
 public:
  // Creates an arena over [begin, end), page-aligned, reserved as inaccessible anonymous memory
  // inside the isolate's guest-kernel arena and used by nothing else.
  HeapArena(uintptr_t begin, uintptr_t end);
  ~HeapArena() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(HeapArena);

  // The jemalloc arena index.
  unsigned getIndex() const {
    return index;
  }
  uintptr_t getBegin() const {
    return begin;
  }
  uintptr_t getEnd() const {
    return end;
  }
  // Whether `p` lies in the arena's region.
  bool contains(const void* p) const {
    auto addr = reinterpret_cast<uintptr_t>(p);
    return addr >= begin && addr < end;
  }
  // How much of the region the arena has taken so far, which only grows.
  size_t getRegionBytes() const;

 private:
  struct Hooks;
  Hooks* hooks;  // jemalloc keeps a pointer to it for the arena's lifetime
  const uintptr_t begin;
  const uintptr_t end;
  // The start of the part of the region not yet handed to jemalloc.
  kj::MutexGuarded<uintptr_t> next;
  unsigned index;

  // Takes `size` bytes at `alignment` (both multiples of the page size, the latter a power of
  // two) from the region, made readable and writable, or returns null when the region is
  // exhausted. Allocates nothing: jemalloc calls it with its locks held.
  void* take(size_t size, size_t alignment);
};

// The jemalloc arena index the calling thread currently allocates from.
unsigned currentHeapArenaIndex();

// Whether `index` is one of jemalloc's automatic arenas, which every thread allocates from
// unless a HeapArenaScope is active on it.
bool isAutomaticHeapArena(unsigned index);
#endif  // WORKERD_HAS_HEAP_ARENA

// Makes `arena` the calling thread's jemalloc arena for the scope's lifetime and restores the
// thread's previous arena afterwards, so that scopes nest. A no-op for kj::none, which is all
// there is where heap arenas are not built in.
//
// The previous arena's index is kept in the scope object, which may live on the heap where guest
// execution can write it. It selects only which arena the thread's host-side allocations come
// from after the scope, never a page-table root: a rewritten index can misplace later host-side
// allocations (into another isolate's arena, where that isolate's turns can read them and other
// isolates' turns fault on them) but cannot make another isolate's memory addressable.
class HeapArenaScope final {
 public:
  explicit HeapArenaScope(kj::Maybe<HeapArena&> arena);
  ~HeapArenaScope() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(HeapArenaScope);

 private:
  bool switched = false;
  unsigned previousIndex = 0;
};

// Makes jemalloc's shared arena the calling thread's arena for the scope's lifetime and restores
// the thread's previous arena afterwards, so that it nests inside a HeapArenaScope: a thread
// that holds an isolate's lock allocates from the isolate's arena, and inside this scope from the
// shared heap. It is for memory that is not the isolate's although a turn of that isolate
// allocates it: process-wide state that is initialized or grown lazily while a turn runs (ICU's
// caches, the thread's timer queue), and objects handed from one isolate's turn to another's.
// Left in the isolate's arena, such memory is unaddressable from every other isolate's turn,
// which faults on it and is condemned.
//
// The shared arena is a compile-time constant -- jemalloc's arena 0, which exists from the
// allocator's initialization on and is one of the automatic arenas that every thread allocates
// from when no HeapArenaScope is active -- rather than a value read from memory, so that nothing
// guest execution can write selects what "shared" means. The arena to return to is kept in the
// scope object, with the same consequences as for HeapArenaScope.
//
// This is a correctness tool, and every use of it is a deliberate confidentiality concession:
// whatever is allocated under it lies in the shared heap, which every isolate's ring-3 execution
// can read (see the Limitations in experimental/guest-kernel/README.md). Keep its extent to what
// must be shared.
//
// A no-op where heap arenas are not built in, and where the allocator is not set up for them
// (heapArenasAvailable()), in which case every allocation is in the shared heap already.
class SharedHeapScope final {
 public:
  SharedHeapScope();
  ~SharedHeapScope() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SharedHeapScope);

 private:
  bool switched = false;
  unsigned previousIndex = 0;
};

// Whether the process allocator is set up for per-isolate heap arenas: workerd is built with
// jemalloc and jemalloc's thread cache is off process-wide. Heap arenas need the latter. A thread
// cache hands a freed pointer to that thread's next allocation of the same size whatever arena
// the pointer belongs to, so with caches on, a thread that freed one isolate's objects (the event
// loop between turns, a V8 worker thread) would place its next objects, for another isolate or
// for the runtime, in memory only the first isolate's guest execution can address. A constructor
// in heap-arena.c++ turns the cache off when WORKERD_EXPERIMENTAL_GUEST_KERNEL is set in the
// environment, before jemalloc initializes, in a binary that links jemalloc statically (workerd);
// a binary that links it as a shared library needs MALLOC_CONF=tcache:false in the environment.
// This reports whether either took effect. Always false where heap arenas are not built in.
bool heapArenasAvailable();

// At startup with guest-kernel isolation enabled: fails if the build has heap arenas but the
// allocator is not set up for them (see heapArenasAvailable()), and warns if it has none, so that
// an isolate's C++ objects stay in the shared heap.
void checkHeapArenaSupport();

}  // namespace workerd::jsg
