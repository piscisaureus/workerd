// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "heap-arena.h"

#include <kj/debug.h>

#ifdef WORKERD_HAS_JEMALLOC
#include <jemalloc/jemalloc.h>

#include <cstdlib>
#endif

#ifdef WORKERD_HAS_HEAP_ARENA
#include <sys/mman.h>

#include <kj/string.h>

#include <cerrno>
#endif

namespace workerd::jsg {

#ifdef WORKERD_HAS_JEMALLOC
namespace {

// With guest-kernel isolation requested, turns the thread cache off process-wide (see
// heapArenasAvailable()) through jemalloc's in-binary configuration string. It takes effect when
// this runs before jemalloc initializes, which holds for a statically linked binary such as
// workerd: jemalloc's own constructor and the first malloc() of the program's static
// initialization both come later in the initialization order than a constructor with an explicit
// priority. In a binary that links jemalloc as a shared library (the test binaries), jemalloc
// initializes with that library's constructors, before this one can run, and MALLOC_CONF in the
// environment is what configures it. MALLOC_CONF takes precedence over this string either way,
// and checkHeapArenaSupport() catches a configuration that leaves the cache on.
__attribute__((constructor(101))) void configureAllocatorForGuestKernel() {
  if (getenv("WORKERD_EXPERIMENTAL_GUEST_KERNEL") != nullptr) {
    malloc_conf = "tcache:false";
  }
}

bool threadCacheEnabled() {
  bool enabled = true;
  size_t size = sizeof(enabled);
  if (mallctl("opt.tcache", &enabled, &size, nullptr, 0) != 0) return true;
  return enabled;
}

}  // namespace
#endif  // WORKERD_HAS_JEMALLOC

bool heapArenasAvailable() {
#ifdef WORKERD_HAS_HEAP_ARENA
  static const bool available = !threadCacheEnabled();
  return available;
#else
  return false;
#endif
}

void checkHeapArenaSupport() {
#ifdef WORKERD_HAS_HEAP_ARENA
  KJ_REQUIRE(heapArenasAvailable(),
      "guest-kernel: per-isolate heap arenas need jemalloc's thread cache off, but it is on; "
      "set MALLOC_CONF=tcache:false in the environment");
  KJ_LOG(WARNING, "guest-kernel: per-isolate heap arenas enabled (jemalloc, thread cache off)");
#else
  KJ_LOG(WARNING,
      "guest-kernel: built without per-isolate heap arenas (build with "
      "--//src/workerd/server:allocator=jemalloc); isolates' C++ objects share one heap");
#endif
}

#ifdef WORKERD_HAS_HEAP_ARENA

namespace {

constexpr size_t PAGE_SIZE = 4096;

// Bounds the extents jemalloc reserves ahead of demand: it grows its retained pool
// geometrically, and without a limit the steps soon reach gigabytes, which the region cannot
// give back. An allocation larger than this gets an extent of exactly its size.
constexpr size_t RETAIN_GROW_LIMIT = 64 * 1024 * 1024;

// The argument of jemalloc's experimental.arenas_create_ext control: its arena_config_t
// (include/jemalloc/internal/arena_types.h), which the public header does not declare.
struct ArenaConfig {
  extent_hooks_t* extent_hooks;
  bool metadata_use_hooks;
};

// Resolved once so that the per-lock arena switch needs no name lookup.
struct ThreadArenaMib {
  size_t mib[2];
  size_t length = 2;
  ThreadArenaMib() {
    int err = mallctlnametomib("thread.arena", mib, &length);
    KJ_REQUIRE(err == 0, "guest-kernel: jemalloc has no thread.arena control", err);
  }
};
const ThreadArenaMib& threadArenaMib() {
  static const ThreadArenaMib mib;
  return mib;
}

// Switches the calling thread onto arena `index` and returns the arena it was on.
unsigned switchThreadArena(unsigned index) {
  auto& mib = threadArenaMib();
  unsigned previous = 0;
  size_t size = sizeof(previous);
  int err = mallctlbymib(mib.mib, mib.length, &previous, &size, &index, sizeof(index));
  KJ_REQUIRE(err == 0, "guest-kernel: switching the thread's heap arena failed", index, err);
  return previous;
}

}  // namespace

// jemalloc hands the registered extent_hooks_t pointer back to every hook; it is the first
// member so that a hook can recover the arena it belongs to. Every hook may be called from any
// thread, in the guest or not, as HeapArena's class comment describes; alloc and commit touch
// the region (and so run only while it is mapped: under the isolate's lock), the rest only
// shared bookkeeping. None of them allocates.
struct HeapArena::Hooks {
  extent_hooks_t table;
  HeapArena& arena;

