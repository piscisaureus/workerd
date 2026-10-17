// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "alloc-census.h"

#ifdef WD_ALLOC_CENSUS

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
void* __real_malloc(size_t size);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t size);
void __real_free(void* p);
}

namespace workerd::allocCensus {
namespace {

constexpr size_t kSiteSlots = 1u << 17;   // 131072 call sites
constexpr size_t kBlockSlots = 1u << 24;  // 16M live blocks
constexpr size_t kIsolateSlots = 4096;
constexpr uintptr_t kTombstone = 1;

// Allocation context buckets, by what was entered on the thread when the allocation was made.
enum Ctx : unsigned { CTX_NONE = 0, CTX_LOCK = 1, CTX_TURN = 2, CTX_COUNT = 3 };

constexpr unsigned kFrames = 10;

struct Stack {
  uintptr_t pc[kFrames];
};

bool plausibleFrame(const void* p) {
  auto a = reinterpret_cast<uintptr_t>(p);
  return a > 0x10000 && (a & 7) == 0;
}

// Walks the frame-pointer chain for the frames above the interposer. The build sets
// -fno-omit-frame-pointer; a frame that does not look like one ends the walk, so a caller built
// without frame pointers truncates the site rather than faulting.
Stack captureStack(void* ra0, void* fp0) {
  Stack st{};
  st.pc[0] = reinterpret_cast<uintptr_t>(ra0);
  void** fp = static_cast<void**>(fp0);
  for (unsigned i = 1; i < kFrames; i++) {
    if (!plausibleFrame(fp)) break;
    void** next = static_cast<void**>(fp[0]);
    if (!plausibleFrame(next)) break;
    if (next <= fp ||
        reinterpret_cast<uintptr_t>(next) - reinterpret_cast<uintptr_t>(fp) > (1u << 22)) {
      break;
    }
    st.pc[i] = reinterpret_cast<uintptr_t>(next[1]);
    fp = next;
  }
  return st;
}

struct Site {
  std::atomic<uintptr_t> pc;
  uintptr_t frame[kFrames];
  std::atomic<uint64_t> allocs[CTX_COUNT];
  std::atomic<uint64_t> bytes;
  // Frees, by the relation between the isolate locked at the free and the one locked at the
  // allocation.
  std::atomic<uint64_t> freeSame;    // the same isolate
  std::atomic<uint64_t> freeOther;   // another isolate: a cross-isolate free
  std::atomic<uint64_t> freeHost;    // no lock held (the event loop, a background thread)
  std::atomic<uint64_t> freeOfHost;  // the block had no owner, freed under a lock
  std::atomic<uint64_t> freeInTurn;  // of the frees above, those made at ring 3
  // Bitmask of the first 64 isolate ids that allocated here, and that freed here.
  std::atomic<uint64_t> allocIsolates;
  std::atomic<uint64_t> freeIsolates;
  // Filled at dump time by the walk of the block table.
  uint64_t liveBlocks;
  uint64_t liveIsolateOwned;
  uint64_t liveBytes;
};

struct Block {
  std::atomic<uintptr_t> ptr;
  uint32_t site;
  uint32_t owner;  // 0 = none
  uint64_t size;
};

Site* sites = nullptr;
Block* blocks = nullptr;
std::atomic<const void*>* isolateIds = nullptr;
std::atomic<uint64_t> siteOverflow{0};
std::atomic<uint64_t> blockOverflow{0};
std::atomic<uint64_t> blockLost{0};
std::atomic<uint64_t> isolateOverflow{0};
std::atomic<uint64_t> freeUnknown{0};
std::atomic<int> initState{0};  // 0 = cold, 1 = running, 2 = ready
uintptr_t imageBase = 0;

thread_local bool inCensus = false;
thread_local unsigned isolateDepth = 0;
thread_local uint32_t isolateStack[16];
thread_local unsigned turnDepth = 0;

struct Reentry {
  bool saved;
  Reentry(): saved(inCensus) {
    inCensus = true;
  }
  ~Reentry() {
    inCensus = saved;
  }
};

void* mapZeroed(size_t bytes) {
  void* p = mmap(
      nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

void dumpSignal(int);

void initTables() {
  int expected = 0;
  if (!initState.compare_exchange_strong(expected, 1)) return;
  sites = static_cast<Site*>(mapZeroed(kSiteSlots * sizeof(Site)));
  blocks = static_cast<Block*>(mapZeroed(kBlockSlots * sizeof(Block)));
  isolateIds = static_cast<std::atomic<const void*>*>(
      mapZeroed(kIsolateSlots * sizeof(std::atomic<const void*>)));
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&initTables), &info) != 0) {
    imageBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
  }
  initState.store(2);
  signal(SIGUSR2, dumpSignal);
  atexit([]() { dump("atexit"); });
}

__attribute__((constructor(101), used)) void censusCtor() {
  initTables();
}

bool ready() {
  return initState.load(std::memory_order_acquire) == 2 && sites != nullptr && blocks != nullptr;
}

uint64_t mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

uint32_t siteIndex(const Stack& st) {
  uint64_t key = 0;
  for (unsigned f = 0; f < kFrames; f++) key = mix(key ^ st.pc[f]);
  if (key == 0) key = 1;
  uint64_t h = mix(key);
  for (uint32_t i = 0; i < 64; i++) {
    uint32_t slot = (h + i) & (kSiteSlots - 1);
    uintptr_t cur = sites[slot].pc.load(std::memory_order_acquire);
    if (cur == key) return slot;
    if (cur == 0) {
      uintptr_t zero = 0;
      if (sites[slot].pc.compare_exchange_strong(zero, key)) {
        for (unsigned f = 0; f < kFrames; f++) sites[slot].frame[f] = st.pc[f];
        return slot;
      }
      if (sites[slot].pc.load() == key) return slot;
    }
  }
  siteOverflow.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

uint32_t currentIsolate() {
  return isolateDepth == 0 ? 0 : isolateStack[isolateDepth - 1];
}

Ctx currentCtx() {
  if (turnDepth > 0) return CTX_TURN;
  if (isolateDepth > 0) return CTX_LOCK;
  return CTX_NONE;
}

void recordAlloc(void* p, size_t size, const Stack& st) {
  if (p == nullptr || !ready()) return;
  uint32_t si = siteIndex(st);
  uint32_t owner = currentIsolate();
  Site& s = sites[si];
  s.allocs[currentCtx()].fetch_add(1, std::memory_order_relaxed);
  s.bytes.fetch_add(size, std::memory_order_relaxed);
  if (owner != 0 && owner <= 64) {
    s.allocIsolates.fetch_or(1ULL << (owner - 1), std::memory_order_relaxed);
  }
  uintptr_t key = reinterpret_cast<uintptr_t>(p);
  uint64_t h = mix(key >> 4);
  int32_t tomb = -1;
  for (uint32_t i = 0; i < 128; i++) {
    uint32_t slot = (h + i) & (kBlockSlots - 1);
    uintptr_t cur = blocks[slot].ptr.load(std::memory_order_acquire);
    if (cur == kTombstone) {
      if (tomb < 0) tomb = static_cast<int32_t>(slot);
      continue;
    }
    if (cur == 0 || cur == key) {
      uintptr_t expected = cur;
      if (blocks[slot].ptr.compare_exchange_strong(expected, key)) {
        blocks[slot].site = si;
        blocks[slot].owner = owner;
        blocks[slot].size = size;
        return;
      }
      continue;  // lost the race for this slot; try the next
    }
  }
  if (tomb >= 0) {
    uintptr_t expected = kTombstone;
    if (blocks[tomb].ptr.compare_exchange_strong(expected, key)) {
      blocks[tomb].site = si;
      blocks[tomb].owner = owner;
      blocks[tomb].size = size;
      return;
    }
  }
  blockOverflow.fetch_add(1, std::memory_order_relaxed);
}

void recordFree(void* p) {
  if (p == nullptr || !ready()) return;
  uintptr_t key = reinterpret_cast<uintptr_t>(p);
  uint64_t h = mix(key >> 4);
  uint32_t locked = currentIsolate();
  for (uint32_t i = 0; i < 128; i++) {
    uint32_t slot = (h + i) & (kBlockSlots - 1);
    uintptr_t cur = blocks[slot].ptr.load(std::memory_order_acquire);
    if (cur == 0) {
      freeUnknown.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (cur != key) continue;
    uint32_t si = blocks[slot].site;
    uint32_t owner = blocks[slot].owner;
    if (!blocks[slot].ptr.compare_exchange_strong(cur, kTombstone)) {
      blockLost.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    Site& s = sites[si];
    if (owner == 0 && locked != 0) {
      s.freeOfHost.fetch_add(1, std::memory_order_relaxed);
    } else if (locked == 0) {
      s.freeHost.fetch_add(1, std::memory_order_relaxed);
    } else if (locked == owner) {
      s.freeSame.fetch_add(1, std::memory_order_relaxed);
    } else {
      s.freeOther.fetch_add(1, std::memory_order_relaxed);
    }
    if (turnDepth > 0) s.freeInTurn.fetch_add(1, std::memory_order_relaxed);
    if (locked != 0 && locked <= 64) {
      s.freeIsolates.fetch_or(1ULL << (locked - 1), std::memory_order_relaxed);
    }
    return;
  }
  freeUnknown.fetch_add(1, std::memory_order_relaxed);
}

void* censusAlloc(size_t size, const Stack& st) {
  void* p = __real_malloc(size);
  if (!inCensus) {
    Reentry guard;
    recordAlloc(p, size, st);
  }
  return p;
}

void censusFree(void* p) {
  if (!inCensus) {
    Reentry guard;
    recordFree(p);
  }
  __real_free(p);
}

// ---- the dump --------------------------------------------------------------------------

char outBuf[1 << 16];
size_t outLen = 0;
int outFd = -1;

void flushOut() {
  size_t off = 0;
  while (off < outLen) {
    ssize_t n = write(outFd, outBuf + off, outLen - off);
    if (n <= 0) break;
    off += static_cast<size_t>(n);
  }
  outLen = 0;
}

void emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void emit(const char* fmt, ...) {
  if (outLen > sizeof(outBuf) - 1024) flushOut();
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(outBuf + outLen, sizeof(outBuf) - outLen, fmt, ap);
  va_end(ap);
  if (n > 0) outLen += static_cast<size_t>(n);
}

std::atomic<int> dumping{0};

void dumpSignal(int) {
  dump("signal");
}

}  // namespace

void enterIsolate(const void* isolate) {
  if (!ready()) initTables();
  if (isolateIds == nullptr) return;
  uint32_t id = 0;
  for (uint32_t i = 0; i < kIsolateSlots; i++) {
    const void* cur = isolateIds[i].load(std::memory_order_acquire);
    if (cur == isolate) {
      id = i + 1;
      break;
    }
    if (cur == nullptr) {
      const void* null = nullptr;
      if (isolateIds[i].compare_exchange_strong(null, isolate) || isolateIds[i].load() == isolate) {
        id = i + 1;
        break;
      }
    }
  }
  if (id == 0) isolateOverflow.fetch_add(1, std::memory_order_relaxed);
  if (isolateDepth < 16) isolateStack[isolateDepth] = id;
  isolateDepth++;
}

void leaveIsolate() {
  if (isolateDepth > 0) isolateDepth--;
}

void enterTurn() {
  turnDepth++;
}

void leaveTurn() {
  if (turnDepth > 0) turnDepth--;
}

void dump(const char* why) {
  if (!ready()) return;
  int expected = 0;
  if (!dumping.compare_exchange_strong(expected, 1)) return;
  Reentry guard;
  const char* path = getenv("WD_ALLOC_CENSUS_OUT");
  if (path == nullptr) path = "/tmp/alloc-census.txt";
  outFd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (outFd < 0) {
    dumping.store(0);
    return;
  }
  outLen = 0;

  // The walk of the block table: what is still live, and who owns it.
  uint64_t liveTotal = 0, liveIsolateTotal = 0;
  for (size_t i = 0; i < kBlockSlots; i++) {
    uintptr_t p = blocks[i].ptr.load(std::memory_order_relaxed);
    if (p == 0 || p == kTombstone) continue;
    Site& s = sites[blocks[i].site];
    s.liveBlocks++;
    s.liveBytes += blocks[i].size;
    liveTotal++;
    if (blocks[i].owner != 0) {
      s.liveIsolateOwned++;
      liveIsolateTotal++;
    }
  }

  uint64_t isolateCount = 0;
  for (uint32_t i = 0; i < kIsolateSlots; i++) {
    if (isolateIds[i].load(std::memory_order_relaxed) != nullptr) isolateCount++;
  }

  emit("# alloc-census why=%s imageBase=0x%lx isolateIds=%lu\n", why, (unsigned long)imageBase,
      (unsigned long)isolateCount);
  emit("# siteOverflow=%lu blockOverflow=%lu blockLost=%lu freeUnknown=%lu isolateOverflow=%lu\n",
      (unsigned long)siteOverflow.load(), (unsigned long)blockOverflow.load(),
      (unsigned long)blockLost.load(), (unsigned long)freeUnknown.load(),
      (unsigned long)isolateOverflow.load());
  emit("# live=%lu liveIsolateOwned=%lu\n", (unsigned long)liveTotal,
      (unsigned long)liveIsolateTotal);
  emit("# fields: frames aNone aLock aTurn bytes fSame fOther fHost fOfHost fInTurn "
       "nAllocIsolates nFreeIsolates live liveIsolateOwned liveBytes\n");
  for (size_t i = 0; i < kSiteSlots; i++) {
    uintptr_t pc = sites[i].pc.load(std::memory_order_relaxed);
    if (pc == 0) continue;
    Site& s = sites[i];
    uint64_t a0 = s.allocs[0].load(), a1 = s.allocs[1].load(), a2 = s.allocs[2].load();
    if (a0 + a1 + a2 == 0) continue;
    emit(
        "S %lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %lu %lu %lu\n",
        (unsigned long)(s.frame[0] - imageBase), (unsigned long)(s.frame[1] - imageBase),
        (unsigned long)(s.frame[2] - imageBase), (unsigned long)(s.frame[3] - imageBase),
        (unsigned long)(s.frame[4] - imageBase), (unsigned long)(s.frame[5] - imageBase),
        (unsigned long)(s.frame[6] - imageBase), (unsigned long)(s.frame[7] - imageBase),
        (unsigned long)(s.frame[8] - imageBase), (unsigned long)(s.frame[9] - imageBase),
        (unsigned long)a0, (unsigned long)a1, (unsigned long)a2, (unsigned long)s.bytes.load(),
        (unsigned long)s.freeSame.load(), (unsigned long)s.freeOther.load(),
        (unsigned long)s.freeHost.load(), (unsigned long)s.freeOfHost.load(),
        (unsigned long)s.freeInTurn.load(), __builtin_popcountll(s.allocIsolates.load()),
        __builtin_popcountll(s.freeIsolates.load()), (unsigned long)s.liveBlocks,
        (unsigned long)s.liveIsolateOwned, (unsigned long)s.liveBytes);
    s.liveBlocks = s.liveIsolateOwned = s.liveBytes = 0;
  }
  flushOut();
  close(outFd);
  outFd = -1;
  dumping.store(0);
}

}  // namespace workerd::allocCensus

// ---- the interposed allocator ------------------------------------------------------------

using workerd::allocCensus::censusAlloc;
using workerd::allocCensus::censusFree;

extern "C" {

#define WD_STACK_HERE                                                                              \
  workerd::allocCensus::captureStack(__builtin_return_address(0), __builtin_frame_address(0))

void* __wrap_malloc(size_t size) {
  return censusAlloc(size, WD_STACK_HERE);
}

void* __wrap_calloc(size_t n, size_t size) {
  void* p = __real_calloc(n, size);
  if (!workerd::allocCensus::inCensus) {
    workerd::allocCensus::Reentry guard;
    workerd::allocCensus::recordAlloc(p, n * size, WD_STACK_HERE);
  }
  return p;
}

void* __wrap_realloc(void* old, size_t size) {
  if (!workerd::allocCensus::inCensus) {
    workerd::allocCensus::Reentry guard;
    workerd::allocCensus::recordFree(old);
  }
  void* p = __real_realloc(old, size);
  if (!workerd::allocCensus::inCensus) {
    workerd::allocCensus::Reentry guard;
    workerd::allocCensus::recordAlloc(p, size, WD_STACK_HERE);
  }
  return p;
}

void __wrap_free(void* p) {
  censusFree(p);
}

}  // extern "C"

void* operator new(size_t size) {
  void* p = censusAlloc(size, WD_STACK_HERE);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t size) {
  void* p = censusAlloc(size, WD_STACK_HERE);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
  return censusAlloc(size, WD_STACK_HERE);
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  return censusAlloc(size, WD_STACK_HERE);
}
void operator delete(void* p) noexcept {
  censusFree(p);
}
void operator delete[](void* p) noexcept {
  censusFree(p);
}
void operator delete(void* p, size_t) noexcept {
  censusFree(p);
}
void operator delete[](void* p, size_t) noexcept {
  censusFree(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  censusFree(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  censusFree(p);
}

namespace {
void* alignedAlloc(size_t size, size_t align, const workerd::allocCensus::Stack& st) {
  if (align < sizeof(void*)) align = sizeof(void*);
  size_t rounded = (size + align - 1) & ~(align - 1);
  void* p = aligned_alloc(align, rounded);
  if (!workerd::allocCensus::inCensus) {
    workerd::allocCensus::Reentry guard;
    workerd::allocCensus::recordAlloc(p, rounded, st);
  }
  return p;
}
}  // namespace

void* operator new(size_t size, std::align_val_t align) {
  void* p = alignedAlloc(size, static_cast<size_t>(align), WD_STACK_HERE);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t size, std::align_val_t align) {
  void* p = alignedAlloc(size, static_cast<size_t>(align), WD_STACK_HERE);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return alignedAlloc(size, static_cast<size_t>(align), WD_STACK_HERE);
}
void operator delete(void* p, std::align_val_t) noexcept {
  censusFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  censusFree(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
  censusFree(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
  censusFree(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  censusFree(p);
}

#endif  // WD_ALLOC_CENSUS
