// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "host-memory.h"

#include <workerd/util/hostcall.h>

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <sys/mman.h>
#include <unistd.h>

#include <kj/mutex.h>
#include <kj/time.h>

#include <atomic>
#include <cerrno>
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

// Whether the guest kernel is active. gk_initialized() reads gk's supervisor control block, which
// a JS turn at ring 3 cannot, and resolve() runs inside turns, so the answer is read once,
// host-side, and kept: gk_init() runs before any isolate exists and never again.
bool guestKernelActive() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  static const bool active = gk_initialized() != 0;
  return active;
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

// =======================================================================================
// HostPool

HostPool::~HostPool() noexcept(false) {
  for (unsigned i = 0; i < regionCount; i++) {
    Region& r = regions[i];
#ifdef WORKERD_HAS_GUEST_KERNEL
    if (r.registered) {
      int result = gk_host_range_unregister(reinterpret_cast<void*>(r.base), r.end - r.base);
      KJ_ASSERT(result == 0, "guest-kernel: gk_host_range_unregister failed", result);
    }
#endif
    munmap(reinterpret_cast<void*>(r.base), r.end - r.base);
  }
}

bool HostPool::ensure(size_t capacity) {
  if (capacity > MAX_RESERVATION) capacity = MAX_RESERVATION;
  if (reserved >= capacity) return true;
  if (regionCount == REGIONS) return false;
  size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  size_t bytes = kj::max(capacity - reserved, MIN_REGION);
  bytes = (bytes + page - 1) / page * page;
  // Mapped readable and writable from the start: nothing is committed until a page is touched,
  // and MAP_NORESERVE keeps the reservation off the overcommit ledger.
  void* mem = mmap(
      nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mem == MAP_FAILED) return false;
  bool registered = false;
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    // Registered before anything is handed out of it, so no guest thread can have touched a
    // page of it as a user page first.
    if (gk_host_range_register(mem, bytes) != 0) {
      munmap(mem, bytes);
      return false;
    }
    registered = true;
  }
#endif
  auto base = reinterpret_cast<uintptr_t>(mem);
  regions[regionCount++] = Region{base, base + bytes, base, registered};
  reserved += bytes;
  return true;
}

unsigned HostPool::classOf(size_t size) {
  unsigned k = 0;
  while ((MIN_BLOCK << k) < size) k++;
  return k;
}

const HostPool::Region* HostPool::regionOf(uintptr_t p) const {
  for (unsigned i = 0; i < regionCount; i++) {
    if (p >= regions[i].base && p < regions[i].end) return &regions[i];
  }
  return nullptr;
}

bool HostPool::contains(const void* ptr) const {
  return regionOf(reinterpret_cast<uintptr_t>(ptr)) != nullptr;
}

void* HostPool::allocate(size_t size) {
  if (size == 0) return nullptr;
  unsigned k = classOf(size);
  if (k >= CLASSES) return nullptr;
  size_t block = MIN_BLOCK << k;
  if (void* p = freeLists[k]; p != nullptr) {
    freeLists[k] = *static_cast<void**>(p);
    *static_cast<void**>(p) = nullptr;  // the rest was zeroed when it was freed
    return p;
  }
  // Carved from a region's bump pointer at the block's own alignment, at most a page's; the
  // newest region first, then the tails of the older ones.
  size_t align = block < 4096 ? block : 4096;
  for (unsigned i = regionCount; i > 0; i--) {
    Region& r = regions[i - 1];
    uintptr_t start = (r.bump + align - 1) & ~(align - 1);
    if (start < r.bump || start > r.end || block > r.end - start) continue;
    r.bump = start + block;
    return reinterpret_cast<void*>(start);  // fresh pages are zero
  }
  return nullptr;
}

void HostPool::deallocate(void* ptr, size_t size) {
  if (ptr == nullptr) return;
  unsigned k = classOf(size);
  size_t block = MIN_BLOCK << k;
  auto p = reinterpret_cast<uintptr_t>(ptr);
  size_t align = block < 4096 ? block : 4096;
  // A pointer that is not the start of a block of this class in one of this pool's regions is
  // a pointer loaded from memory the guest could rewrite: refused as fatal rather than written
  // through, as the guest kernel refuses one on its own blocks.
  const Region* r = regionOf(p);
  KJ_ASSERT(r != nullptr && (p & (align - 1)) == 0 && block <= r->bump - p,
      "HostPool::deallocate of a pointer that is not a block of that size");
  scrub(ptr, block);
  *static_cast<void**>(ptr) = freeLists[k];
  freeLists[k] = ptr;
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
    WD_LOG_REFUSAL(LogSite::SLOT_RESOLVE,
        "host slot handle does not name a live slot; it resolves to nothing", handle);
    return nullptr;
  }
  return slot->ptr;
}

void fallbackFreeSlot(SlotHandle handle) {
  auto lock = fallbackTable().lockExclusive();
  FallbackSlot* slot = lock->lookup(handle);
  if (slot == nullptr) {
    WD_LOG_REFUSAL(
        LogSite::SLOT_FREE, "host slot handle does not name a live slot; nothing is freed", handle);
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

// The resolve table used when the guest kernel is not active: the same shape, the same handle
// layout and the same checks as gk's (see gk_resolve in gk.h). An entry is {pointer, generation},
// live while its generation is nonzero; a handle is the entry's index in its low
// RESOLVE_INDEX_BITS bits and the generation above them, and the capacity is exactly
// 2^RESOLVE_INDEX_BITS, so masking the index bounds it. The lookup takes no lock: it reads the
// generation on both sides of the pointer and answers only if both match the handle, and the
// writers publish an entry pointer-first and retire it generation-first, as gk does. A free
// entry's pointer word holds the free-list link (the next free index + 1, 0 ending the list);
// the reader never returns the word of an entry whose generation is 0. A reserved entry is live
// with a null pointer, and resolves to nothing until it is committed.
constexpr unsigned RESOLVE_INDEX_BITS = 20;
constexpr unsigned RESOLVE_GEN_BITS = 64 - RESOLVE_INDEX_BITS;
static_assert(RESOLVE_CAPACITY == (size_t{1} << RESOLVE_INDEX_BITS));

struct FallbackResolveEntry {
  std::atomic<void*> ptr{nullptr};
  std::atomic<uint64_t> gen{0};
  std::atomic<uint64_t> type{RESOLVE_TYPE_NONE};
};
static_assert(sizeof(FallbackResolveEntry) == 24);

// Static and never touched until an entry is written, like gk's table, so the capacity costs
// nothing per unused page.
FallbackResolveEntry fallbackResolveEntries[RESOLVE_CAPACITY];

struct FallbackResolveState {
  size_t taken = 0;       // entries ever taken
  uint32_t freeHead = 0;  // free-list head, as index + 1; 0 when empty
  uint64_t gen = 0;       // the generation of the last registration
  size_t live = 0;        // entries registered or reserved and not yet unregistered
};

kj::MutexGuarded<FallbackResolveState>& fallbackResolveState() {
  static kj::MutexGuarded<FallbackResolveState> state;
  return state;
}

// Takes a free entry under a fresh generation and publishes it with `ptr` (null for a
// reservation). 0 when the table is full. The caller holds the lock.
ResolveHandle fallbackTake(FallbackResolveState& state, void* ptr, uint64_t type) {
  uint32_t idx;
  if (state.freeHead != 0) {
    idx = state.freeHead - 1;
    state.freeHead = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
        fallbackResolveEntries[idx].ptr.load(std::memory_order_relaxed)));
  } else if (state.taken < RESOLVE_CAPACITY) {
    idx = state.taken++;
  } else {
    return 0;  // the table is full: refused, never fatal
  }
  // A generation is never 0 and never reused within 2^RESOLVE_GEN_BITS registrations.
  uint64_t gen = ++state.gen;
  if (gen >> RESOLVE_GEN_BITS) gen = state.gen = 1;
  auto& entry = fallbackResolveEntries[idx];
  entry.type.store(type, std::memory_order_release);
  entry.ptr.store(ptr, std::memory_order_release);
  entry.gen.store(gen, std::memory_order_release);
  state.live++;
  return (gen << RESOLVE_INDEX_BITS) | idx;
}

