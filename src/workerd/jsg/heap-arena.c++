// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "heap-arena.h"

#include <kj/debug.h>

#ifdef WORKERD_HAS_JEMALLOC
#include <jemalloc/jemalloc.h>

#include <atomic>
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

// Whether a __cxa_atexit registration has to be placed in the shared heap, which it does from
// the moment the first per-isolate heap arena exists; see __wrap___cxa_atexit at the end of this
// file for what the registration would otherwise cost, and armSharedHeapAtexit() for why the
// moment is that one. Constant-initialized, with no lazily initialized state behind it, because
// the wrapper reads it from the program's first registration on, long before jemalloc is set up.
std::atomic<bool> placeAtexitInSharedHeap{false};

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

// Switches the calling thread back onto arena `index` at the end of a scope, where a failure can
// only be logged.
void restoreThreadArena(unsigned index) {
  auto& mib = threadArenaMib();
  int err = mallctlbymib(mib.mib, mib.length, nullptr, nullptr, &index, sizeof(index));
  if (err != 0) {
    KJ_LOG(ERROR, "guest-kernel: restoring the thread's heap arena failed", index, err);
  }
}

// The arena SharedHeapScope switches to. jemalloc creates arena 0 when it initializes and never
// destroys it.
constexpr unsigned SHARED_HEAP_ARENA_INDEX = 0;