  explicit Hooks(HeapArena& arena)
      : table{&alloc, &dalloc, &destroy, &commit, &decommit, &purgeLazy, &purgeForced, &split,
          &merge},
        arena(arena) {}

  static HeapArena& from(extent_hooks_t* table) {
    return reinterpret_cast<Hooks*>(table)->arena;
  }

  // Returns `size` bytes at `alignment` from the region, or null. A request for a specific
  // address (jemalloc growing a large allocation in place) is declined; jemalloc then allocates
  // elsewhere and copies.
  static void* alloc(extent_hooks_t* table,
      void* newAddr,
      size_t size,
      size_t alignment,
      bool* zero,
      bool* commit,
      unsigned arenaIndex) {
    if (newAddr != nullptr) return nullptr;
    void* p = from(table).take(size, alignment);
    if (p == nullptr) return nullptr;
    // Fresh pages of the reservation, so they read as zero, just made accessible, so committed.
    *zero = true;
    *commit = true;
    return p;
  }

  // Refused: jemalloc keeps the extent retained for reuse.
  static bool dalloc(extent_hooks_t*, void*, size_t, bool, unsigned) {
    return true;
  }

  // Only on arena destruction. The pages are left as they are: the region is never reused.
  static void destroy(extent_hooks_t*, void*, size_t, bool, unsigned) {}

  // Every extent is handed out committed, so this is only ever a no-op; done anyway in case
  // jemalloc's view of an extent differs.
  static bool commit(extent_hooks_t*, void* addr, size_t, size_t offset, size_t length, unsigned) {
    return mprotect(static_cast<kj::byte*>(addr) + offset, length, PROT_READ | PROT_WRITE) != 0;
  }

  // Refused: the extent stays committed, with its contents.
  static bool decommit(extent_hooks_t*, void*, size_t, size_t, size_t, unsigned) {
    return true;
  }
  static bool purgeLazy(extent_hooks_t*, void*, size_t, size_t, size_t, unsigned) {
    return true;
  }
  static bool purgeForced(extent_hooks_t*, void*, size_t, size_t, size_t, unsigned) {
    return true;
  }