ResolveHandle fallbackRegisterResolve(void* ptr, uint64_t type) {
  auto lock = fallbackResolveState().lockExclusive();
  return fallbackTake(*lock, ptr, type);
}

uint32_t fallbackReserveResolve(ResolveHandle* handles, uint32_t n) {
  auto lock = fallbackResolveState().lockExclusive();
  uint32_t got = 0;
  while (got < n) {
    ResolveHandle handle = fallbackTake(*lock, nullptr, RESOLVE_TYPE_NONE);
    if (handle == 0) break;
    handles[got++] = handle;
  }
  return got;
}

bool fallbackCommitResolve(ResolveHandle handle, void* ptr, uint64_t type) {
  uint64_t idx = handle & ((uint64_t{1} << RESOLVE_INDEX_BITS) - 1);
  uint64_t gen = handle >> RESOLVE_INDEX_BITS;
  auto lock = fallbackResolveState().lockExclusive();
  auto& entry = fallbackResolveEntries[idx];
  if (gen == 0 || entry.gen.load(std::memory_order_relaxed) != gen ||
      entry.ptr.load(std::memory_order_relaxed) != nullptr) {
    WD_LOG_REFUSAL(LogSite::RESOLVE_COMMIT,
        "resolve handle does not name a reserved entry; nothing is committed", kj::hex(handle));
    return false;
  }
  if (ptr == nullptr) {
    WD_LOG_REFUSAL(LogSite::RESOLVE_COMMIT_NULL,
        "a null pointer cannot be committed to a resolve entry", kj::hex(handle));
    return false;
  }
  entry.type.store(type, std::memory_order_release);
  entry.ptr.store(ptr, std::memory_order_release);
  return true;
}

void* fallbackResolveTyped(ResolveHandle handle, uint64_t* type) {
  if (type != nullptr) *type = RESOLVE_TYPE_NONE;
  uint64_t idx = handle & ((uint64_t{1} << RESOLVE_INDEX_BITS) - 1);
  uint64_t gen = handle >> RESOLVE_INDEX_BITS;
  if (gen == 0) return nullptr;
  auto& entry = fallbackResolveEntries[idx];  // the mask bounds idx
  uint64_t g1 = entry.gen.load(std::memory_order_acquire);
  void* ptr = entry.ptr.load(std::memory_order_acquire);
  uint64_t tag = entry.type.load(std::memory_order_acquire);
  uint64_t g2 = entry.gen.load(std::memory_order_acquire);
  if (g1 != gen || g2 != gen) return nullptr;
  if (type != nullptr) *type = tag;
  return ptr;
}

void* fallbackResolve(ResolveHandle handle) {
  return fallbackResolveTyped(handle, nullptr);
}

void fallbackUnregisterResolve(ResolveHandle handle) {
  uint64_t idx = handle & ((uint64_t{1} << RESOLVE_INDEX_BITS) - 1);
  uint64_t gen = handle >> RESOLVE_INDEX_BITS;
  auto lock = fallbackResolveState().lockExclusive();
  auto& entry = fallbackResolveEntries[idx];
  if (gen == 0 || entry.gen.load(std::memory_order_relaxed) != gen) {
    WD_LOG_REFUSAL(LogSite::RESOLVE_UNREGISTER,
        "resolve handle does not name a live entry; nothing is unregistered", kj::hex(handle));
    return;
  }
  entry.gen.store(0, std::memory_order_release);
  entry.ptr.store(reinterpret_cast<void*>(uintptr_t{lock->freeHead}), std::memory_order_release);
  lock->freeHead = idx + 1;
  lock->live--;
}

size_t fallbackLiveResolveEntries() {
  return fallbackResolveState().lockShared()->live;
}

// The table operations, routed to gk's table when the guest kernel is active and to the
// fallback otherwise. Host-side: inside a turn they are reached only through the journal
// below.
void* tableResolve(ResolveHandle handle);
void clearCompanion(ResolveHandle handle);
void tableUnregister(ResolveHandle handle);

ResolveHandle tableRegister(void* ptr, uint64_t type) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_register(ptr, type);
#endif
  return fallbackRegisterResolve(ptr, type);
}
uint32_t tableReserve(ResolveHandle* handles, uint32_t n) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    int got = gk_resolve_reserve(handles, static_cast<int>(n));
    return got < 0 ? 0 : static_cast<uint32_t>(got);
  }
#endif
  return fallbackReserveResolve(handles, n);
}
bool tableCommit(ResolveHandle handle, void* ptr, uint64_t type) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_commit(handle, ptr, type) == 0;
#endif
  return fallbackCommitResolve(handle, ptr, type);
}
// The lookup, with the entry's tag when the caller wants it. The guest kernel returns it beside
// the pointer, which at ring 3 costs the instruction that otherwise zeroes the register.
void* tableResolveTyped(ResolveHandle handle, uint64_t* type) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_typed(handle, type);
#endif
  return fallbackResolveTyped(handle, type);
}
void* tableResolve(ResolveHandle handle) {
  return tableResolveTyped(handle, nullptr);
}
// The arena the calling thread is on, as gk records it: the arena a turn on this thread runs
// under, whatever the turn wrote in its own memory. Null without the guest kernel.
void* currentArena() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_arena_current();
#endif
  return nullptr;
}

#ifdef WORKERD_GK_NEG_RESOLVE_TYPE
// WORKERD_GK_NEG_RESOLVE_TYPE is a negative-test build, never shipped: a resolve answers with
// the entry's pointer whatever class the entry holds, as it did before the tag existed, so a
// handle a turn has written over another of its own isolate's resolves to an object of the
// wrong type. The guest kernel's ioown-test.sh must then fail, a turn reaching an object of
// another class through a handle of its own.
constexpr bool CHECK_RESOLVE_TYPE = false;
#else
constexpr bool CHECK_RESOLVE_TYPE = true;
#endif