// Starts placing __cxa_atexit registrations in the shared heap, which stays so for the rest of
// the process. HeapArena's constructor calls it, and that moment is both late enough and early
// enough.
//
// Late enough that the wrapper can switch arenas at all. Switching reads two function-local
// statics -- heapArenasAvailable()'s answer and the thread.arena MIB -- and a registration made
// while either is still being initialized would re-enter it on the same thread, which aborts the
// process ("__cxa_guard_acquire detected recursive initialization"). That is reachable, not
// hypothetical: both statics are initialized through mallctl, which initializes jemalloc, and
// jemalloc registers an atexit handler of its own from inside malloc_init_hard when stats_print
// or prof is on. So the switch is made once here, where nothing is half-initialized, and the
// flag is published only afterwards; from then on the wrapper reaches both statics complete.
//
// Early enough that nothing is missed: until the first arena exists, every thread allocates from
// an automatic arena, so a registration before this point is in the shared heap already with the
// wrapper doing nothing. Doing nothing is also the only safe thing for the wrapper to do until
// then. The program's first registrations are made during static initialization, and a wrapper
// that evaluated heapArenasAvailable() on its first call could latch that answer before
// configureAllocatorForGuestKernel() has set malloc_conf, which would read the thread cache as
// on and leave heap arenas off for the whole process.
void armSharedHeapAtexit() {
  // Enter and leave a scope, for its initialization of the statics rather than for its effect.
  { SharedHeapScope scope; }
  placeAtexitInSharedHeap.store(true, std::memory_order_release);
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

  // A turn can run on an arena from here on, so glibc's exit-function chain needs placing.
  armSharedHeapAtexit();
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

HeapArena::Stats HeapArena::getStats() const {
  uint64_t epoch = 1;
  size_t epochSize = sizeof(epoch);
  mallctl("epoch", &epoch, &epochSize, &epoch, sizeof(epoch));
  auto read = [&](kj::StringPtr name) -> size_t {
    auto key = kj::str("stats.arenas.", index, ".", name);
    size_t value = 0;
    size_t size = sizeof(value);
    if (mallctl(key.cStr(), &value, &size, nullptr, 0) != 0) return 0;
    return value;
  };
  return Stats{
    .regionBytes = getRegionBytes(),
    .allocated = read("small.allocated") + read("large.allocated"),
    .active = read("pactive") * PAGE_SIZE,
    .mapped = read("mapped"),
    .retained = read("retained"),
    .dirty = read("pdirty") * PAGE_SIZE,
  };
}

HeapArenaScope::HeapArenaScope(kj::Maybe<HeapArena&> arena) {
  KJ_IF_SOME(a, arena) {
    previousIndex = switchThreadArena(a.getIndex());
    switched = true;
  }
}

HeapArenaScope::~HeapArenaScope() noexcept(false) {
  if (switched) restoreThreadArena(previousIndex);
}

SharedHeapScope::SharedHeapScope() {
  if (!heapArenasAvailable()) return;
  previousIndex = switchThreadArena(SHARED_HEAP_ARENA_INDEX);
  switched = true;
}

SharedHeapScope::~SharedHeapScope() noexcept(false) {
  if (switched) restoreThreadArena(previousIndex);
}

// The hooks under which V8 runs the code that grows its process-wide state, one pair per patch
// that needs them. Each pair is the SharedHeapScope in two halves, with the arena to return to
// carried in the token (offset by one so that zero means nothing was switched). V8 declares each
// function weak and calls it only when the embedder defines it, so a pair left undefined leaves
// its patch inert; no header declares them.
namespace {
uintptr_t sharedAllocationEnter() {
  if (!heapArenasAvailable()) return 0;
  return switchThreadArena(SHARED_HEAP_ARENA_INDEX) + 1;
}
void sharedAllocationLeave(uintptr_t token) {
  if (token != 0) restoreThreadArena(static_cast<unsigned>(token - 1));
}
}  // namespace

// The Intl builtins (patches/v8/0049-Run-Intl-builtins-under-an-embedder-shared-allocation-
// scope.patch): V8 keeps process-wide Intl containers (available-locale sets, calendar, date
// format and pattern generator caches) that it grows from inside those builtins, which run in a
// turn, on the isolate's heap arena.
extern "C" uintptr_t v8_intl_shared_allocation_enter() {
  return sharedAllocationEnter();
}
extern "C" void v8_intl_shared_allocation_leave(uintptr_t token) {
  sharedAllocationLeave(token);
}

// The wasm type canonicalizer (patches/v8/0050-Canonicalize-wasm-types-under-an-embedder-shared-
// allocation-scope.patch): the canonicalizer is a process-wide singleton whose zone, canonical
// supertype vector and recursion-group sets grow from whichever isolate first needs a type that
// is not canonicalized yet. Installing the WebAssembly JS API alone grows them, on every
// Context::New, so without this the second isolate in a process faults creating its context.
extern "C" uintptr_t v8_wasm_shared_allocation_enter() {
  return sharedAllocationEnter();
}
extern "C" void v8_wasm_shared_allocation_leave(uintptr_t token) {
  sharedAllocationLeave(token);
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
SharedHeapScope::SharedHeapScope() {}
SharedHeapScope::~SharedHeapScope() noexcept(false) {}

#endif  // WORKERD_HAS_HEAP_ARENA

#ifdef WORKERD_HAS_JEMALLOC
extern "C" int __real___cxa_atexit(void (*func)(void*), void* arg, void* dso);

// Every function-local static with a non-trivial destructor registers its destructor through
// __cxa_atexit when it is first initialized, and glibc keeps those registrations in a chain of
// 32-entry blocks whose first block is static. __new_exitfn walks the chain from its head on
// every registration and calloc()s another block once the head block is full, so a registration
// made inside a turn puts a block of process-wide state in that isolate's heap arena -- and from
// then on every other isolate's turn faults *reading* that block on its way down the chain,
// whatever it is registering. Scoping the code that registers is no remedy, which is why the
// embedder scope V8's Intl builtins run under does not stop ICU's registration from faulting the
// next isolate: what faults is the read of a block an earlier, unscoped registration placed.
//
// So no registration is left to place a block: each one runs in the shared heap, from the moment
// the first arena exists (armSharedHeapAtexit() above). This interposes on the whole chain of
// blocks rather than reserving blocks ahead of demand, because the number of statics first
// reached inside a turn is not something the runtime bounds -- V8 and ICU have thousands -- so a
// reserve could only postpone the fault by however many entries it guessed, and exhausting it
// would bring the fault back silently.
//
// The reference the linker redirects here is the one compiler-generated static initialization
// emits, which glibc's own atexit() shares; what stays with libc is what libc registers for
// itself while the program starts, before any arena exists. The --wrap option comes from
// jsg-core's linkopts, so it covers every binary that links this file and none that does not.
//
// glibc's other registration call, __cxa_thread_atexit_impl, is left alone. It prepends a node
// to a list whose head is thread-local and which no other turn reads, so a node in an arena
// faults nothing; placing the node would also not place the thread_local object whose destructor
// it names, which is what would actually outlive the arena.
extern "C" int __wrap___cxa_atexit(void (*func)(void*), void* arg, void* dso) {
#ifndef WORKERD_GK_NEG_ATEXIT_SHARED_HEAP
  if (placeAtexitInSharedHeap.load(std::memory_order_acquire)) {
    SharedHeapScope scope;
    return __real___cxa_atexit(func, arg, dso);
  }
  // Nothing to place: no arena exists yet, so the calling thread allocates from an automatic
  // arena, which is the shared heap.
#else
  // WORKERD_GK_NEG_ATEXIT_SHARED_HEAP is a negative-test build, never shipped: registrations are
  // left wherever the calling thread allocates, so the first isolate to register inside a turn
  // puts a block of the chain in its own arena and the next isolate's registration faults
  // walking to it. atexit-arena-test.sh must then fail.
#endif
  return __real___cxa_atexit(func, arg, dso);
}
#endif  // WORKERD_HAS_JEMALLOC

}  // namespace workerd::jsg