  // Splitting and merging extents is bookkeeping on jemalloc's side only: the region's pages
  // are not affected, so both always succeed.
  static bool split(extent_hooks_t*, void*, size_t, size_t, size_t, bool, unsigned) {
    return false;
  }
  static bool merge(extent_hooks_t*, void*, size_t, void*, size_t, bool, unsigned) {
    return false;
  }
};

HeapArena::HeapArena(uintptr_t begin, uintptr_t end)
    : hooks(new Hooks(*this)),
      begin(begin),
      end(end),
      next(begin) {
  KJ_REQUIRE(begin % PAGE_SIZE == 0 && end % PAGE_SIZE == 0 && begin < end,
      "invalid heap arena region", begin, end);
  KJ_REQUIRE(heapArenasAvailable(),
      "guest-kernel: per-isolate heap arenas need jemalloc's thread cache off");
  ArenaConfig config{.extent_hooks = &hooks->table, .metadata_use_hooks = false};
  size_t size = sizeof(index);
  int err = mallctl("experimental.arenas_create_ext", &index, &size, &config, sizeof(config));
  KJ_REQUIRE(err == 0, "guest-kernel: creating a heap arena failed", err);

  auto set = [&](kj::StringPtr name, auto value) {
    auto key = kj::str("arena.", index, ".", name);
    int err = mallctl(key.cStr(), nullptr, nullptr, &value, sizeof(value));
    KJ_REQUIRE(err == 0, "guest-kernel: configuring a heap arena failed", key, err);
  };
  // No decay: freed extents stay retained rather than being purged through the hooks later.
  set("dirty_decay_ms", ssize_t(-1));
  set("muzzy_decay_ms", ssize_t(-1));
  set("retain_grow_limit", RETAIN_GROW_LIMIT);
}

HeapArena::~HeapArena() noexcept(false) {
  auto key = kj::str("arena.", index, ".destroy");
  int err = mallctl(key.cStr(), nullptr, nullptr, nullptr, 0);
  if (err != 0) {
    // jemalloc still refers to the hooks (a thread is still on the arena, say), so they stay.
    KJ_LOG(ERROR, "guest-kernel: destroying a heap arena failed; its memory is leaked", index, err);
    return;
  }
  delete hooks;
}

void* HeapArena::take(size_t size, size_t alignment) {
  if (size == 0 || size % PAGE_SIZE != 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
    return nullptr;
  }
  alignment = kj::max(alignment, PAGE_SIZE);
  uintptr_t start;
  {
    auto lock = next.lockExclusive();
    start = (*lock + alignment - 1) & ~(alignment - 1);
    if (start < *lock || start > end || end - start < size) return nullptr;
    *lock = start + size;
  }
  // The reservation is inaccessible; only running out of VMAs (ENOMEM) is a legitimate failure,
  // as in V8's own allocator. Anything else is a bug, and KJ_ASSERT is the one thing here that
  // allocates, on its way out of the process.
  if (mprotect(reinterpret_cast<void*>(start), size, PROT_READ | PROT_WRITE) != 0) {
    KJ_ASSERT(errno == ENOMEM, "committing heap arena pages failed", errno);
    return nullptr;
  }
  return reinterpret_cast<void*>(start);
}

size_t HeapArena::getRegionBytes() const {
  return *next.lockShared() - begin;
}

HeapArenaScope::HeapArenaScope(kj::Maybe<HeapArena&> arena) {
  KJ_IF_SOME(a, arena) {
    previousIndex = switchThreadArena(a.getIndex());
    switched = true;
  }
}

HeapArenaScope::~HeapArenaScope() noexcept(false) {
  if (switched) {
    auto& mib = threadArenaMib();
    int err =
        mallctlbymib(mib.mib, mib.length, nullptr, nullptr, &previousIndex, sizeof(previousIndex));
    if (err != 0) {
      KJ_LOG(ERROR, "guest-kernel: restoring the thread's heap arena failed", previousIndex, err);
    }
  }
}

unsigned currentHeapArenaIndex() {
  auto& mib = threadArenaMib();
  unsigned index = 0;
  size_t size = sizeof(index);
  int err = mallctlbymib(mib.mib, mib.length, &index, &size, nullptr, 0);
  KJ_REQUIRE(err == 0, "guest-kernel: reading the thread's heap arena failed", err);
  return index;
}

bool isAutomaticHeapArena(unsigned index) {
  unsigned automatic = 0;
  size_t size = sizeof(automatic);
  int err = mallctl("opt.narenas", &automatic, &size, nullptr, 0);
  KJ_REQUIRE(err == 0, "guest-kernel: reading jemalloc's arena count failed", err);
  return index < automatic;
}

#else  // WORKERD_HAS_HEAP_ARENA

HeapArenaScope::HeapArenaScope(kj::Maybe<HeapArena&> arena) {}
HeapArenaScope::~HeapArenaScope() noexcept(false) {}

#endif  // WORKERD_HAS_HEAP_ARENA

}  // namespace workerd::jsg