#ifdef WORKERD_GK_NEG_RESOLVE_RETIRE
// WORKERD_GK_NEG_RESOLVE_RETIRE is a negative-test build, never shipped: a handle out of a
// turn's journal is acted on without asking whose entry it is, as it was before this check
// existed, so the host runs the released hook over whatever entry the turn named and retires
// it. The guest kernel's ioown-test.sh must then fail, a turn's drop of a stolen handle
// unlinking another isolate's object from its list and leaving that isolate unable to reach it.
constexpr bool CHECK_TURN_OWNER = false;
#else
constexpr bool CHECK_TURN_OWNER = true;
#endif

// Whether `handle` names a live committed entry that `arena` owns. This is the comparison the
// ring-0 lookup makes for a turn and that the host's own lookup makes for nobody.
bool tableOwnedBy(void* arena, ResolveHandle handle) {
  if (!CHECK_TURN_OWNER) return true;
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    return gk_resolve_owned_by(static_cast<gk_arena*>(arena), handle) != 0;
  }
#endif
  // No guest kernel: one address space, no arenas, and every live entry is the one tenant's.
  return fallbackResolve(handle) != nullptr;
}

// Retires an entry that `arena` owns and refuses one it does not, the check and the retire in
// one step under gk's lock so that nothing comes between them. The caller has established the
// owner already -- that is what decides whether the released hook runs at all -- and this is
// where the check is atomic with the retire. The companion goes first, as in tableUnregister.
void tableUnregisterFor(void* arena, ResolveHandle handle) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (CHECK_TURN_OWNER && guestKernelActive()) {
    clearCompanion(handle);
    gk_resolve_unregister_for(static_cast<gk_arena*>(arena), handle);
    return;
  }
#endif
  if (!tableOwnedBy(arena, handle)) return;
  tableUnregister(handle);
}

void tableUnregister(ResolveHandle handle) {
  // The companion goes first, while the entry is still this handle's: once the entry is free a
  // later registration may take its index.
  clearCompanion(handle);
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    gk_resolve_unregister(handle);
    return;
  }
#endif
  fallbackUnregisterResolve(handle);
}

// Companions (see host-memory.h): a table of RESOLVE_CAPACITY records indexed like the resolve
// table, in host memory, in chunks mapped as entries come into use. The chunk directory is a
// host-memory block reached through a host slot handle kept in a function-local static: the
// handle is the one word of this facility outside host memory, and a rewritten one resolves to
// nothing, so the host refuses the use rather than reading a directory of the guest's choosing.
// A chunk pointer is published once, so readers take no lock; the directory's own lock covers
// mapping a chunk.
constexpr unsigned COMPANION_CHUNK_BITS = 10;
constexpr size_t COMPANION_CHUNK_ENTRIES = size_t{1} << COMPANION_CHUNK_BITS;
constexpr size_t COMPANION_CHUNKS = RESOLVE_CAPACITY / COMPANION_CHUNK_ENTRIES;
static_assert(RESOLVE_CAPACITY % COMPANION_CHUNK_ENTRIES == 0);

struct CompanionChunk {
  ResolveCompanion entries[COMPANION_CHUNK_ENTRIES];
};
static_assert(sizeof(CompanionChunk) <= 16384, "a chunk is one host-memory block");

struct CompanionDirectory {
  kj::MutexGuarded<int> mapLock;  // held while mapping a chunk; the value is unused
  std::atomic<CompanionChunk*> chunks[COMPANION_CHUNKS];
};

CompanionDirectory* companionDirectory() {
  // Never freed: entries are retired from threads that may outlive static destruction.
  static HostSlot<CompanionDirectory>* slot =
      new HostSlot<CompanionDirectory>(hostSlot<CompanionDirectory>());
  return slot->get();  // null if the slot handle no longer resolves
}

// The companion record of a live committed entry, or none: the handle is validated against the
// table, and the record found by the entry's index. With `map`, a chunk not yet mapped is
// mapped first, out of host memory, which a hostcall handler may take (the commit hook runs in
// one for a turn that refills its reserved entries).
kj::Maybe<ResolveCompanion&> companionOf(ResolveHandle handle, bool map) {
  if (tableResolve(handle) == nullptr) return kj::none;
  CompanionDirectory* dir = companionDirectory();
  if (dir == nullptr) {
    WD_LOG_REFUSAL(LogSite::COMPANION_DIRECTORY,
        "the resolve companion directory does not resolve; the use is refused");
    return kj::none;
  }
  size_t idx = handle & (RESOLVE_CAPACITY - 1);
  auto& slot = dir->chunks[idx >> COMPANION_CHUNK_BITS];
  CompanionChunk* chunk = slot.load(std::memory_order_acquire);
  if (chunk == nullptr) {
    if (!map) return kj::none;
    auto lock = dir->mapLock.lockExclusive();
    chunk = slot.load(std::memory_order_acquire);
    if (chunk == nullptr) {
      chunk = new (allocate(sizeof(CompanionChunk), alignof(CompanionChunk))) CompanionChunk();
      slot.store(chunk, std::memory_order_release);
    }
  }
  return chunk->entries[idx & (COMPANION_CHUNK_ENTRIES - 1)];
}

void clearCompanion(ResolveHandle handle) {
  KJ_IF_SOME(c, companionOf(handle, false)) {
    c = ResolveCompanion{.kind = 0, .ptr = nullptr};
  }
}

// Turn-batched registration (see ResolveTurn in host-memory.h). A thread's pool is the host's
// record of the entries it has reserved for that thread, in host memory, found by the host
// through the guest kernel's per-thread words (supervisor memory, written by the host) rather
// than through anything a turn can write; without the guest kernel, through a thread-local. The
// journal is what the turn does with them: ordinary memory the turn writes, which the host reads
// only by position, bounded by its own count, and only for pointers, which the table validates.
constexpr uint32_t POOL_CAPACITY = 256;    // entries reserved ahead for a thread
constexpr uint32_t POOL_LOW = 64;          // a turn starts with at least this many
constexpr uint32_t RETIRE_CAPACITY = 256;  // committed entries a turn retires before a flush
constexpr uint32_t NOTE_CAPACITY = 512;    // notes a turn makes before handing them over

struct TurnJournal {
  enum State : uint8_t { UNUSED = 0, USED = 1, RETIRED = 2 };
  ResolveHandle handles[POOL_CAPACITY];            // the turn's copy of the pool, to hand out
  void* ptr[POOL_CAPACITY];                        // the pointer each handed-out position names
  uint64_t aux[POOL_CAPACITY][RESOLVE_AUX_WORDS];  // the words each registration carried
  uint64_t type[POOL_CAPACITY];                    // the tag each registration carried
  State state[POOL_CAPACITY];
  uint32_t count = 0;                     // positions available (the pool's count, copied)
  uint32_t next = 0;                      // positions handed out
  ResolveHandle retire[RETIRE_CAPACITY];  // committed entries unlinked during the turn
  uint32_t retireCount = 0;
  uint64_t notes[NOTE_CAPACITY][NOTE_WORDS];  // what the turn noted for the host (noteInTurn)
  uint32_t noteCount = 0;
  bool active = false;  // a turn is running on this thread
};

