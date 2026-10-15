// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "host-memory.h"

#include <workerd/util/hostcall.h>

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <kj/mutex.h>

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
};
static_assert(sizeof(FallbackResolveEntry) == 16);

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
ResolveHandle fallbackTake(FallbackResolveState& state, void* ptr) {
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
  entry.ptr.store(ptr, std::memory_order_release);
  entry.gen.store(gen, std::memory_order_release);
  state.live++;
  return (gen << RESOLVE_INDEX_BITS) | idx;
}

ResolveHandle fallbackRegisterResolve(void* ptr) {
  auto lock = fallbackResolveState().lockExclusive();
  return fallbackTake(*lock, ptr);
}

uint32_t fallbackReserveResolve(ResolveHandle* handles, uint32_t n) {
  auto lock = fallbackResolveState().lockExclusive();
  uint32_t got = 0;
  while (got < n) {
    ResolveHandle handle = fallbackTake(*lock, nullptr);
    if (handle == 0) break;
    handles[got++] = handle;
  }
  return got;
}

bool fallbackCommitResolve(ResolveHandle handle, void* ptr) {
  uint64_t idx = handle & ((uint64_t{1} << RESOLVE_INDEX_BITS) - 1);
  uint64_t gen = handle >> RESOLVE_INDEX_BITS;
  auto lock = fallbackResolveState().lockExclusive();
  auto& entry = fallbackResolveEntries[idx];
  if (gen == 0 || entry.gen.load(std::memory_order_relaxed) != gen ||
      entry.ptr.load(std::memory_order_relaxed) != nullptr) {
    KJ_LOG(ERROR, "resolve handle does not name a reserved entry; nothing is committed",
        kj::hex(handle));
    return false;
  }
  if (ptr == nullptr) {
    KJ_LOG(ERROR, "a null pointer cannot be committed to a resolve entry", kj::hex(handle));
    return false;
  }
  entry.ptr.store(ptr, std::memory_order_release);
  return true;
}

void* fallbackResolve(ResolveHandle handle) {
  uint64_t idx = handle & ((uint64_t{1} << RESOLVE_INDEX_BITS) - 1);
  uint64_t gen = handle >> RESOLVE_INDEX_BITS;
  if (gen == 0) return nullptr;
  auto& entry = fallbackResolveEntries[idx];  // the mask bounds idx
  uint64_t g1 = entry.gen.load(std::memory_order_acquire);
  void* ptr = entry.ptr.load(std::memory_order_acquire);
  uint64_t g2 = entry.gen.load(std::memory_order_acquire);
  return (g1 == gen && g2 == gen) ? ptr : nullptr;
}

void fallbackUnregisterResolve(ResolveHandle handle) {
  uint64_t idx = handle & ((uint64_t{1} << RESOLVE_INDEX_BITS) - 1);
  uint64_t gen = handle >> RESOLVE_INDEX_BITS;
  auto lock = fallbackResolveState().lockExclusive();
  auto& entry = fallbackResolveEntries[idx];
  if (gen == 0 || entry.gen.load(std::memory_order_relaxed) != gen) {
    KJ_LOG(ERROR, "resolve handle does not name a live entry; nothing is unregistered",
        kj::hex(handle));
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
ResolveHandle tableRegister(void* ptr) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_register(ptr);
#endif
  return fallbackRegisterResolve(ptr);
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
bool tableCommit(ResolveHandle handle, void* ptr) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve_commit(handle, ptr) == 0;
#endif
  return fallbackCommitResolve(handle, ptr);
}
void* tableResolve(ResolveHandle handle) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) return gk_resolve(handle);
#endif
  return fallbackResolve(handle);
}
void tableUnregister(ResolveHandle handle) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    gk_resolve_unregister(handle);
    return;
  }
#endif
  fallbackUnregisterResolve(handle);
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