// A registration the host could not keep, waiting for the end of the turn. A settle inside the
// refill hostcall cannot tell the owner about one on the spot: refused() may dispose of what
// the registration named, and a hostcall handler must neither take the process heap nor make a
// hostcall of its own, which a disposer may do either of (see the handler rules in
// experimental/guest-kernel/gk.h). The entry itself is retired at the settle; only the hook is
// deferred, so nothing resolves the handle in between.
//
// The list is short because a refusal is an error path -- the table refusing a pointer, or the
// owner refusing the words -- and a turn producing this many of them is not going to succeed.
// It is a turn's own words that decide, so a turn can make refusals happen as often as it
// likes; what it cannot do is outrun the list. A settle that fills it stops there, leaving the
// positions it has not reached spent and the pool with nothing to hand back, so the turn's
// next registration is refused as it is when the table is full, and every refusal still
// reaches the owner when the turn returns. Nothing is dropped and nothing is left unowned.
constexpr uint32_t REFUSAL_CAPACITY = 32;
struct DeferredRefusal {
  ResolveHandle handle;
  void* ptr;
  uint64_t aux[RESOLVE_AUX_WORDS];
};

// A refusal report a hostcall handler made, waiting for the end of the turn for the same
// reason the refusals above do: a handler may not take the process heap, and KJ_LOG formats.
// So the line is rendered where the refusal was found, into this, and written when the turn
// returns. The text is a copy, not a reference: it names a handle or a pointer the turn
// supplied, and by the end of the turn neither may name anything.
//
// The list is short because the budget is spent before anything reaches it -- a report held
// back costs no slot -- so what can arrive here in one turn is bounded by the burst and not by
// the flood. A turn that fills it anyway has the overflow counted and said so.
constexpr uint32_t REPORT_CAPACITY = 16;
struct DeferredReport {
  const char* file;  // __FILE__ where the refusal was found; a literal, so holding it is safe
  int line;
  uint32_t suppressed;
  uint32_t length;
  char text[_::REPORT_TEXT];
};

struct ThreadPool {
  ResolveHandle handles[POOL_CAPACITY];
  uint32_t count = 0;
  TurnJournal* journal = nullptr;
  DeferredRefusal refusals[REFUSAL_CAPACITY];
  uint32_t refusalCount = 0;
  DeferredReport reports[REPORT_CAPACITY];
  uint32_t reportCount = 0;
  uint32_t reportsDropped = 0;
  ResolveTurnHooks hooks = {
    nullptr, nullptr, nullptr};  // the current turn's, written by the host at entry
  // The arena the current turn runs under, written by the host at entry from gk's own record of
  // the thread and cleared once the journal has been processed. It is what every handle the
  // host takes out of the journal is checked against (see ownedByTurn), and it is here, in the
  // pool, which is host memory the turn cannot write, rather than in a thread-local, which it
  // can.
  void* turnArena = nullptr;
};

// The turn's way to its journal. Guest-writable, and a lie costs only the liar: the host
// reaches the journal through the pool, never through this.
thread_local TurnJournal* currentJournal = nullptr;

#ifdef WORKERD_HAS_GUEST_KERNEL
constexpr unsigned POOL_THREAD_WORD = 3;  // the key of the pool's address among gk's thread words
static_assert(POOL_THREAD_WORD < GK_THREAD_WORDS);
#endif
thread_local ThreadPool* fallbackPool = nullptr;

ThreadPool* poolOfThread() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    uint64_t word = 0;
    if (gk_thread_words_get(POOL_THREAD_WORD, 1, &word) != 0) return nullptr;
    return reinterpret_cast<ThreadPool*>(static_cast<uintptr_t>(word));
  }
#endif
  return fallbackPool;
}

void setPoolOfThread(ThreadPool* pool) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    uint64_t word = reinterpret_cast<uintptr_t>(pool);
    KJ_ASSERT(gk_thread_words_set(POOL_THREAD_WORD, 1, &word) == 0);
    return;
  }
#endif
  fallbackPool = pool;
}

// The journal is the thread's, not any isolate's: it is created by whichever isolate first runs
// a turn on the thread, and read and written at ring 3 by every turn after it. So it can live
// neither in host memory, which ring 3 cannot touch, nor on the C++ heap, where the allocator
// decides its placement rather than what reads it does: an isolate's allocations are that
// isolate's own here (experimental/guest-kernel/memory-classes.md), so the heap would put the
// journal in the arena of the first isolate to run a turn on the thread, and every other
// isolate's turn would fault on it. Pages of its own are in no arena: ordinary process memory
// the guest kernel maps on demand under whichever page-table root asks for it.
#ifdef WORKERD_GK_NEG_TURN_JOURNAL_PAGES
// A negative build, never shipped: the journal comes from the C++ heap, so it lands in the
// arena of the first isolate to run a turn on the thread, and every later isolate's first turn
// faults reading journal->active. (Where the heap is shared rather than per isolate, the two
// placements are the same memory and nothing tells them apart.)
TurnJournal* allocateJournal() {
  return new TurnJournal();
}

void releaseJournal(TurnJournal* journal) {
  delete journal;
}
#else
TurnJournal* allocateJournal() {
  void* ptr = mmap(
      nullptr, sizeof(TurnJournal), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  KJ_REQUIRE(ptr != MAP_FAILED, "allocating a turn journal failed", errno);
  return new (ptr) TurnJournal();
}

// The pages are scrubbed and kept rather than unmapped: a turn has read and written them at
// ring 3, so the guest kernel has backed them with a memslot and a page-table entry in every
// arena root that faulted on them, and those roots outlive the thread. Unmapping them would
// leave those entries naming memory the process no longer has; handing a block back to the
// allocator, the other way to release one, leaves it mapped too. What is kept is one mapping
// of sizeof(TurnJournal) per thread that has ever run a turn, and workerd's threads are not
// short-lived.
void releaseJournal(TurnJournal* journal) {
  if (journal == nullptr) return;
  scrub(journal, sizeof(TurnJournal));
}
#endif

// Releases the thread's pool when the thread exits: its reserved entries, its journal and the
// block itself.
struct PoolReleaser {
  ~PoolReleaser() noexcept(false) {
    ThreadPool* pool = poolOfThread();
    if (pool == nullptr) return;
    setPoolOfThread(nullptr);
    for (uint32_t i = 0; i < pool->count; i++) tableUnregister(pool->handles[i]);
    releaseJournal(pool->journal);
    pool->~ThreadPool();
    deallocate(pool, sizeof(ThreadPool), alignof(ThreadPool));
  }
};

// Reserves entries until the pool holds POOL_CAPACITY, or the table runs out. Host-side.
// Returns how many were added.
uint32_t refillPool(ThreadPool& pool) {
  uint32_t before = pool.count;
  pool.count += tableReserve(pool.handles + pool.count, POOL_CAPACITY - pool.count);
  return pool.count - before;
}

// Gives the journal the entries of the pool that no position of it holds, as unspent
// positions above the ones a settle left behind. Host-side; leaves the retire list and the
// notes alone, so that a turn restarted over a refilled pool keeps what it has not yet handed
// over.
void startJournal(ThreadPool& pool) {
  TurnJournal& journal = *pool.journal;
  for (uint32_t i = journal.next; i < pool.count; i++) {
    journal.handles[i] = pool.handles[i];
    journal.state[i] = TurnJournal::UNUSED;
  }
  journal.count = pool.count;
}

ThreadPool& ensurePoolOfThread() {
  ThreadPool* pool = poolOfThread();
  if (pool == nullptr) {
    pool = new (allocate(sizeof(ThreadPool), alignof(ThreadPool))) ThreadPool();
    pool->journal = allocateJournal();
    setPoolOfThread(pool);
    static thread_local PoolReleaser releaser;
    (void)releaser;
  }
  return *pool;
}

// Retires the journal's committed-entry retire list, running the released hook for each entry
// first. The list is the turn's own memory, so a handle in it is a value the turn chose and not
// necessarily one of its own: each is checked against the arena the turn ran under, and one that
// is another isolate's is reported and left alone, entry, companion and all. Host-side; bounded
// by the list's capacity whatever the journal says.
void flushRetired(TurnJournal& journal, const ResolveTurnHooks& hooks, void* arena) {
  uint32_t n = kj::min(journal.retireCount, RETIRE_CAPACITY);
  for (uint32_t i = 0; i < n; i++) {
    ResolveHandle handle = journal.retire[i];
    if (!tableOwnedBy(arena, handle)) {
      // Either the entry has gone -- a turn may release what it never committed -- or it is
      // another isolate's, which is a turn naming a handle it was not given.
      if (tableResolve(handle) != nullptr) {
        WD_LOG_REFUSAL(LogSite::RETIRE_FOREIGN,
            "a turn released a resolve entry of another isolate; nothing is retired",
            kj::hex(handle));
      }
      continue;
    }
    if (hooks.released != nullptr) hooks.released(handle);
    tableUnregisterFor(arena, handle);
  }
  journal.retireCount = 0;
}

// Hands the journal's notes to the noted hook, in order, and keeps the ones it did not consume
// (only possible inside a handler), compacted to the front. Host-side; bounded by the capacity
// whatever the journal says.
void replayNotes(TurnJournal& journal, const ResolveTurnHooks& hooks, bool inHandler) {
  uint32_t n = kj::min(journal.noteCount, NOTE_CAPACITY);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint64_t words[NOTE_WORDS];
    for (size_t w = 0; w < NOTE_WORDS; w++) words[w] = journal.notes[i][w];
    bool consumed = hooks.noted == nullptr || hooks.noted(words, inHandler);
    if (consumed) continue;
    if (!inHandler) {
      WD_LOG_REFUSAL(LogSite::NOTE_UNCONSUMED,
          "a turn's note was not consumed at the end of the turn; it is dropped", words[0]);
      continue;
    }
    for (size_t w = 0; w < NOTE_WORDS; w++) journal.notes[kept][w] = words[w];
    kept++;
  }
  journal.noteCount = kept;
}

// Settles the positions the turn has spent: each registration is committed with the pointer
// and tag the turn journaled for it and handed to the committed hook, and a position the turn
// released, never used, or whose registration the table or the hook would not keep has its
// entry retired. The entries the turn did not reach stay reserved, compacted to the front of
// the pool for the next journal. Host-side; `next` is bounded by the pool's own count, so a
// turn that overstates it settles nothing beyond what was reserved for it.
//
// `inHandler` says this is the refill hostcall rather than the end of the turn, where the
// refused hook is deferred (see DeferredRefusal).
void settleJournal(ThreadPool& pool, bool inHandler) {
  TurnJournal& journal = *pool.journal;
  const ResolveTurnHooks hooks = pool.hooks;
  uint32_t next = kj::min(journal.next, pool.count);
  uint32_t done = 0;  // positions settled; the rest are left spent for the end of the turn
  for (; done < next; done++) {
    uint32_t i = done;
    ResolveHandle handle = pool.handles[i];
    if (journal.state[i] == TurnJournal::USED) {
      // A refusal found here has to wait for the end of the turn, and the list of those it
      // waits in is finite (see DeferredRefusal): with the list full there is no settling a
      // position that might refuse, so this one and the ones after it stay as they are.
      if (inHandler && pool.refusalCount >= REFUSAL_CAPACITY) break;
      if (tableCommit(handle, journal.ptr[i], journal.type[i])) {
        if (hooks.committed == nullptr || hooks.committed(handle, journal.ptr[i], journal.aux[i])) {
          continue;
        }
        WD_LOG_REFUSAL(LogSite::COMMIT_HOOK_REFUSED,
            "a turn's resolve entry was refused by the commit hook; it is retired", kj::hex(handle),
            journal.ptr[i]);
      } else {
        WD_LOG_REFUSAL(LogSite::COMMIT_FAILED,
            "a turn's resolve entry could not be committed; it resolves to nothing",
            kj::hex(handle), journal.ptr[i]);
      }
      // The entry is about to be retired with nothing of the turn's left naming it, so the
      // owner is told before the handle stops meaning anything.
      if (hooks.refused != nullptr) {
        if (inHandler) {
          DeferredRefusal& refusal = pool.refusals[pool.refusalCount++];
          refusal.handle = handle;
          refusal.ptr = journal.ptr[i];
          for (size_t w = 0; w < RESOLVE_AUX_WORDS; w++) refusal.aux[w] = journal.aux[i][w];
        } else {
          hooks.refused(handle, journal.ptr[i], journal.aux[i]);
        }
      }
    }
    // Retired by the turn, never used, or refused: the entry resolves to nothing from now on,
    // and so does any holder of it.
    tableUnregister(handle);
  }
  // The settled positions go, and what is left -- the spent positions a stopped settle did not
  // reach, then the entries no position holds -- moves down to meet the front of the pool. The
  // journal's own words move with the pool's handles, which is the pairing the settle reads.
  for (uint32_t i = done; i < pool.count; i++) {
    pool.handles[i - done] = pool.handles[i];
  }
  for (uint32_t i = done; i < next; i++) {
    journal.handles[i - done] = journal.handles[i];
    journal.ptr[i - done] = journal.ptr[i];
    journal.type[i - done] = journal.type[i];
    journal.state[i - done] = journal.state[i];
    for (size_t w = 0; w < RESOLVE_AUX_WORDS; w++) journal.aux[i - done][w] = journal.aux[i][w];
  }
  pool.count -= done;
  journal.next = next - done;
  journal.count = journal.next;
}

// Tells the owner about the registrations a settle inside a hostcall could not keep. Host-side,
// at the end of the turn, where the hook may dispose of what the registration named.
void flushRefusals(ThreadPool& pool) {
  const ResolveTurnHooks hooks = pool.hooks;
  uint32_t n = kj::min(pool.refusalCount, REFUSAL_CAPACITY);
  pool.refusalCount = 0;
  if (hooks.refused == nullptr) return;
  for (uint32_t i = 0; i < n; i++) {
    DeferredRefusal& refusal = pool.refusals[i];
    hooks.refused(refusal.handle, refusal.ptr, refusal.aux);
  }
}