struct TurnJournal {
  enum State : uint8_t { UNUSED = 0, USED = 1, RETIRED = 2 };
  ResolveHandle handles[POOL_CAPACITY];  // the turn's copy of the pool, to hand out
  void* ptr[POOL_CAPACITY];              // the pointer each handed-out position names
  State state[POOL_CAPACITY];
  uint32_t count = 0;                     // positions available (the pool's count, copied)
  uint32_t next = 0;                      // positions handed out
  ResolveHandle retire[RETIRE_CAPACITY];  // committed entries unlinked during the turn
  uint32_t retireCount = 0;
  bool active = false;  // a turn is running on this thread
};

struct ThreadPool {
  ResolveHandle handles[POOL_CAPACITY];
  uint32_t count = 0;
  kj::Own<TurnJournal> journal;
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

// Releases the thread's pool when the thread exits: its reserved entries and the block.
struct PoolReleaser {
  ~PoolReleaser() noexcept(false) {
    ThreadPool* pool = poolOfThread();
    if (pool == nullptr) return;
    setPoolOfThread(nullptr);
    for (uint32_t i = 0; i < pool->count; i++) tableUnregister(pool->handles[i]);
    pool->~ThreadPool();
    deallocate(pool, sizeof(ThreadPool), alignof(ThreadPool));
  }
};

// Reserves entries until the pool holds POOL_CAPACITY, or the table runs out, and copies the
// new ones into the journal. Host-side. Returns how many were added.
uint32_t refillPool(ThreadPool& pool) {
  uint32_t before = pool.count;
  pool.count += tableReserve(pool.handles + pool.count, POOL_CAPACITY - pool.count);
  TurnJournal& journal = *pool.journal;
  for (uint32_t i = before; i < pool.count; i++) {
    journal.handles[i] = pool.handles[i];
    journal.state[i] = TurnJournal::UNUSED;
  }
  journal.count = pool.count;
  return pool.count - before;
}

ThreadPool& ensurePoolOfThread() {
  ThreadPool* pool = poolOfThread();
  if (pool == nullptr) {
    pool = new (allocate(sizeof(ThreadPool), alignof(ThreadPool))) ThreadPool();
    pool->journal = kj::heap<TurnJournal>();
    setPoolOfThread(pool);
    static thread_local PoolReleaser releaser;
    (void)releaser;
  }
  return *pool;
}

// Retires the journal's committed-entry retire list. Host-side; bounded by the list's capacity
// whatever the journal says.
void flushRetired(TurnJournal& journal) {
  uint32_t n = kj::min(journal.retireCount, RETIRE_CAPACITY);
  for (uint32_t i = 0; i < n; i++) tableUnregister(journal.retire[i]);
  journal.retireCount = 0;
}

// The RESOLVE hostcall: a turn whose journal has run out of reserved entries, or whose retire
// list is full, asks the host to refill or flush it. The handler finds the thread's pool and
// journal through the host's own record, never through the frame.
enum class ResolveOp : uint64_t { REFILL = 1, RETIRE = 2 };

long resolveHostcall(hostcall::Frame& frame) {
  ThreadPool* pool = poolOfThread();
  if (pool == nullptr || !pool->journal->active) return -EINVAL;
  switch (static_cast<ResolveOp>(frame.args[0])) {
    case ResolveOp::REFILL:
      return static_cast<long>(refillPool(*pool));
    case ResolveOp::RETIRE:
      flushRetired(*pool->journal);
      return 0;
  }
  return -EINVAL;
}

const hostcall::Entry RESOLVE_HOSTCALL = {hostcall::RESOLVE, &resolveHostcall};

long callResolveHostcall(ResolveOp op) {
  hostcall::Frame frame;
  frame.args[0] = static_cast<uint64_t>(op);
  frame.args[1] = frame.args[2] = frame.args[3] = 0;
  frame.inLen = 0;
  return hostcall::call(RESOLVE_HOSTCALL, frame);
}

// The turn's journal, if a turn is running on this thread (as the turn's own bookkeeping says).
TurnJournal* activeJournal() {
  TurnJournal* journal = currentJournal;
  return journal != nullptr && journal->active ? journal : nullptr;
}

}  // namespace