// Writes the reports the turn's hostcalls could not write themselves, in the order they were
// made. Host-side, at the end of the turn, where formatting is allowed again.
void flushReports(ThreadPool& pool) {
  uint32_t n = kj::min(pool.reportCount, REPORT_CAPACITY);
  uint32_t dropped = pool.reportsDropped;
  pool.reportCount = 0;
  pool.reportsDropped = 0;
  for (uint32_t i = 0; i < n; i++) {
    DeferredReport& report = pool.reports[i];
    kj::ArrayPtr<const char> text(report.text, kj::min(report.length, _::REPORT_TEXT));
    uint32_t suppressedSinceLast = report.suppressed;
    // Logged at the refusal's own file and line, not at this one, and wrapped in kj::str() so
    // that kj leaves the text unlabelled: the line then reads exactly as the one the refusal
    // would have written on the spot.
    const struct {
      const char* fileName;
      int lineNumber;
    } location = {report.file, report.line};
    KJ_LOG_AT(ERROR, location, kj::str(text), suppressedSinceLast);
  }
  if (dropped != 0) {
    KJ_LOG(ERROR,
        "a turn's hostcalls made more refusal reports than the turn holds; the rest "
        "are lost",
        dropped);
  }
}

// The RESOLVE hostcall: a turn whose journal has run out of reserved entries, or whose retire
// list or notes are full, asks the host to refill or flush it. The handler finds the thread's
// pool and journal through the host's own record, never through the frame.
enum class ResolveOp : uint64_t { REFILL = 1, RETIRE = 2, NOTES = 3, LOG = 4 };

long resolveHostcall(hostcall::Frame& frame) {
  if (static_cast<ResolveOp>(frame.args[0]) == ResolveOp::LOG) {
    // The verdict on a report the turn's own code is about to make. The budget is in gk's
    // control block, which ring 3 cannot reach, so the turn pays an exit to be told whether to
    // speak -- the same exit the report's own write would have cost it, and the only thing it
    // buys with the exit when the answer is no is silence. The site id is all the frame carries
    // and gk validates it; nothing here depends on a turn being in progress.
#ifdef WORKERD_HAS_GUEST_KERNEL
    return gk_log_budget(static_cast<unsigned>(frame.args[1]));
#else
    return 1;
#endif
  }
  ThreadPool* pool = poolOfThread();
  if (pool == nullptr || !pool->journal->active) return -EINVAL;
  switch (static_cast<ResolveOp>(frame.args[0])) {
    case ResolveOp::REFILL:
      // The pool is spent, not merely low: topping it up would add nothing while every entry
      // in it is still held by a position of the journal. So the spent positions are settled
      // first, which commits what the turn registered through them and gives the entries it
      // did not keep back to the table, and the turn goes on over fresh reservations.
      settleJournal(*pool, true);
      refillPool(*pool);
      startJournal(*pool);
      return static_cast<long>(pool->journal->count);
    case ResolveOp::RETIRE:
      flushRetired(*pool->journal, pool->hooks, pool->turnArena);
      return 0;
    case ResolveOp::NOTES:
      replayNotes(*pool->journal, pool->hooks, true);
      return static_cast<long>(pool->journal->noteCount);
    case ResolveOp::LOG:
      break;  // answered above, before the pool is looked for
  }
  return -EINVAL;
}

const hostcall::Entry RESOLVE_HOSTCALL = {hostcall::RESOLVE, &resolveHostcall};

long callResolveHostcall(ResolveOp op, uint64_t arg = 0) {
  hostcall::Frame frame;
  frame.args[0] = static_cast<uint64_t>(op);
  frame.args[1] = arg;
  frame.args[2] = frame.args[3] = 0;
  frame.inLen = 0;
  return hostcall::call(RESOLVE_HOSTCALL, frame);
}

// The budget when there is no guest kernel: the same policy over process memory, which is as
// safe as it needs to be, since without a guest nothing is forging the words these paths refuse.
struct FallbackBucket {
  std::atomic<uint64_t> window{0};
  std::atomic<uint32_t> allowed{0};
  std::atomic<uint32_t> suppressed{0};
};
constexpr unsigned FALLBACK_SITES = 64;
constexpr uint32_t FALLBACK_BURST = 8;
FallbackBucket fallbackBuckets[FALLBACK_SITES];

uint32_t fallbackLogBudget(unsigned site) {
  if (site >= FALLBACK_SITES) return 1;
  FallbackBucket& bucket = fallbackBuckets[site];
  uint64_t now =
      (kj::systemCoarseMonotonicClock().now() - kj::origin<kj::TimePoint>()) / kj::SECONDS;
  uint64_t was = bucket.window.load(std::memory_order_relaxed);
  if (now != was && bucket.window.compare_exchange_strong(was, now, std::memory_order_relaxed)) {
    bucket.allowed.store(1, std::memory_order_relaxed);
    return 1 + bucket.suppressed.exchange(0, std::memory_order_relaxed);
  }
  if (bucket.allowed.load(std::memory_order_relaxed) < FALLBACK_BURST) {
    bucket.allowed.fetch_add(1, std::memory_order_relaxed);
    return 1 + bucket.suppressed.exchange(0, std::memory_order_relaxed);
  }
  bucket.suppressed.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

}  // namespace

bool inHandler() {
#ifdef WORKERD_GK_NEG_REPORT_DEFER
  // WORKERD_GK_NEG_REPORT_DEFER is a negative-test build, never shipped: a report made under a
  // hostcall handler is formatted there, which is what these paths did before they waited for
  // the end of the turn. The handler_alloc_guard build must then abort on the first refusal a
  // handler finds; this is the check that the guard build tests the waiting.
  return false;
#endif
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_in_handler() != 0;
#endif
  return false;
}

void _::appendReportChars(char* buf, size_t& n, kj::ArrayPtr<const char> chars) {
  size_t room = kj::min(chars.size(), _::REPORT_TEXT - n);
  memcpy(buf + n, chars.begin(), room);
  n += room;
}

void _::appendReportLabel(char* buf, size_t& n, const char*& macroArgs, bool first) {
  const char* start = macroArgs;
  while (*start == ' ') ++start;
  // The end of this name: the next comma outside brackets and outside a string.
  const char* pos = start;
  unsigned depth = 0;
  bool quoted = false;
  for (; *pos != '\0'; ++pos) {
    if (quoted) {
      if (*pos == '\\' && pos[1] != '\0')
        ++pos;
      else if (*pos == '"')
        quoted = false;
    } else if (*pos == '(')
      ++depth;
    else if (*pos == ')')
      --depth;
    else if (*pos == '"')
      quoted = true;
    else if (*pos == ',' && depth == 0)
      break;
  }
  macroArgs = *pos == ',' ? pos + 1 : pos;
  if (!first) appendReportChars(buf, n, "; "_kj);
  // kj's rule: a string literal, and anything already run through kj::str(), carries its value
  // alone; everything else is named.
  kj::ArrayPtr<const char> name(start, pos - start);
  if (name.size() > 0 && name[0] != '"' && !name.startsWith("kj::str("_kjc)) {
    appendReportChars(buf, n, name);
    appendReportChars(buf, n, " = "_kj);
  }
}

void deferReport(const char* file, int line, kj::ArrayPtr<const char> text, uint32_t suppressed) {
  ThreadPool* pool = poolOfThread();
  if (pool == nullptr) {
    // No pool is no turn to wait for, which the handlers that reach these paths are always in.
    // Writing it here is the allocation this exists to avoid, and losing the report is worse.
    uint32_t suppressedSinceLast = suppressed;
    const struct {
      const char* fileName;
      int lineNumber;
    } location = {file, line};
    KJ_LOG_AT(ERROR, location, kj::str(text), suppressedSinceLast);
    return;
  }
  if (pool->reportCount >= REPORT_CAPACITY) {
    pool->reportsDropped++;
    return;
  }
  DeferredReport& report = pool->reports[pool->reportCount++];
  report.file = file;
  report.line = line;
  report.suppressed = suppressed;
  report.length = kj::min(text.size(), _::REPORT_TEXT);
  memcpy(report.text, text.begin(), report.length);
}

uint32_t logBudget(LogSite site) {
#ifdef WORKERD_GK_NEG_LOG_CAP
  // WORKERD_GK_NEG_LOG_CAP is a negative-test build, never shipped: every report is made, which
  // is what these paths did before they were budgeted. The guest kernel's log-flood-test.sh must
  // then fail -- a worker that forces refusals in a loop draws as much log as it can make turns
  // for; this is the check that the test tests the budget.
  (void)site;
  return 1;
#else
  unsigned id = static_cast<unsigned>(site);
#ifdef WORKERD_HAS_GUEST_KERNEL
  static_assert(static_cast<unsigned>(LogSite::SLOT_RESOLVE) == GK_LOG_SITES_GK,
      "the runtime's report sites continue gk's");
  static_assert(static_cast<unsigned>(LogSite::TEARDOWN_OBJECTS) < GK_LOG_SITES,
      "the report sites fit the budgets gk keeps for them");
  if (guestKernelActive()) {
    long verdict = gk_log_budget(id);
    // -EPERM is gk saying the caller is at ring 3, where the budget is out of reach: the turn
    // asks the host for the verdict through the hostcall its journal already uses.
    if (verdict == -EPERM) verdict = callResolveHostcall(ResolveOp::LOG, id);
    // A verdict that could not be had is a report made: the thing these reports exist for must
    // not be lost to a failure of the thing that counts them.
    return verdict < 0 ? 1 : static_cast<uint32_t>(verdict);
  }
#endif
  return fallbackLogBudget(id);
#endif
}

namespace {

// The turn's journal, if a turn is running on this thread (as the turn's own bookkeeping says).
TurnJournal* activeJournal() {
  TurnJournal* journal = currentJournal;
  return journal != nullptr && journal->active ? journal : nullptr;
}

}  // namespace

ResolveHandle registerResolve(void* ptr, uint64_t type) {
  uint64_t aux[RESOLVE_AUX_WORDS] = {};
  return registerResolve(ptr, type, aux);
}

ResolveHandle registerResolveFor(IsolateArena arena, void* ptr, uint64_t type) {
  KJ_REQUIRE(ptr != nullptr);
  KJ_REQUIRE(!inTurn(), "registerResolveFor is host-side; a turn registers from its own pool");
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    return gk_resolve_register_for(static_cast<gk_arena*>(arena), ptr, type);
  }
#endif
  return fallbackRegisterResolve(ptr, type);
}

bool disownResolve(ResolveHandle handle, void* ptr, uint64_t type) {
  // No inTurn() bar: the commit hook that disowns a context entry runs while the journal it
  // came from is still being processed, which is host-side all the same.
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_disown(handle, ptr, type) == 0;
#endif
  return true;
}

bool adoptResolve(ResolveHandle handle, void* ptr, uint64_t type) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_adopt(handle, ptr, type) == 0;
#endif
  return true;
}

ResolveHandle registerResolve(void* ptr, uint64_t type, const uint64_t (&aux)[RESOLVE_AUX_WORDS]) {
  KJ_REQUIRE(ptr != nullptr);
  if (TurnJournal* journal = activeJournal()) {
    if (journal->next >= journal->count) {
      // The reserved entries are spent: one exit to refill, which a turn making more than
      // POOL_CAPACITY registrations pays once per POOL_CAPACITY.
      if (callResolveHostcall(ResolveOp::REFILL) <= 0 || journal->next >= journal->count) {
        return 0;
      }
    }
    uint32_t i = journal->next++;
    journal->ptr[i] = ptr;
    journal->type[i] = type;
    for (size_t w = 0; w < RESOLVE_AUX_WORDS; w++) journal->aux[i][w] = aux[w];
    journal->state[i] = TurnJournal::USED;
    return journal->handles[i];
  }
  return tableRegister(ptr, type);
}

void* resolveOfType(ResolveHandle handle, uint64_t type) {
  if (handle == 0) return nullptr;
  uint64_t actual = RESOLVE_TYPE_NONE;
  void* ptr = tableResolveTyped(handle, &actual);
  if (ptr != nullptr) {
    if (type == RESOLVE_TYPE_NONE || actual == type || !CHECK_RESOLVE_TYPE) return ptr;
    // A live entry of this isolate, holding something else: a handle the caller did not get
    // from the registration it thinks it is using.
    return nullptr;
  }
  // Not in the table: an entry this turn handed out and has not yet committed, if the journal
  // says so. The journal is the turn's own memory, trusted only by the turn itself, and only
  // until the host commits it -- including the tag, so this path's comparison is the turn's
  // own word against itself, and what it catches is a swap and not a lie.
  if (TurnJournal* journal = activeJournal()) {
    for (uint32_t i = kj::min(journal->next, journal->count); i-- > 0;) {
      if (journal->handles[i] == handle && journal->state[i] == TurnJournal::USED) {
        if (type == RESOLVE_TYPE_NONE || journal->type[i] == type || !CHECK_RESOLVE_TYPE) {
          return journal->ptr[i];
        }
        return nullptr;
      }
    }
  }
#ifdef WORKERD_GK_NEG_IOOWN
  // WORKERD_GK_NEG_IOOWN is a negative-test build, never shipped: a handle the table does not
  // validate is taken as the pointer itself, which is what a holder's raw pointer was before it
  // held a handle. The guest kernel's ioown-test.sh must then fail, with the host reading through
  // the address a turn chose; this is the check that the test tests the validation.
  return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
#else
  return nullptr;
#endif
}

void* resolve(ResolveHandle handle) {
  return resolveOfType(handle, RESOLVE_TYPE_NONE);
}

void unregisterResolve(ResolveHandle handle) {
  if (handle == 0) return;
  if (TurnJournal* journal = activeJournal()) {
    for (uint32_t i = kj::min(journal->next, journal->count); i-- > 0;) {
      if (journal->handles[i] == handle && journal->state[i] == TurnJournal::USED) {
        journal->state[i] = TurnJournal::RETIRED;  // never committed; released after the turn
        journal->ptr[i] = nullptr;
        return;
      }
    }
    if (journal->retireCount >= RETIRE_CAPACITY) {
      callResolveHostcall(ResolveOp::RETIRE);
      if (journal->retireCount >= RETIRE_CAPACITY) {
        return;  // the flush was refused; the entry leaks
      }
    }
    journal->retire[journal->retireCount++] = handle;
    return;
  }
  tableUnregister(handle);
}