// WORKERD_GK_NEG_IOOWN is a negative-test build, never shipped: the handle is the pointer itself
// and nothing validates it, which is the state before IoOwn held a handle. The guest kernel's
// ioown-test.sh must then fail, with the host reading through the address a turn chose; this is
// the check that the test tests the protection.
#ifdef WORKERD_GK_NEG_IOOWN
ResolveHandle registerResolve(void* ptr) {
  return reinterpret_cast<uintptr_t>(ptr);
}
void* resolve(ResolveHandle handle) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
}
void unregisterResolve(ResolveHandle) {}
size_t liveResolveEntries() {
  return 0;
}
ResolveTurn::ResolveTurn(): pool(nullptr) {}
ResolveTurn::~ResolveTurn() noexcept(false) {}
void installResolveHostcall() {}
#else
ResolveHandle registerResolve(void* ptr) {
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
    journal->state[i] = TurnJournal::USED;
    return journal->handles[i];
  }
  return tableRegister(ptr);
}

void* resolve(ResolveHandle handle) {
  if (handle == 0) return nullptr;
  void* ptr = tableResolve(handle);
  if (ptr != nullptr) return ptr;
  // Not in the table: an entry this turn handed out and has not yet committed, if the journal
  // says so. The journal is the turn's own memory, trusted only by the turn itself, and only
  // until the host commits it.
  if (TurnJournal* journal = activeJournal()) {
    for (uint32_t i = kj::min(journal->next, journal->count); i-- > 0;) {
      if (journal->handles[i] == handle && journal->state[i] == TurnJournal::USED) {
        return journal->ptr[i];
      }
    }
  }
  return nullptr;
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

ResolveTurn::ResolveTurn(): pool(nullptr) {
  // Nested in a turn: this frame is inside the guest, and the outermost frame commits.
  if (activeJournal() != nullptr) return;
  ThreadPool& threadPool = ensurePoolOfThread();
  if (threadPool.count < POOL_LOW) refillPool(threadPool);
  TurnJournal& journal = *threadPool.journal;
  for (uint32_t i = 0; i < threadPool.count; i++) {
    journal.handles[i] = threadPool.handles[i];
    journal.state[i] = TurnJournal::UNUSED;
  }
  journal.count = threadPool.count;
  journal.next = 0;
  journal.retireCount = 0;
  journal.active = true;
  currentJournal = &journal;
  pool = &threadPool;
}

ResolveTurn::~ResolveTurn() noexcept(false) {
  if (pool == nullptr) return;
  // Everything below reads the pool and its journal through this frame's pointer, which the
  // host wrote at entry; nothing the turn left in the thread-local decides what is committed.
  ThreadPool& threadPool = *static_cast<ThreadPool*>(pool);
  TurnJournal& journal = *threadPool.journal;
  uint32_t next = kj::min(journal.next, threadPool.count);
  for (uint32_t i = 0; i < next; i++) {
    ResolveHandle handle = threadPool.handles[i];
    if (journal.state[i] == TurnJournal::USED) {
      if (tableCommit(handle, journal.ptr[i])) continue;
      KJ_LOG(ERROR, "a turn's resolve entry could not be committed; it resolves to nothing",
          kj::hex(handle), journal.ptr[i]);
    }
    // Retired by the turn, never used, or refused by the table: the entry resolves to nothing
    // from now on, and so does any IoOwn holding it.
    tableUnregister(handle);
  }
  for (uint32_t i = next; i < threadPool.count; i++) {
    threadPool.handles[i - next] = threadPool.handles[i];
  }
  threadPool.count -= next;
  flushRetired(journal);
  journal.active = false;
  journal.next = 0;
  journal.count = 0;
  currentJournal = nullptr;
}

void installResolveHostcall() {
  hostcall::install(RESOLVE_HOSTCALL);
}
#endif  // WORKERD_GK_NEG_IOOWN

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