void retireResolve(ResolveHandle handle) {
  if (handle == 0) return;
  tableUnregister(handle);
}

void flushReleasedInTurn() {
  TurnJournal* journal = activeJournal();
  if (journal == nullptr || journal->retireCount == 0) return;
  callResolveHostcall(ResolveOp::RETIRE);
}

bool noteInTurn(const uint64_t (&words)[NOTE_WORDS]) {
  TurnJournal* journal = activeJournal();
  if (journal == nullptr) return false;
  if (journal->noteCount >= NOTE_CAPACITY) {
    // The notes are spent: one exit to hand them over, which a turn making more than
    // NOTE_CAPACITY notes pays once per NOTE_CAPACITY.
    callResolveHostcall(ResolveOp::NOTES);
    if (journal->noteCount >= NOTE_CAPACITY) return false;
  }
  uint32_t i = journal->noteCount++;
  for (size_t w = 0; w < NOTE_WORDS; w++) journal->notes[i][w] = words[w];
  return true;
}

uint64_t* lastNoteForTest() {
  TurnJournal* journal = activeJournal();
  if (journal == nullptr) return nullptr;
  uint32_t n = kj::min(journal->noteCount, NOTE_CAPACITY);
  if (n == 0) return nullptr;
  return journal->notes[n - 1];
}

size_t liveResolveEntries() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    gk_stats stats;
    gk_get_stats(&stats);
    return stats.resolve_entries;
  }
#endif
  return fallbackLiveResolveEntries();
}

bool inTurn() {
  return activeJournal() != nullptr;
}

bool ownedByTurn(ResolveHandle handle) {
  ThreadPool* pool = poolOfThread();
  // No journal is being processed, so the handle did not come out of one and there is nothing
  // to distrust: a hook the host runs for its own registration sees this.
  if (pool == nullptr || !pool->journal->active) return true;
  if (tableOwnedBy(pool->turnArena, handle)) return true;
  // Not the turn's, but it names nothing either: the operation refuses it in its own terms.
  return tableResolve(handle) == nullptr;
}

IsolateArena currentIsolateArena() {
  return currentArena();
}

bool ownedByCurrentArena(ResolveHandle handle) {
  void* arena = currentArena();
  if (arena == nullptr) return tableResolve(handle) != nullptr;
  return tableOwnedBy(arena, handle);
}

bool arenaRangeOk(const void* addr, size_t len) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_arena_range_ok(addr, len) != 0;
#endif
  return true;
}

void releaseJournaled(
    kj::FunctionParam<bool(ResolveHandle handle, void* ptr, const uint64_t* aux)> fn) {
  TurnJournal* journal = activeJournal();
  if (journal == nullptr) return;
  uint32_t next = kj::min(journal->next, journal->count);
  for (uint32_t i = 0; i < next; i++) {
    if (journal->state[i] != TurnJournal::USED) continue;
    if (!fn(journal->handles[i], journal->ptr[i], journal->aux[i])) continue;
    journal->state[i] = TurnJournal::RETIRED;
    journal->ptr[i] = nullptr;
    for (size_t w = 0; w < RESOLVE_AUX_WORDS; w++) journal->aux[i][w] = 0;
  }
}

ResolveTurn::ResolveTurn(ResolveTurnHooks hooks): pool(nullptr) {
  // Nested in a turn: this frame is inside the guest, and the outermost frame commits.
  if (activeJournal() != nullptr) return;
  ThreadPool& threadPool = ensurePoolOfThread();
  if (threadPool.count < POOL_LOW) refillPool(threadPool);
  threadPool.hooks = hooks;
  threadPool.turnArena = currentArena();
  threadPool.refusalCount = 0;
  TurnJournal& journal = *threadPool.journal;
  journal.next = 0;
  startJournal(threadPool);
  journal.retireCount = 0;
  journal.noteCount = 0;
  journal.active = true;
  currentJournal = &journal;
  pool = &threadPool;
}

namespace {
// Host memory would be the place for a pointer the host calls through, but this one is set once
// before any turn runs and never again, so it is a plain static the way the handler table's
// shim context is: a turn that rewrote it would redirect a call the host makes after the turn,
// which is the status of every function pointer in the runtime's writable data.
void (*turnEndHook)() = nullptr;
}  // namespace

void setTurnEndHook(void (*hook)()) {
  KJ_REQUIRE(turnEndHook == nullptr || hook == nullptr, "the turn-end hook is set once");
  turnEndHook = hook;
}

ResolveTurn::~ResolveTurn() noexcept(false) {
  if (pool == nullptr) return;
  // Everything below reads the pool and its journal through this frame's pointer, which the
  // host wrote at entry; nothing the turn left in the thread-local decides what is committed.
  ThreadPool& threadPool = *static_cast<ThreadPool*>(pool);
  TurnJournal& journal = *threadPool.journal;
  const ResolveTurnHooks hooks = threadPool.hooks;
  // What the turn's hostcalls found and could not say there, first, so that the reports read
  // in the order the refusals happened: everything below runs host-side and writes its own.
  flushReports(threadPool);
  settleJournal(threadPool, false);
  // What a settle inside a refill hostcall could not hand to the owner there.
  flushRefusals(threadPool);
  flushRetired(journal, hooks, threadPool.turnArena);
  // The notes last: one about an entry registered above finds it committed, one about an entry
  // released above finds it gone.
  replayNotes(journal, hooks, false);
  threadPool.hooks = {nullptr, nullptr, nullptr};
  threadPool.turnArena = nullptr;
  journal.active = false;
  currentJournal = nullptr;
  // Last, outside the turn's bookkeeping: what a handler of this turn left for the host to do.
  if (turnEndHook != nullptr) turnEndHook();
}

void installResolveHostcall() {
  hostcall::install(RESOLVE_HOSTCALL);
  // The companion directory's slot is a function-local static, so the first use allocates it
  // from the process heap. The first use is inside a turn's refill, in a hostcall handler,
  // which must not take the heap (see hostcall.h), so it is made here instead, host-side,
  // before any turn runs -- as the host entropy buffer's state is (server/actor-id-impl.c++).
  companionDirectory();
}

bool setResolveCompanion(ResolveHandle handle, ResolveCompanion companion) {
  KJ_IF_SOME(c, companionOf(handle, true)) {
    c = companion;
    return true;
  }
  WD_LOG_REFUSAL(LogSite::COMPANION_SET,
      "resolve handle does not name a live committed entry; no companion is set", kj::hex(handle));
  return false;
}

kj::Maybe<ResolveCompanion&> resolveCompanion(ResolveHandle handle) {
  KJ_IF_SOME(c, companionOf(handle, false)) {
    if (c.kind != 0) return c;
  }
  return kj::none;
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
