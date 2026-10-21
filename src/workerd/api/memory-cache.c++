#include "memory-cache.h"

#include <workerd/api/global-scope.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/io/io-own.h>
#include <workerd/io/io-util.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/heap-arena.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/thread-scopes.h>

#include <kj/async.h>
#include <kj/debug.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

namespace workerd::api {

static constexpr size_t MAX_KEY_SIZE = 2 * 1024;

// =======================================================================================
// The hostcall's frame layouts. Every field is a 64-bit word so that nothing depends on the
// packing of the compiler that built either side; the structs are copied into and out of the
// frame's buffer with memcpy, never aliased into it.

namespace {

enum class ReadKind : uint64_t {
  MISS = 0,
  HIT = 1,        // the value follows ReadOut in the frame
  HIT_LARGE = 2,  // the value is pinned on this thread: READ_RANGE or READ_CHUNK fetches it
  LEAD = 3,       // the caller runs the fallback for the key, under the ticket
  WAIT = 4,       // another caller is; registered as a waiter when the call named a cookie
};
constexpr uint64_t READ_WITH_FALLBACK = 1;

struct ReadIn {
  uint64_t nowBits;  // the caller's clock as a double, for expiration
  uint64_t context;  // the resolve handle of the caller's IoContext, or 0
  uint64_t cookie;   // the resolve handle of the caller's waiter record, or 0
  uint64_t keyLen;   // the key follows
};
struct ReadOut {
  uint64_t kind;
  uint64_t valueSize;
  uint64_t ticket;
  uint64_t lockWaitNs;
  uint64_t totalValueSize;
  uint64_t entryCount;
  uint64_t waitersAhead;
  uint64_t inlineLen;
};
constexpr size_t INLINE_VALUE_MAX = hostcall::BYTES - sizeof(ReadOut);

enum class PublishMode : uint64_t { INLINE = 0, RANGE = 1 };
struct PublishIn {
  uint64_t nowBits;
  uint64_t expirationBits;
  uint64_t hasExpiration;
  uint64_t ticket;
  uint64_t valueSize;
  uint64_t keyLen;  // the key follows, then the value for an INLINE publish
};
enum class WriteOutcome : uint64_t {
  SUCCESS = 0,
  VALUE_TOO_LARGE = 1,
  ALREADY_EXPIRED = 2,
  NO_MEMORY = 3,  // the instance's pool is spent
  STALE = 4,      // the key is not in flight under the ticket: nothing stored
};
struct WriteOut {
  uint64_t outcome;
  uint64_t isUpdate;
  uint64_t evictions;        // all of them
  uint64_t packedEvictions;  // how many PackedEviction records follow
  uint64_t totalAfter;
  uint64_t entriesAfter;
  uint64_t maxValueSize;
  uint64_t waitersNotified;
  uint64_t lockWaitNs;
  uint64_t valueSize;
};
enum class EvictionReason : uint64_t { EXPIRATION = 0, LRU = 1 };
struct PackedEviction {
  uint64_t reason;
  uint64_t size;
  uint64_t totalBefore;
  uint64_t entriesBefore;
  uint64_t keyLen;  // the key follows
};
struct AbandonIn {
  uint64_t ticket;
  uint64_t keyLen;
};
struct DeleteIn {
  uint64_t keyLen;
};

double fromBits(uint64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}
uint64_t toBits(double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof(bits));
  return bits;
}

uint64_t hashKey(kj::ArrayPtr<const char> key) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (char c: key) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// Milliseconds since the epoch, as Date.now() has it: the IoContext's clock inside a request,
// the system's outside one (a binding's creation or destruction).
double cacheNow() {
  if (IoContext::hasCurrent()) return dateNow();
  return (kj::systemCoarseCalendarClock().now() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
}

uint64_t monotonicNs() {
  // A vDSO read: no lock, no allocation, so a handler may take it.
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

int64_t lockWaitNsForTrace(uint64_t lockWaitNs) {
  if (isPredictableModeForTest()) return 0;
  return static_cast<int64_t>(kj::min(lockWaitNs, static_cast<uint64_t>(INT64_MAX)));
}

// Whether a buffer the caller allocates now lies in its own arena, so that it can be named to
// the handler by address for the handler to copy into or out of directly (one exit) rather
// than a frame at a time. Under the isolate lock with per-isolate heap arenas every allocation
// is in the isolate's arena; the handler checks the range against gk's record of the arena, so
// a wrong answer here costs the caller its own operation and nothing else.
bool rangePathUsable() {
  return jsg::heapArenasAvailable() && hostMemory::inTurn();
}

}  // namespace

// =======================================================================================
// The instance's data, all of it in host memory: the entries in a chained hash table with a
// recency list through them and a min-heap of the ones that expire, the keys in flight in a
// second table with their waiters, and the pool everything is carved from.

struct SharedMemoryCache::Value {
  uint32_t refs;  // the entry's reference plus one per read that has it pinned
  uint32_t size;
  kj::byte* bytes() {
    return reinterpret_cast<kj::byte*>(this + 1);
  }
  static size_t blockSize(size_t size) {
    return sizeof(Value) + size;
  }
};

struct SharedMemoryCache::Entry {
  Entry* hashNext;
  Entry* lruPrev;
  Entry* lruNext;
  Value* value;
  uint64_t hash;
  double expiration;
  bool hasExpiration;
  uint32_t heapIndex;  // position in the expiration heap, or NO_HEAP
  uint32_t keyLen;
  static constexpr uint32_t NO_HEAP = ~uint32_t(0);
  char* key() {
    return reinterpret_cast<char*>(this + 1);
  }
  kj::ArrayPtr<const char> keyPtr() {
    return kj::arrayPtr(key(), keyLen);
  }
  static size_t blockSize(size_t keyLen) {
    return sizeof(Entry) + keyLen;
  }
};

struct SharedMemoryCache::Waiter {
  enum Kind : uint64_t { ACTION = 1, FULFILLER = 2 };
  Waiter* next;
  uint64_t kind;
  uint64_t context;  // ACTION: the waiter's IoContext, by resolve handle
  uint64_t cookie;   // ACTION: the waiter's record, by resolve handle
  // FULFILLER: a host-side waiter's fulfiller, for callers outside any turn.
  kj::Maybe<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> fulfiller;
};

struct SharedMemoryCache::InFlight {
  InFlight* hashNext;
  uint64_t hash;
  uint64_t ticket;
  const void* leader;  // the leader's binding record, compared and never dereferenced
  Waiter* head;
  Waiter* tail;
  uint32_t waiters;
  uint32_t keyLen;
  char* key() {
    return reinterpret_cast<char*>(this + 1);
  }
  kj::ArrayPtr<const char> keyPtr() {
    return kj::arrayPtr(key(), keyLen);
  }
  static size_t blockSize(size_t keyLen) {
    return sizeof(InFlight) + keyLen;
  }
};

// Waiters a publish or abandon set aside for the host to wake after the turn.
struct SharedMemoryCache::Wake {
  Wake* next;
  Waiter* head;
};

struct SharedMemoryCache::SuggestedLimit {
  SuggestedLimit* next;
  Limits limits;
  uint32_t count;
};

struct SharedMemoryCache::Data: public ThreadUnsafeData {
  hostMemory::HostPool pool;
  SuggestedLimit* suggested = nullptr;
  size_t totalValueSize = 0;
  size_t entryCount = 0;
  Entry** buckets = nullptr;
  size_t bucketCount = 0;
  Entry* lruHead = nullptr;  // least recently used
  Entry* lruTail = nullptr;  // most recently used
  Entry** heap = nullptr;    // the entries with an expiration, earliest first
  size_t heapCount = 0;
  size_t heapCapacity = 0;
  InFlight** inFlightBuckets = nullptr;
  size_t inFlightBucketCount = 0;
  size_t inFlightCount = 0;
  uint64_t nextTicket = 1;
  Wake* wakeHead = nullptr;
  Wake* wakeTail = nullptr;
  bool queuedForWake = false;
};

// What a handler keeps on its thread between calls (the frame's host-side state): a value a
// large read has pinned for READ_RANGE or READ_CHUNK to fetch, or a value a large publish is
// staging. The instance pointer is compared against the instance the next call resolves
// through its binding and never dereferenced on its own, since the state may outlive the
// instance when a turn never makes the call that would have released it.
struct SharedMemoryCache::PinState {
  uint64_t tag;
  const SharedMemoryCache* instance;
  Value* value;
  uint64_t offset;
  uint64_t staging;
};
static constexpr uint64_t PIN_TAG = 0x6d63616368652d70ull;
static_assert(sizeof(SharedMemoryCache::PinState) <= hostcall::STATE);

namespace {

using Data = SharedMemoryCache::Data;

template <typename T>
T* poolAllocate(Data& data, size_t bytes) {
  return static_cast<T*>(data.pool.allocate(bytes));
}

// ---- values ----

void releaseValue(Data& data, SharedMemoryCache::Value* value) {
  if (--value->refs == 0) {
    data.pool.deallocate(value, SharedMemoryCache::Value::blockSize(value->size));
  }
}

// ---- the recency list ----

void lruUnlink(Data& data, SharedMemoryCache::Entry* e) {
  if (e->lruPrev != nullptr) {
    e->lruPrev->lruNext = e->lruNext;
  } else {
    data.lruHead = e->lruNext;
  }
  if (e->lruNext != nullptr) {
    e->lruNext->lruPrev = e->lruPrev;
  } else {
    data.lruTail = e->lruPrev;
  }
  e->lruPrev = e->lruNext = nullptr;
}

void lruAppend(Data& data, SharedMemoryCache::Entry* e) {
  e->lruPrev = data.lruTail;
  e->lruNext = nullptr;
  if (data.lruTail != nullptr) {
    data.lruTail->lruNext = e;
  } else {
    data.lruHead = e;
  }
  data.lruTail = e;
}

// ---- the expiration heap ----

void heapSwap(Data& data, size_t a, size_t b) {
  auto* ea = data.heap[a];
  auto* eb = data.heap[b];
  data.heap[a] = eb;
  data.heap[b] = ea;
  eb->heapIndex = a;
  ea->heapIndex = b;
}

void heapSiftUp(Data& data, size_t i) {
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (data.heap[parent]->expiration <= data.heap[i]->expiration) break;
    heapSwap(data, parent, i);
    i = parent;
  }
}

void heapSiftDown(Data& data, size_t i) {
  for (;;) {
    size_t left = 2 * i + 1, right = left + 1, least = i;
    if (left < data.heapCount && data.heap[left]->expiration < data.heap[least]->expiration) {
      least = left;
    }
    if (right < data.heapCount && data.heap[right]->expiration < data.heap[least]->expiration) {
      least = right;
    }
    if (least == i) break;
    heapSwap(data, i, least);
    i = least;
  }
}

bool heapPush(Data& data, SharedMemoryCache::Entry* e) {
  if (data.heapCount == data.heapCapacity) {
    size_t capacity = data.heapCapacity == 0 ? 64 : data.heapCapacity * 2;
    auto** grown = poolAllocate<SharedMemoryCache::Entry*>(data, capacity * sizeof(void*));
    if (grown == nullptr) return false;
    if (data.heap != nullptr) {
      memcpy(grown, data.heap, data.heapCount * sizeof(void*));
      data.pool.deallocate(data.heap, data.heapCapacity * sizeof(void*));
    }
    data.heap = grown;
    data.heapCapacity = capacity;
  }
  e->heapIndex = data.heapCount;
  data.heap[data.heapCount++] = e;
  heapSiftUp(data, e->heapIndex);
  return true;
}

void heapRemove(Data& data, SharedMemoryCache::Entry* e) {
  size_t i = e->heapIndex;
  if (i == SharedMemoryCache::Entry::NO_HEAP) return;
  size_t last = --data.heapCount;
  if (i != last) {
    data.heap[i] = data.heap[last];
    data.heap[i]->heapIndex = i;
    // One of the two moves it, or neither does; the other then finds nothing to do.
    heapSiftUp(data, i);
    heapSiftDown(data, i);
  }
  data.heap[last] = nullptr;
  e->heapIndex = SharedMemoryCache::Entry::NO_HEAP;
}

// ---- the entry table ----

SharedMemoryCache::Entry* findEntry(Data& data, kj::ArrayPtr<const char> key, uint64_t hash) {
  if (data.bucketCount == 0) return nullptr;
  for (auto* e = data.buckets[hash & (data.bucketCount - 1)]; e != nullptr; e = e->hashNext) {
    if (e->hash == hash && e->keyPtr() == key) return e;
  }
  return nullptr;
}

bool growBuckets(Data& data) {
  size_t count = data.bucketCount == 0 ? 64 : data.bucketCount * 2;
  auto** grown = poolAllocate<SharedMemoryCache::Entry*>(data, count * sizeof(void*));
  if (grown == nullptr) return false;
  for (size_t i = 0; i < data.bucketCount; i++) {
    for (auto* e = data.buckets[i]; e != nullptr;) {
      auto* next = e->hashNext;
      auto& slot = grown[e->hash & (count - 1)];
      e->hashNext = slot;
      slot = e;
      e = next;
    }
  }
  if (data.buckets != nullptr) data.pool.deallocate(data.buckets, data.bucketCount * sizeof(void*));
  data.buckets = grown;
  data.bucketCount = count;
  return true;
}

// Takes the entry out of every structure and frees it, with its reference to the value.
void eraseEntry(Data& data, SharedMemoryCache::Entry* e) {
  auto& slot = data.buckets[e->hash & (data.bucketCount - 1)];
  for (auto** p = &slot; *p != nullptr; p = &(*p)->hashNext) {
    if (*p == e) {
      *p = e->hashNext;
      break;
    }
  }
  lruUnlink(data, e);
  heapRemove(data, e);
  data.totalValueSize -= e->value->size;
  data.entryCount--;
  releaseValue(data, e->value);
  data.pool.deallocate(e, SharedMemoryCache::Entry::blockSize(e->keyLen));
}

// An inserted entry owns `value`'s reference. Null when the pool is spent, with nothing changed.
SharedMemoryCache::Entry* insertEntry(Data& data,
    kj::ArrayPtr<const char> key,
    uint64_t hash,
    SharedMemoryCache::Value* value,
    kj::Maybe<double> expiration) {
  if (data.entryCount + 1 > data.bucketCount && !growBuckets(data)) return nullptr;
  auto* e =
      poolAllocate<SharedMemoryCache::Entry>(data, SharedMemoryCache::Entry::blockSize(key.size()));
  if (e == nullptr) return nullptr;
  e->hash = hash;
  e->keyLen = key.size();
  memcpy(e->key(), key.begin(), key.size());
  e->value = value;
  e->heapIndex = SharedMemoryCache::Entry::NO_HEAP;
  KJ_IF_SOME(x, expiration) {
    e->hasExpiration = true;
    e->expiration = x;
    if (!heapPush(data, e)) {
      data.pool.deallocate(e, SharedMemoryCache::Entry::blockSize(key.size()));
      return nullptr;
    }
  } else {
    e->hasExpiration = false;
    e->expiration = 0;
  }
  auto& slot = data.buckets[hash & (data.bucketCount - 1)];
  e->hashNext = slot;
  slot = e;
  lruAppend(data, e);
  data.entryCount++;
  data.totalValueSize += value->size;
  return e;
}

// Evictions a write made, packed into the result as far as it holds them (the count in the
// result is the full one).
struct EvictionLog {
  kj::byte* buf;
  size_t capacity;
  size_t used = 0;
  uint64_t count = 0;
  uint64_t packed = 0;

  void record(Data& data, EvictionReason reason, SharedMemoryCache::Entry* e) {
    count++;
    size_t need = sizeof(PackedEviction) + e->keyLen;
    if (buf == nullptr || need > capacity - used) return;
    PackedEviction packedEviction = {
      static_cast<uint64_t>(reason),
      e->value->size,
      data.totalValueSize,
      data.entryCount,
      e->keyLen,
    };
    memcpy(buf + used, &packedEviction, sizeof(packedEviction));
    memcpy(buf + used + sizeof(packedEviction), e->key(), e->keyLen);
    used += need;
    packed++;
  }
};

// Evicts one entry: the earliest-expiring one if it has expired, else the least recently used.
// The cache must not be empty.
void evictNext(Data& data, double now, EvictionLog& log) {
  KJ_REQUIRE(data.entryCount > 0);
  if (data.heapCount > 0 && data.heap[0]->expiration < now) {
    auto* e = data.heap[0];
    log.record(data, EvictionReason::EXPIRATION, e);
    eraseEntry(data, e);
    return;
  }
  auto* e = data.lruHead;
  log.record(data, EvictionReason::LRU, e);
  eraseEntry(data, e);
}

// A hit refreshes the entry's recency; an expired entry is dropped and is a miss.
SharedMemoryCache::Entry* lookup(
    Data& data, kj::ArrayPtr<const char> key, uint64_t hash, double now) {
  auto* e = findEntry(data, key, hash);
  if (e == nullptr) return nullptr;
  if (e->hasExpiration && e->expiration < now) {
    eraseEntry(data, e);
    return nullptr;
  }
  lruUnlink(data, e);
  lruAppend(data, e);
  return e;
}

void removeIfExists(Data& data, kj::ArrayPtr<const char> key, uint64_t hash) {
  if (auto* e = findEntry(data, key, hash); e != nullptr) eraseEntry(data, e);
}

// Stores `value` under `key`, taking the value's reference whatever the outcome, and fills in
// `out` for the write span. `declaredSize` is the size the leader published; it is the value's
// size unless the value was never copied because the declared size exceeded the limit.
void store(Data& data,
    kj::ArrayPtr<const char> key,
    uint64_t hash,
    SharedMemoryCache::Value* value,
    size_t declaredSize,
    kj::Maybe<double> expiration,
    double now,
    WriteOut& out,
    EvictionLog& log) {
  size_t valueSize = declaredSize;
  out.valueSize = valueSize;
  out.maxValueSize = data.effectiveLimits.maxValueSize;
  KJ_DEFER({
    out.evictions = log.count;
    out.packedEvictions = log.packed;
    out.totalAfter = data.totalValueSize;
    out.entriesAfter = data.entryCount;
  });

  if (valueSize > data.effectiveLimits.maxValueSize || value->size != declaredSize) {
    // Silently drop the value. For consistency, also drop the previous value, if one exists,
    // such that a subsequent read() will not return an outdated value. A value that was never
    // copied because its declared size exceeded the limit at the time is dropped the same way,
    // whatever the limit is now.
    out.outcome = static_cast<uint64_t>(WriteOutcome::VALUE_TOO_LARGE);
    removeIfExists(data, key, hash);
    releaseValue(data, value);
    return;
  }
  KJ_IF_SOME(x, expiration) {
    if (x < now) {
      out.outcome = static_cast<uint64_t>(WriteOutcome::ALREADY_EXPIRED);
      removeIfExists(data, key, hash);
      releaseValue(data, value);
      return;
    }
  }

  auto* existing = findEntry(data, key, hash);
  out.isUpdate = existing != nullptr;
  if (existing != nullptr) {
    // Released first, so that the evictions below cannot pick it.
    eraseEntry(data, existing);
  } else if (data.entryCount >= data.effectiveLimits.maxKeys) {
    evictNext(data, now, log);
  }
  while (data.totalValueSize + valueSize > data.effectiveLimits.maxTotalValueSize) {
    evictNext(data, now, log);
  }
  if (insertEntry(data, key, hash, value, expiration) == nullptr) {
    out.outcome = static_cast<uint64_t>(WriteOutcome::NO_MEMORY);
    releaseValue(data, value);
    return;
  }
  out.outcome = static_cast<uint64_t>(WriteOutcome::SUCCESS);
}

// ---- the in-flight table ----

SharedMemoryCache::InFlight* findInFlight(Data& data, kj::ArrayPtr<const char> key, uint64_t hash) {
  if (data.inFlightBucketCount == 0) return nullptr;
  for (auto* f = data.inFlightBuckets[hash & (data.inFlightBucketCount - 1)]; f != nullptr;
       f = f->hashNext) {
    if (f->hash == hash && f->keyPtr() == key) return f;
  }
  return nullptr;
}

bool growInFlightBuckets(Data& data) {
  size_t count = data.inFlightBucketCount == 0 ? 16 : data.inFlightBucketCount * 2;
  auto** grown = poolAllocate<SharedMemoryCache::InFlight*>(data, count * sizeof(void*));
  if (grown == nullptr) return false;
  for (size_t i = 0; i < data.inFlightBucketCount; i++) {
    for (auto* f = data.inFlightBuckets[i]; f != nullptr;) {
      auto* next = f->hashNext;
      auto& slot = grown[f->hash & (count - 1)];
      f->hashNext = slot;
      slot = f;
      f = next;
    }
  }
  if (data.inFlightBuckets != nullptr) {
    data.pool.deallocate(data.inFlightBuckets, data.inFlightBucketCount * sizeof(void*));
  }
  data.inFlightBuckets = grown;
  data.inFlightBucketCount = count;
  return true;
}

SharedMemoryCache::InFlight* insertInFlight(
    Data& data, kj::ArrayPtr<const char> key, uint64_t hash, const void* leader) {
  if (data.inFlightCount + 1 > data.inFlightBucketCount && !growInFlightBuckets(data)) {
    return nullptr;
  }
  auto* f = poolAllocate<SharedMemoryCache::InFlight>(
      data, SharedMemoryCache::InFlight::blockSize(key.size()));
  if (f == nullptr) return nullptr;
  f->hash = hash;
  f->keyLen = key.size();
  memcpy(f->key(), key.begin(), key.size());
  f->ticket = data.nextTicket++;
  f->leader = leader;
  f->head = f->tail = nullptr;
  f->waiters = 0;
  auto& slot = data.inFlightBuckets[hash & (data.inFlightBucketCount - 1)];
  f->hashNext = slot;
  slot = f;
  data.inFlightCount++;
  return f;
}

// The entry's waiters must have been set aside or destroyed.
void eraseInFlight(Data& data, SharedMemoryCache::InFlight* f) {
  auto& slot = data.inFlightBuckets[f->hash & (data.inFlightBucketCount - 1)];
  for (auto** p = &slot; *p != nullptr; p = &(*p)->hashNext) {
    if (*p == f) {
      *p = f->hashNext;
      break;
    }
  }
  data.inFlightCount--;
  data.pool.deallocate(f, SharedMemoryCache::InFlight::blockSize(f->keyLen));
}

SharedMemoryCache::Waiter* newWaiter(Data& data) {
  auto* w = poolAllocate<SharedMemoryCache::Waiter>(data, sizeof(SharedMemoryCache::Waiter));
  if (w == nullptr) return nullptr;
  return new (w) SharedMemoryCache::Waiter();
}

void destroyWaiter(Data& data, SharedMemoryCache::Waiter* w) {
  w->~Waiter();
  data.pool.deallocate(w, sizeof(SharedMemoryCache::Waiter));
}

void appendWaiter(SharedMemoryCache::InFlight& f, SharedMemoryCache::Waiter* w) {
  w->next = nullptr;
  if (f.tail != nullptr) {
    f.tail->next = w;
  } else {
    f.head = w;
  }
  f.tail = w;
  f.waiters++;
}

// ---- the instances with waiters to wake ----

struct WD_MEMORY_CLASS_HOST WakeQueue {
  WakeQueue(): head(nullptr) {}
  kj::MutexGuarded<SharedMemoryCache*> head;
  std::atomic<bool> pending{false};
};

WakeQueue& wakeQueue() {
  // Made once, host-side, and never freed: host code reaches it after every turn.
  static WakeQueue* queue =
      new (hostMemory::allocate(sizeof(WakeQueue), alignof(WakeQueue))) WakeQueue();
  return *queue;
}

// Test-only: takes the guest fault a request for /__gk_fault_cache_fallback armed (see
// maybeForceGuestFaultForTest in global-scope.c++), in the frame that runs a fallback.
void maybeForceGuestFaultInFallbackForTest(kj::StringPtr key) {
  if (takeForcedGuestFaultInMemoryCacheFallbackForTest()) {
    KJ_LOG(WARNING, "WORKERD_GK_TEST_FAULT: forcing a guest fault inside a memory cache fallback",
        key);
    *static_cast<volatile int*>(nullptr) = 0;
  }
}

}  // namespace

// =======================================================================================
// The instance: host-side construction and teardown, limits, and the handler.

void* SharedMemoryCache::operator new(size_t size) {
  return hostMemory::allocate(size, alignof(SharedMemoryCache));
}

void SharedMemoryCache::operator delete(void* ptr, size_t size) {
  hostMemory::deallocate(ptr, size, alignof(SharedMemoryCache));
}

SharedMemoryCache::SharedMemoryCache(kj::Maybe<const MemoryCacheProvider&> provider,
    kj::StringPtr id,
    kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler,
    const kj::MonotonicClock& timer)
    : data(hostOwned<kj::MutexGuarded<Data>>()),
      provider(provider),
      id(static_cast<char*>(hostMemory::allocate(id.size() + 1, 1))),
      idLength(id.size()),
      additionalResizeMemoryLimitHandler(additionalResizeMemoryLimitHandler),
      timer(timer) {
  memcpy(this->id, id.begin(), id.size());
}

SharedMemoryCache::~SharedMemoryCache() noexcept(false) {
  KJ_IF_SOME(p, provider) {
    p.removeInstance(*this);
  }
  {
    // Nothing a drain would do for this instance's waiters can reach anyone: the last binding
    // is gone, and with it every context that could have waited.
    auto& queue = wakeQueue();
    auto head = queue.head.lockExclusive();
    for (auto** p = &*head; *p != nullptr; p = &(*p)->wakeNext) {
      if (*p == this) {
        *p = wakeNext;
        break;
      }
    }
  }
  auto lock = data->lockExclusive();
  auto destroyWaiters = [&](Waiter* w) {
    while (w != nullptr) {
      auto* next = w->next;
      destroyWaiter(*lock, w);
      w = next;
    }
  };
  for (size_t i = 0; i < lock->inFlightBucketCount; i++) {
    for (auto* f = lock->inFlightBuckets[i]; f != nullptr; f = f->hashNext) {
      destroyWaiters(f->head);
    }
  }
  for (auto* wake = lock->wakeHead; wake != nullptr; wake = wake->next) {
    destroyWaiters(wake->head);
  }
  // The entries, keys, values and tables go with the pool's regions.
  hostMemory::deallocate(id, idLength + 1, 1);
}

kj::Own<const SharedMemoryCache> SharedMemoryCache::create(
    kj::Maybe<const MemoryCacheProvider&> provider,
    kj::StringPtr id,
    kj::Maybe<AdditionalResizeMemoryLimitHandler&> handler,
    const kj::MonotonicClock& timer) {
  return kj::atomicRefcounted<const SharedMemoryCache>(provider, id, handler, timer);
}

void SharedMemoryCache::suggest(const Limits& limits) const {
  auto lock = data->lockExclusive();
  for (auto* s = lock->suggested; s != nullptr; s = s->next) {
    if (s->limits == limits) {
      s->count++;
      return;
    }
  }
  auto* s = poolAllocate<SuggestedLimit>(*lock, sizeof(SuggestedLimit));
  if (s == nullptr) {
    // The pool has no region yet: the first suggestion reserves it, below.
    Limits normalized = limits.normalize();
    size_t need = 2 * normalized.maxTotalValueSize + size_t{normalized.maxKeys} * 512;
    if (need < hostMemory::HostPool::MIN_REGION) need = hostMemory::HostPool::MIN_REGION;
    KJ_REQUIRE(lock->pool.ensure(need), "memory cache: could not reserve the instance's pool");
    s = poolAllocate<SuggestedLimit>(*lock, sizeof(SuggestedLimit));
    KJ_REQUIRE(s != nullptr);
  }
  s->limits = limits;
  s->count = 1;
  s->next = lock->suggested;
  lock->suggested = s;
  resize(*lock);
}

void SharedMemoryCache::unsuggest(const Binding& binding) const {
  auto lock = data->lockExclusive();
  // Every key this binding leads is abandoned: its waiters get the key, as they would had the
  // leader's fallback failed.
  for (size_t i = 0; i < lock->inFlightBucketCount; i++) {
    for (auto* f = lock->inFlightBuckets[i]; f != nullptr;) {
      auto* next = f->hashNext;
      if (f->leader == &binding) {
        setAsideWaiters(*lock, *f);
        eraseInFlight(*lock, f);
      }
      f = next;
    }
  }
  for (auto** p = &lock->suggested; *p != nullptr; p = &(*p)->next) {
    auto* s = *p;
    if (s->limits != binding.limits) continue;
    if (--s->count == 0) {
      *p = s->next;
      lock->pool.deallocate(s, sizeof(SuggestedLimit));
    }
    break;
  }
  resize(*lock);
}

void SharedMemoryCache::resize(Data& data) const {
  data.effectiveLimits = Limits::min();
  for (auto* s = data.suggested; s != nullptr; s = s->next) {
    data.effectiveLimits = Limits::max(data.effectiveLimits, s->limits.normalize());
  }

  KJ_IF_SOME(handler, additionalResizeMemoryLimitHandler) {
    // Allow the embedder to adjust the effective limits.
    handler(data);
  }

  // The reservation follows the limits: twice the values, for the allocator's rounding, plus
  // room per key for its entry.
  size_t need = 2 * data.effectiveLimits.maxTotalValueSize +
      size_t{data.effectiveLimits.maxKeys} * 512 + hostMemory::HostPool::MIN_REGION;
  if (data.pool.reservation() < need && !data.pool.ensure(need)) {
    KJ_LOG(WARNING, "memory cache: could not grow the instance's pool; it serves what it has",
        getId(), need, data.pool.reservation());
  }

  // Fast path for clearing the cache.
  if (data.effectiveLimits.maxKeys == 0) {
    while (data.lruHead != nullptr) eraseEntry(data, data.lruHead);
    return;
  }

  // First, remove any values that might be too large.
  for (auto* e = data.lruHead; e != nullptr;) {
    auto* next = e->lruNext;
    if (e->value->size > data.effectiveLimits.maxValueSize) eraseEntry(data, e);
    e = next;
  }

  // Now just keep evicting until we are within limits. Outside any request, expiry is judged
  // by the system clock.
  double now = cacheNow();
  EvictionLog log = {nullptr, 0};
  while (data.totalValueSize > data.effectiveLimits.maxTotalValueSize ||
      data.entryCount > data.effectiveLimits.maxKeys) {
    evictNext(data, now, log);
  }
}

void SharedMemoryCache::setAsideWaiters(Data& data, InFlight& inFlight) const {
  if (inFlight.head == nullptr) return;
  auto* wake = poolAllocate<Wake>(data, sizeof(Wake));
  if (wake == nullptr) {
    // The pool is spent; the waiters cannot be kept for a wake. Dropped, as a context that
    // ends drops its waiter: each waiter's own request ends with it.
    auto* w = inFlight.head;
    while (w != nullptr) {
      auto* next = w->next;
      destroyWaiter(data, w);
      w = next;
    }
  } else {
    wake->head = inFlight.head;
    wake->next = nullptr;
    if (data.wakeTail != nullptr) {
      data.wakeTail->next = wake;
    } else {
      data.wakeHead = wake;
    }
    data.wakeTail = wake;
    if (!data.queuedForWake) {
      data.queuedForWake = true;
      auto& queue = wakeQueue();
      auto head = queue.head.lockExclusive();
      // The queue holds a pointer to a const instance only to wake it; the wake mutates the
      // instance's data under its own lock, which every method here does through `data`.
      auto* self = const_cast<SharedMemoryCache*>(this);
      self->wakeNext = *head;
      *head = self;
      queue.pending.store(true, std::memory_order_release);
    }
  }
  inFlight.head = inFlight.tail = nullptr;
  inFlight.waiters = 0;
}

void SharedMemoryCache::drainWakes() {
  auto& queue = wakeQueue();
  if (!queue.pending.load(std::memory_order_acquire)) return;
  SharedMemoryCache* list;
  {
    auto head = queue.head.lockExclusive();
    list = *head;
    *head = nullptr;
    queue.pending.store(false, std::memory_order_release);
  }
  for (auto* cache = list; cache != nullptr;) {
    SharedMemoryCache* next;
    Wake* wakes;
    {
      // The link is read and cleared under the instance's lock, which is what a publish holds
      // when it queues the instance again.
      auto lock = cache->data->lockExclusive();
      next = cache->wakeNext;
      cache->wakeNext = nullptr;
      wakes = lock->wakeHead;
      lock->wakeHead = lock->wakeTail = nullptr;
      lock->queuedForWake = false;
    }
    // Woken outside the instance's lock: a fulfiller's fulfill() may destroy a promise whose
    // teardown comes back to the cache, and an action's box is allocated.
    for (auto* wake = wakes; wake != nullptr; wake = wake->next) {
      for (auto* w = wake->head; w != nullptr; w = w->next) {
        if (w->kind == Waiter::ACTION) {
          // The box the waiter's context will run is made in the shared heap: this runs under
          // the lock of whichever isolate published, and the box is another isolate's to run.
          jsg::SharedHeapScope sharedHeap;
          uint64_t cookie = w->cookie;
          DeleteQueue(w->context).tryScheduleAction([cookie](jsg::Lock& js) {
            wakeHolder(cookie);
          });
        } else {
          KJ_IF_SOME(fulfiller, w->fulfiller) {
            fulfiller->fulfill();
          }
        }
      }
    }
    {
      auto lock = cache->data->lockExclusive();
      for (auto* wake = wakes; wake != nullptr;) {
        auto* nextWake = wake->next;
        for (auto* w = wake->head; w != nullptr;) {
          auto* nextWaiter = w->next;
          destroyWaiter(*lock, w);
          w = nextWaiter;
        }
        lock->pool.deallocate(wake, sizeof(Wake));
        wake = nextWake;
      }
    }
    cache = next;
  }
}

// ---- the handler ----

long SharedMemoryCache::hostcallHandler(hostcall::Frame& frame) {
  // Nothing may unwind out of a handler into the guest kernel's frames; anything thrown by what
  // the handler calls is reported and becomes a failed call.
  long result = -EIO;
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { result = handleCall(frame); })) {
    KJ_LOG(ERROR, "memory cache hostcall handler threw", exception);
    result = -EIO;
  }
  return result;
}

long SharedMemoryCache::handleCall(hostcall::Frame& frame) {
  auto op = static_cast<Op>(frame.args[0]);
  uint64_t handle = frame.args[1];
  if (frame.inLen > hostcall::BYTES) return -E2BIG;
  frame.outLen = 0;

  // The binding, through the table; a handle naming anything else is refused here. Then the
  // sharing contract: the record was made under the arena of the isolate the binding belongs
  // to, and the call must come from a thread under that arena by gk's record of it. A thread
  // under no arena is host code -- a binding's teardown, a context's -- and is its own
  // authority.
  auto* binding = static_cast<const Binding*>(hostMemory::resolveSlot(handle));
  if (binding == nullptr || binding->tag != BINDING_TAG) return -EACCES;
  hostMemory::IsolateArena arena = hostMemory::currentIsolateArena();
  if (arena != nullptr && arena != binding->arena) return -EACCES;
  const SharedMemoryCache& cache = *binding->cache;
  auto& pin = *reinterpret_cast<PinState*>(frame.state);

  switch (op) {
    case Op::READ:
      return cache.handleRead(frame, *binding, pin);
    case Op::READ_RANGE:
      return cache.handleReadRange(frame, pin);
    case Op::READ_CHUNK:
      return cache.handleReadChunk(frame, pin);
    case Op::PUBLISH:
    case Op::PUBLISH_BEGIN:
    case Op::PUBLISH_CHUNK:
    case Op::PUBLISH_END:
      return cache.handlePublish(frame, *binding, pin, op);
    case Op::ABANDON:
      return cache.handleAbandon(frame);
    case Op::DELETE:
      return cache.handleDelete(frame);
  }
  return -EINVAL;
}

void SharedMemoryCache::unpin(PinState& pin) const {
  if (pin.tag != PIN_TAG || pin.instance != this) return;
  auto lock = data->lockExclusive();
  releaseValue(*lock, pin.value);
  pin = PinState{};
}

namespace {

// A header and the key that follows it, copied out of the frame: the frame's buffer is where
// the result goes, over the arguments.
struct KeyCopy {
  char bytes[MAX_KEY_SIZE];
};
template <typename In>
bool takeKey(
    hostcall::Frame& frame, In& in, KeyCopy& copy, kj::ArrayPtr<const char>& key, size_t& used) {
  if (frame.inLen < sizeof(In)) return false;
  memcpy(&in, frame.buf, sizeof(In));
  if (in.keyLen > MAX_KEY_SIZE || in.keyLen > frame.inLen - sizeof(In)) return false;
  memcpy(copy.bytes, frame.buf + sizeof(In), in.keyLen);
  key = kj::arrayPtr(copy.bytes, in.keyLen);
  used = sizeof(In) + in.keyLen;
  return true;
}

}  // namespace

long SharedMemoryCache::handleRead(
    hostcall::Frame& frame, const Binding& binding, PinState& pin) const {
  // A value a previous read on this thread pinned and never fetched is released first.
  unpin(pin);
  ReadIn in;
  KeyCopy keyCopy;
  kj::ArrayPtr<const char> key;
  size_t used;
  if (!takeKey(frame, in, keyCopy, key, used)) return -EINVAL;
  bool withFallback = (frame.args[2] & READ_WITH_FALLBACK) != 0;
  double now = fromBits(in.nowBits);
  uint64_t hash = hashKey(key);

  uint64_t t0 = monotonicNs();
  auto lock = data->lockExclusive();
  ReadOut out = {};
  out.lockWaitNs = monotonicNs() - t0;
  KJ_DEFER({
    out.totalValueSize = lock->totalValueSize;
    out.entryCount = lock->entryCount;
    memcpy(frame.buf, &out, sizeof(out));
    if (frame.outLen < sizeof(out)) frame.outLen = sizeof(out);
  });

  if (auto* e = lookup(*lock, key, hash, now); e != nullptr) {
    out.valueSize = e->value->size;
    if (e->value->size <= INLINE_VALUE_MAX) {
      out.kind = static_cast<uint64_t>(ReadKind::HIT);
      out.inlineLen = e->value->size;
      memcpy(frame.buf + sizeof(out), e->value->bytes(), e->value->size);
      frame.outLen = sizeof(out) + e->value->size;
    } else {
      // Pinned for the fetch that follows: the entry may be evicted meanwhile, and the value
      // then lives on until the fetch releases it.
      out.kind = static_cast<uint64_t>(ReadKind::HIT_LARGE);
      e->value->refs++;
      pin = PinState{PIN_TAG, this, e->value, 0, 0};
    }
    return 0;
  }
  if (!withFallback) {
    out.kind = static_cast<uint64_t>(ReadKind::MISS);
    return 0;
  }
  if (auto* f = findInFlight(*lock, key, hash); f != nullptr) {
    out.kind = static_cast<uint64_t>(ReadKind::WAIT);
    out.ticket = f->ticket;
    out.waitersAhead = f->waiters;
    if (in.cookie != 0) {
      // The waiter's context and record are handles the turn chose; the host queues an action
      // through the first and the waiter's own turn resolves the second, and both must be the
      // caller's own, which is what the table says of them here.
      if (!hostMemory::ownedByCurrentArena(in.context)) return -EACCES;
      auto* w = newWaiter(*lock);
      if (w == nullptr) return -ENOMEM;
      w->kind = Waiter::ACTION;
      w->context = in.context;
      w->cookie = in.cookie;
      appendWaiter(*f, w);
    }
    return 0;
  }
  auto* f = insertInFlight(*lock, key, hash, &binding);
  if (f == nullptr) return -ENOMEM;
  out.kind = static_cast<uint64_t>(ReadKind::LEAD);
  out.ticket = f->ticket;
  return 0;
}

long SharedMemoryCache::handleReadRange(hostcall::Frame& frame, PinState& pin) const {
  if (pin.tag != PIN_TAG || pin.instance != this || pin.staging != 0) return -EINVAL;
  auto* dest = reinterpret_cast<kj::byte*>(static_cast<uintptr_t>(frame.args[2]));
  uint64_t len = frame.args[3];
  if (len != pin.value->size) return -EINVAL;
  // The range is the caller's to name and the host's to check: only the caller's own arena.
  if (!hostMemory::arenaRangeOk(dest, len)) return -EFAULT;
  memcpy(dest, pin.value->bytes(), len);
  unpin(pin);
  return 0;
}

long SharedMemoryCache::handleReadChunk(hostcall::Frame& frame, PinState& pin) const {
  if (pin.tag != PIN_TAG || pin.instance != this || pin.staging != 0) return -EINVAL;
  uint64_t offset = frame.args[2];
  uint64_t size = pin.value->size;
  if (offset >= size) return -EINVAL;
  size_t n = size - offset;
  if (n > hostcall::BYTES) n = hostcall::BYTES;
  memcpy(frame.buf, pin.value->bytes() + offset, n);
  frame.outLen = n;
  if (offset + n == size) unpin(pin);
  return 0;
}

long SharedMemoryCache::handlePublish(
    hostcall::Frame& frame, const Binding& binding, PinState& pin, Op op) const {
  if (op == Op::PUBLISH_CHUNK) {
    if (pin.tag != PIN_TAG || pin.instance != this || pin.staging == 0) return -EINVAL;
    size_t n = frame.inLen;
    // A staged value the limits will not take is empty, and its chunks are dropped.
    if (pin.value->size == 0) return 0;
    if (n > pin.value->size - pin.offset) return -EINVAL;
    memcpy(pin.value->bytes() + pin.offset, frame.buf, n);
    pin.offset += n;
    return 0;
  }

  PublishIn in;
  KeyCopy keyCopy;
  kj::ArrayPtr<const char> key;
  size_t used;
  if (!takeKey(frame, in, keyCopy, key, used)) return -EINVAL;
  if (in.valueSize > ~uint32_t(0)) return -EINVAL;
  Value* value = nullptr;

  if (op == Op::PUBLISH_BEGIN) {
    // A value larger than the frame is staged in the pool and filled by the chunks that follow;
    // whatever this thread was staging before is given up.
    unpin(pin);
    auto lock = data->lockExclusive();
    value = poolAllocate<Value>(*lock, Value::blockSize(in.valueSize));
    if (value == nullptr) return -ENOMEM;
    value->refs = 1;
    value->size = in.valueSize;
    pin = PinState{PIN_TAG, this, value, 0, 1};
    return 0;
  }
  // A value the limits will not take is not copied: the leader's declared size decides here,
  // and the store below judges the same size, so what it does with the key is the same.
  bool oversized = in.valueSize > data->lockExclusive()->effectiveLimits.maxValueSize;
  if (op == Op::PUBLISH_BEGIN && oversized) {
    unpin(pin);
    auto lock = data->lockExclusive();
    value = poolAllocate<Value>(*lock, Value::blockSize(0));
    if (value == nullptr) return -ENOMEM;
    value->refs = 1;
    value->size = 0;
    pin = PinState{PIN_TAG, this, value, 0, 1};
    return 0;
  }
  if (op == Op::PUBLISH_END) {
    if (pin.tag != PIN_TAG || pin.instance != this || pin.staging == 0 ||
        pin.offset != pin.value->size ||
        (pin.value->size != in.valueSize && pin.value->size != 0)) {
      return -EINVAL;
    }
    value = pin.value;
    pin = PinState{};
  } else {
    unpin(pin);
    auto mode = static_cast<PublishMode>(frame.args[2]);
    const kj::byte* source;
    if (mode == PublishMode::INLINE) {
      if (in.valueSize > frame.inLen - used) return -EINVAL;
      source = frame.buf + used;
    } else if (mode == PublishMode::RANGE) {
      source = reinterpret_cast<const kj::byte*>(static_cast<uintptr_t>(frame.args[3]));
      // The leader's own bytes, in its own arena, read once.
      if (!oversized && !hostMemory::arenaRangeOk(source, in.valueSize)) return -EFAULT;
    } else {
      return -EINVAL;
    }
    size_t copied = oversized ? 0 : in.valueSize;
    auto lock = data->lockExclusive();
    value = poolAllocate<Value>(*lock, Value::blockSize(copied));
    if (value == nullptr) return -ENOMEM;
    value->refs = 1;
    value->size = copied;
    memcpy(value->bytes(), source, copied);
  }

  double now = fromBits(in.nowBits);
  kj::Maybe<double> expiration;
  if (in.hasExpiration != 0) expiration = fromBits(in.expirationBits);
  uint64_t hash = hashKey(key);

  uint64_t t0 = monotonicNs();
  auto lock = data->lockExclusive();
  WriteOut out = {};
  out.lockWaitNs = monotonicNs() - t0;
  auto* f = findInFlight(*lock, key, hash);
  if (f == nullptr || f->ticket != in.ticket) {
    // Not the leader of this key any more: the binding was torn down and the key abandoned, or
    // the ticket is not one the host issued to this caller. Nothing is stored.
    releaseValue(*lock, value);
    out.outcome = static_cast<uint64_t>(WriteOutcome::STALE);
    memcpy(frame.buf, &out, sizeof(out));
    frame.outLen = sizeof(out);
    return 0;
  }
  EvictionLog log = {frame.buf + sizeof(out), hostcall::BYTES - sizeof(out)};
  store(*lock, key, hash, value, in.valueSize, expiration, now, out, log);
  out.waitersNotified = f->waiters;
  setAsideWaiters(*lock, *f);
  eraseInFlight(*lock, f);
  memcpy(frame.buf, &out, sizeof(out));
  frame.outLen = sizeof(out) + log.used;
  return 0;
}

long SharedMemoryCache::handleAbandon(hostcall::Frame& frame) const {
  AbandonIn in;
  KeyCopy keyCopy;
  kj::ArrayPtr<const char> key;
  size_t used;
  if (!takeKey(frame, in, keyCopy, key, used)) return -EINVAL;
  uint64_t hash = hashKey(key);
  auto lock = data->lockExclusive();
  WriteOut out = {};
  auto* f = findInFlight(*lock, key, hash);
  if (f != nullptr && f->ticket == in.ticket) {
    out.waitersNotified = f->waiters;
    setAsideWaiters(*lock, *f);
    eraseInFlight(*lock, f);
  } else {
    out.outcome = static_cast<uint64_t>(WriteOutcome::STALE);
  }
  memcpy(frame.buf, &out, sizeof(out));
  frame.outLen = sizeof(out);
  return 0;
}

long SharedMemoryCache::handleDelete(hostcall::Frame& frame) const {
  DeleteIn in;
  KeyCopy keyCopy;
  kj::ArrayPtr<const char> key;
  size_t used;
  if (!takeKey(frame, in, keyCopy, key, used)) return -EINVAL;
  auto lock = data->lockExclusive();
  removeIfExists(*lock, key, hashKey(key));
  return 0;
}

const hostcall::Entry SharedMemoryCache::HOSTCALL = {
  hostcall::MEMORY_CACHE, &SharedMemoryCache::hostcallHandler};

namespace {
std::atomic<bool> wakeHookInstalled{false};

void installWakeHook() {
  if (wakeHookInstalled.exchange(true)) return;
  wakeQueue();
  hostMemory::setTurnEndHook(&SharedMemoryCache::drainWakes);
}
}  // namespace

void SharedMemoryCache::installHostcall() {
  hostcall::install(HOSTCALL);
  installWakeHook();
}

// =======================================================================================
// The turn's side: a Use, its reads and the leader's callback.

namespace {

using Use = SharedMemoryCache::Use;
using Outcome = Use::GetWithFallbackOutcome;
using Op = SharedMemoryCache::Op;

long callCache(hostcall::Frame& frame, Op op, uint64_t binding, uint64_t a2 = 0, uint64_t a3 = 0) {
  frame.args[0] = static_cast<uint64_t>(op);
  frame.args[1] = binding;
  frame.args[2] = a2;
  frame.args[3] = a3;
  return hostcall::call(SharedMemoryCache::HOSTCALL, frame);
}

template <typename In>
void putKey(hostcall::Frame& frame, const In& in, kj::ArrayPtr<const char> key) {
  memcpy(frame.buf, &in, sizeof(in));
  memcpy(frame.buf + sizeof(in), key.begin(), key.size());
  frame.inLen = sizeof(in) + key.size();
}

ReadOut doRead(hostcall::Frame& frame,
    uint64_t binding,
    const kj::String& key,
    bool withFallback,
    uint64_t context,
    uint64_t cookie) {
  KJ_REQUIRE(key.size() <= MAX_KEY_SIZE);
  ReadIn in = {toBits(cacheNow()), context, cookie, key.size()};
  putKey(frame, in, key.asArray());
  long result = callCache(frame, Op::READ, binding, withFallback ? READ_WITH_FALLBACK : 0);
  KJ_REQUIRE(result == 0, "memory cache read failed", result);
  KJ_REQUIRE(frame.outLen >= sizeof(ReadOut));
  ReadOut out;
  memcpy(&out, frame.buf, sizeof(out));
  return out;
}

// Brings a hit's bytes into the caller's own memory: from the frame when they came with the
// answer, else fetched from the pinned value, into a buffer named by address when the buffer is
// in the caller's arena and a frame at a time otherwise.
kj::Own<CacheValue> fetchValue(hostcall::Frame& frame, uint64_t binding, const ReadOut& out) {
  if (out.kind == static_cast<uint64_t>(ReadKind::HIT)) {
    KJ_REQUIRE(out.inlineLen == out.valueSize && sizeof(ReadOut) + out.inlineLen <= frame.outLen);
    auto bytes = kj::heapArray<kj::byte>(out.inlineLen);
    memcpy(bytes.begin(), frame.buf + sizeof(ReadOut), out.inlineLen);
    return kj::atomicRefcounted<CacheValue>(kj::mv(bytes));
  }
  KJ_REQUIRE(out.kind == static_cast<uint64_t>(ReadKind::HIT_LARGE));
  auto bytes = kj::heapArray<kj::byte>(out.valueSize);
  if (rangePathUsable()) {
    frame.inLen = 0;
    long result = callCache(
        frame, Op::READ_RANGE, binding, reinterpret_cast<uintptr_t>(bytes.begin()), bytes.size());
    KJ_REQUIRE(result == 0, "memory cache read failed", result);
  } else {
    size_t offset = 0;
    while (offset < bytes.size()) {
      frame.inLen = 0;
      long result = callCache(frame, Op::READ_CHUNK, binding, offset);
      KJ_REQUIRE(result == 0, "memory cache read failed", result);
      KJ_REQUIRE(frame.outLen > 0 && frame.outLen <= bytes.size() - offset);
      memcpy(bytes.begin() + offset, frame.buf, frame.outLen);
      offset += frame.outLen;
    }
  }
  return kj::atomicRefcounted<CacheValue>(kj::mv(bytes));
}

void emitReadTags(SpanBuilder& span, const ReadOut& out) {
  span.setTag("memory_cache_lock_wait_time_ns"_kjc, lockWaitNsForTrace(out.lockWaitNs));
  bool hit = out.kind == static_cast<uint64_t>(ReadKind::HIT) ||
      out.kind == static_cast<uint64_t>(ReadKind::HIT_LARGE);
  span.setTag("cache_hit"_kjc, hit);
  if (hit) span.setTag("entry_size"_kjc, static_cast<double>(out.valueSize));
  span.setTag("cache_total_size"_kjc, static_cast<double>(out.totalValueSize));
  span.setTag("cache_entry_count"_kjc, static_cast<double>(out.entryCount));
}

// The write span and the eviction spans of a publish, from the counters the handler returned.
void emitWriteTrace(kj::StringPtr key,
    kj::Maybe<double> expiration,
    const WriteOut& out,
    kj::ArrayPtr<const kj::byte> packed) {
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    auto writeSpan = context.makeTraceSpan("memory_cache_write"_kjc);
    writeSpan.setTag("key"_kjc, key);
    writeSpan.setTag("value_size"_kjc, static_cast<double>(out.valueSize));
    writeSpan.setTag("has_expiration"_kjc, expiration != kj::none);
    switch (static_cast<WriteOutcome>(out.outcome)) {
      case WriteOutcome::SUCCESS:
        writeSpan.setTag("write_success"_kjc, true);
        writeSpan.setTag("is_update"_kjc, out.isUpdate != 0);
        writeSpan.setTag("evictions_triggered"_kjc, static_cast<double>(out.evictions));
        writeSpan.setTag("cache_total_size_after"_kjc, static_cast<double>(out.totalAfter));
        writeSpan.setTag("cache_entry_count_after"_kjc, static_cast<double>(out.entriesAfter));
        break;
      case WriteOutcome::VALUE_TOO_LARGE:
        writeSpan.setTag("write_rejected"_kjc, true);
        writeSpan.setTag("rejection_reason"_kjc, "value_too_large"_kjc);
        writeSpan.setTag("max_value_size"_kjc, static_cast<double>(out.maxValueSize));
        break;
      case WriteOutcome::ALREADY_EXPIRED:
        writeSpan.setTag("write_rejected"_kjc, true);
        writeSpan.setTag("rejection_reason"_kjc, "already_expired"_kjc);
        break;
      case WriteOutcome::NO_MEMORY:
        writeSpan.setTag("write_rejected"_kjc, true);
        writeSpan.setTag("rejection_reason"_kjc, "host_memory"_kjc);
        break;
      case WriteOutcome::STALE:
        writeSpan.setTag("write_rejected"_kjc, true);
        writeSpan.setTag("rejection_reason"_kjc, "stale_fallback"_kjc);
        break;
    }
    size_t offset = 0;
    for (uint64_t i = 0; i < out.packedEvictions; i++) {
      PackedEviction eviction;
      if (packed.size() - offset < sizeof(eviction)) break;
      memcpy(&eviction, packed.begin() + offset, sizeof(eviction));
      offset += sizeof(eviction);
      if (packed.size() - offset < eviction.keyLen) break;
      auto evictedKey = kj::str(
          kj::arrayPtr(reinterpret_cast<const char*>(packed.begin() + offset), eviction.keyLen));
      offset += eviction.keyLen;
      auto span = context.makeTraceSpan("memory_cache_eviction"_kjc);
      span.setTag("eviction_reason"_kjc,
          static_cast<EvictionReason>(eviction.reason) == EvictionReason::EXPIRATION
              ? "expiration"_kjc
              : "lru"_kjc);
      span.setTag("evicted_key"_kjc, kj::mv(evictedKey));
      span.setTag("evicted_size"_kjc, static_cast<double>(eviction.size));
      span.setTag("cache_size_before"_kjc, static_cast<double>(eviction.totalBefore));
      span.setTag("cache_entries_before"_kjc, static_cast<double>(eviction.entriesBefore));
    }
  }
}

// After a publish or abandon: the waiters it set aside are woken by the host after the turn
// (the turn-end hook), or now, where there is no turn to end.
void wakeAfter() {
  installWakeHook();
#ifdef WORKERD_GK_NEG_CACHE_WAKE_IN_TURN
  // WORKERD_GK_NEG_CACHE_WAKE_IN_TURN is a negative-test build, never shipped: the wake queue is
  // drained from inside the leader's turn rather than by the host after it. The queue, the
  // instances' wake lists and the waiters are host memory, which the turn cannot read, so the
  // leader's turn faults at its first publish; the guest kernel's memory-cache-fault-test.sh
  // must then fail at its first step. (Were the queue readable, the action for a waiter in
  // another isolate would still be refused, as a turn's note naming another isolate's context.)
  SharedMemoryCache::drainWakes();
#else
  if (!hostMemory::inTurn()) SharedMemoryCache::drainWakes();
#endif
}

// What the leader holds while its fallback runs: enough to publish or abandon the key.
struct Lead {
  uint64_t binding;
  kj::String key;
  uint64_t ticket;
  bool settled = false;
};

void abandon(const Lead& lead) {
  hostcall::Frame frame{};
  AbandonIn in = {lead.ticket, lead.key.size()};
  putKey(frame, in, lead.key.asArray());
  long result = callCache(frame, Op::ABANDON, lead.binding);
  if (result != 0) {
    // The binding is gone (its teardown abandoned the key already), or the handle names
    // nothing of ours.
    KJ_LOG(WARNING, "memory cache abandon refused", result);
    return;
  }
  wakeAfter();
}

void publish(const Lead& lead, Use::FallbackResult& result, SpanBuilder& fallbackSpan) {
  auto bytes = result.value->asBytes();
  hostcall::Frame frame{};
  PublishIn in = {
    toBits(cacheNow()),
    toBits(result.expiration.orDefault(0.0)),
    result.expiration != kj::none ? uint64_t(1) : uint64_t(0),
    lead.ticket,
    bytes.size(),
    lead.key.size(),
  };
  long callResult;
  if (bytes.size() <= hostcall::BYTES - sizeof(in) - lead.key.size()) {
    putKey(frame, in, lead.key.asArray());
    memcpy(frame.buf + frame.inLen, bytes.begin(), bytes.size());
    frame.inLen += bytes.size();
    callResult =
        callCache(frame, Op::PUBLISH, lead.binding, static_cast<uint64_t>(PublishMode::INLINE));
  } else if (rangePathUsable()) {
    putKey(frame, in, lead.key.asArray());
    callResult = callCache(frame, Op::PUBLISH, lead.binding,
        static_cast<uint64_t>(PublishMode::RANGE), reinterpret_cast<uintptr_t>(bytes.begin()));
  } else {
    putKey(frame, in, lead.key.asArray());
    callResult = callCache(frame, Op::PUBLISH_BEGIN, lead.binding);
    size_t offset = 0;
    while (callResult == 0 && offset < bytes.size()) {
      size_t n = bytes.size() - offset;
      if (n > hostcall::BYTES) n = hostcall::BYTES;
      memcpy(frame.buf, bytes.begin() + offset, n);
      frame.inLen = n;
      callResult = callCache(frame, Op::PUBLISH_CHUNK, lead.binding);
      offset += n;
    }
    if (callResult == 0) {
      putKey(frame, in, lead.key.asArray());
      callResult = callCache(frame, Op::PUBLISH_END, lead.binding);
    }
  }
  if (callResult != 0) {
    KJ_LOG(WARNING, "memory cache publish refused", callResult);
    return;
  }
  KJ_REQUIRE(frame.outLen >= sizeof(WriteOut));
  WriteOut out;
  memcpy(&out, frame.buf, sizeof(out));
  emitWriteTrace(lead.key, result.expiration, out,
      kj::arrayPtr(frame.buf + sizeof(out), frame.outLen - sizeof(out)));
  fallbackSpan.setTag("waiters_notified"_kjc, static_cast<double>(out.waitersNotified));
  wakeAfter();
}

Use::FallbackDoneCallback makeCallback(uint64_t binding, kj::String key, uint64_t ticket) {
  auto lead = kj::heap<Lead>(Lead{binding, kj::mv(key), ticket});
  auto& leadRef = *lead;
  // Destroyed before `lead` (closure members go in reverse order), so a callback dropped
  // unsettled -- its fallback failed, or its context ended, or its turn faulted -- gives the
  // key up.
  auto deferredAbandon = kj::defer([&lead = leadRef]() {
    if (!lead.settled) abandon(lead);
  });
  return [lead = kj::mv(lead), deferredAbandon = kj::mv(deferredAbandon)](
             kj::Maybe<Use::FallbackResult> maybeResult, SpanBuilder& fallbackSpan) mutable {
    lead->settled = true;
    KJ_IF_SOME(result, maybeResult) {
      publish(*lead, result, fallbackSpan);
    } else {
      abandon(*lead);
    }
  };
}

// A read that found its key in flight, host-side (outside any turn): waits on a fulfiller the
// host keeps with the key, and reads again when woken, as often as it takes.
kj::Promise<Outcome> hostSideWait(uint64_t binding, kj::String key) {
  for (;;) {
    hostcall::Frame frame{};
    ReadOut out = doRead(frame, binding, key, true, 0, 0);
    switch (static_cast<ReadKind>(out.kind)) {
      case ReadKind::HIT:
      case ReadKind::HIT_LARGE:
        co_return Outcome(fetchValue(frame, binding, out));
      case ReadKind::LEAD:
        co_return Outcome(makeCallback(binding, kj::mv(key), out.ticket));
      case ReadKind::WAIT:
        break;
      case ReadKind::MISS:
        KJ_FAIL_REQUIRE("unexpected memory cache miss with fallback");
    }
    auto* record = static_cast<const SharedMemoryCache::Binding*>(hostMemory::resolveSlot(binding));
    KJ_REQUIRE(record != nullptr && record->tag == SharedMemoryCache::BINDING_TAG);
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    bool registered = record->cache->addHostSideWaiter(key, out.ticket, kj::mv(paf.fulfiller));
    if (registered) co_await paf.promise;
  }
}

}  // namespace

bool SharedMemoryCache::addHostSideWaiter(kj::StringPtr key,
    uint64_t ticket,
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller) const {
  auto lock = data->lockExclusive();
  auto* f = findInFlight(*lock, key.asArray(), hashKey(key.asArray()));
  if (f == nullptr || f->ticket != ticket) return false;
  auto* w = newWaiter(*lock);
  KJ_REQUIRE(w != nullptr, "memory cache: the instance's pool is spent");
  w->kind = Waiter::FULFILLER;
  w->fulfiller = kj::mv(fulfiller);
  appendWaiter(*f, w);
  return true;
}

// A waiter inside a request: made in the waiter's own memory, named to the host by the handle
// of its entry in the resolve table, which the host hands back in the wake action and the
// waiter's own turn resolves. The host never touches the record.
class SharedMemoryCache::Use::WaitHolder {
 public:
  WaitHolder(uint64_t binding, kj::String key, uint64_t context)
      : binding(binding),
        key(kj::mv(key)),
        context(context),
        paf(kj::newPromiseAndFulfiller<Outcome>()) {}

  // Reads again; a hit or the lead settles the promise, and another wait leaves the waiter
  // registered under its cookie, as the first read did.
  void wake() {
    if (!paf.fulfiller->isWaiting()) return;
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      hostcall::Frame frame{};
      ReadOut out = doRead(frame, binding, key, true, context, self);
      switch (static_cast<ReadKind>(out.kind)) {
        case ReadKind::HIT:
        case ReadKind::HIT_LARGE:
          paf.fulfiller->fulfill(fetchValue(frame, binding, out));
          break;
        case ReadKind::LEAD:
          paf.fulfiller->fulfill(makeCallback(binding, kj::str(key), out.ticket));
          break;
        case ReadKind::WAIT:
          break;
        case ReadKind::MISS:
          KJ_FAIL_REQUIRE("unexpected memory cache miss with fallback");
      }
    })) {
      paf.fulfiller->reject(kj::mv(exception));
    }
  }

  uint64_t binding;
  kj::String key;
  uint64_t context;
  hostMemory::ResolveHandle self = 0;
  kj::PromiseFulfillerPair<Outcome> paf;
};

namespace {

// Owns the holder and its registration for the life of the promise.
class WaitRegistration {
 public:
  explicit WaitRegistration(kj::Own<Use::WaitHolder> holder): holder(kj::mv(holder)) {}
  ~WaitRegistration() noexcept(false) {
    if (holder->self != 0) hostMemory::unregisterResolve(holder->self);
  }
  KJ_DISALLOW_COPY_AND_MOVE(WaitRegistration);

 private:
  kj::Own<Use::WaitHolder> holder;
};

}  // namespace

void SharedMemoryCache::wakeHolder(uint64_t cookie) {
  auto* holder = static_cast<Use::WaitHolder*>(
      hostMemory::resolveOfType(cookie, hostMemory::resolveTypeTag<Use::WaitHolder>()));
  if (holder != nullptr) holder->wake();
}

SharedMemoryCache::Use::Use(kj::Own<const SharedMemoryCache> cache, const Limits& limits)
    : limits(limits) {
  // Host-side: a binding is made when a worker's bindings are compiled, under its isolate's
  // lock, outside any turn, so gk's record of the thread names the binder's arena.
  installWakeHook();
  const SharedMemoryCache& instance = *cache;
  binding =
      hostSlot<Binding>(BINDING_TAG, kj::mv(cache), limits, hostMemory::currentIsolateArena());
  instance.suggest(limits);
}

SharedMemoryCache::Use::~Use() noexcept(false) {
  if (binding == nullptr) return;
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
    Binding& record = *binding;
    record.cache->unsuggest(record);
  })) {
    KJ_LOG(ERROR, "memory cache binding teardown failed", exception);
  }
  // Woken while the instance is still held by this binding's record.
  wakeAfter();
  binding = nullptr;
}

kj::Maybe<kj::Own<CacheValue>> SharedMemoryCache::Use::getWithoutFallback(
    const kj::String& key, SpanBuilder& readSpan) const {
  hostcall::Frame frame{};
  ReadOut out = doRead(frame, binding.getHandle(), key, false, 0, 0);
  emitReadTags(readSpan, out);
  if (out.kind == static_cast<uint64_t>(ReadKind::MISS)) return kj::none;
  return fetchValue(frame, binding.getHandle(), out);
}

kj::OneOf<kj::Own<CacheValue>, kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>
SharedMemoryCache::Use::getWithFallback(const kj::String& key, SpanBuilder& readSpan) const {
  uint64_t handle = binding.getHandle();
  hostcall::Frame frame{};
  ReadOut out = doRead(frame, handle, key, true, 0, 0);
  switch (static_cast<ReadKind>(out.kind)) {
    case ReadKind::HIT:
    case ReadKind::HIT_LARGE:
      emitReadTags(readSpan, out);
      return fetchValue(frame, handle, out);
    case ReadKind::LEAD:
      readSpan.setTag("memory_cache_lock_wait_time_ns"_kjc, lockWaitNsForTrace(out.lockWaitNs));
      readSpan.setTag("cache_hit"_kjc, false);
      readSpan.setTag("coalesced_request"_kjc, false);
      readSpan.setTag("initiating_fallback"_kjc, true);
      readSpan.setTag("cache_total_size"_kjc, static_cast<double>(out.totalValueSize));
      readSpan.setTag("cache_entry_count"_kjc, static_cast<double>(out.entryCount));
      return kj::Promise<GetWithFallbackOutcome>(makeCallback(handle, kj::str(key), out.ticket));
    case ReadKind::WAIT:
      break;
    case ReadKind::MISS:
      KJ_FAIL_REQUIRE("unexpected memory cache miss with fallback");
  }

  // Cache miss, and another request is already fetching this key.
  readSpan.setTag("memory_cache_lock_wait_time_ns"_kjc, lockWaitNsForTrace(out.lockWaitNs));
  readSpan.setTag("cache_hit"_kjc, false);
  readSpan.setTag("coalesced_request"_kjc, true);
  readSpan.setTag("waiting_on_inflight"_kjc, true);
  readSpan.setTag("inflight_waiters_count"_kjc, static_cast<double>(out.waitersAhead + 1));
  auto waitSpan = readSpan.newChild("memory_cache_coalesce_wait"_kjc);
  waitSpan.setTag("key"_kjc, key.asPtr());
  waitSpan.setTag("waiters_ahead"_kjc, static_cast<double>(out.waitersAhead));

  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    auto holder = kj::heap<WaitHolder>(
        handle, kj::str(key), context.getCrossContextExecutor().getContextHandle());
    holder->self =
        hostMemory::registerResolve(holder.get(), hostMemory::resolveTypeTag<WaitHolder>());
    JSG_REQUIRE(holder->self != 0, Error,
        "Too many outstanding memory cache waiters; cannot register another.");
    // Registered as a waiter by this second read, unless the key settled between the two, in
    // which case this read answers.
    holder->wake();
    auto promise = kj::mv(holder->paf.promise);
    // We have to register a pending event with the I/O context so that the runtime does not
    // detect a hanging promise: another fallback is in progress, and once it settles the host
    // wakes this context, which reads again.
    return promise.attach(kj::heap<WaitRegistration>(kj::mv(holder)),
        context.registerPendingEvent(), kj::mv(waitSpan));
  }
  return hostSideWait(handle, kj::str(key)).attach(kj::mv(waitSpan));
}

void SharedMemoryCache::Use::delete_(const kj::String& key) const {
  KJ_REQUIRE(key.size() <= MAX_KEY_SIZE);
  hostcall::Frame frame{};
  DeleteIn in = {key.size()};
  putKey(frame, in, key.asArray());
  long result = callCache(frame, Op::DELETE, binding.getHandle());
  KJ_REQUIRE(result == 0, "memory cache delete failed", result);
}

// =======================================================================================
// The JavaScript binding.

// Attempts to serialize a JavaScript value. If that fails, this function throws
// a tunneled exception, see jsg::createTunneledException().
static kj::Own<CacheValue> hackySerialize(jsg::Lock& js, jsg::JsRef<jsg::JsValue>& value) {
  JSG_TRY(js) {
    jsg::Serializer serializer(js);
    serializer.write(js, value.getHandle(js));
    return kj::atomicRefcounted<CacheValue>(serializer.release().data);
  }
  JSG_CATCH(exception) {
    // We run into big problems with tunneled exceptions here. When
    // the toString() function of the JavaScript error is not marked
    // as side effect free, tunneling the exception fails entirely
    // because kj::str() returns an empty string for the error. As a
    // workaround, we drop the error object in that case and return
    // a generic error that only includes the type of the value.
    // TODO(later): remove this workaround
    if (kj::str(exception.getHandle(js)).size() == 0) {
      throw JSG_KJ_EXCEPTION(
          FAILED, DOMDataCloneError, "failed to serialize ", value.getHandle(js).typeOf(js));
    }

    // This is still pretty bad. We lose the original error stack.
    // TODO(later): remove string-based error tunneling
    throw js.exceptionToKj(kj::mv(exception));
  }
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> MemoryCache::read(jsg::Lock& js,
    jsg::NonCoercible<kj::String> key,
    jsg::Optional<FallbackFunction> optionalFallback) {
  if (key.value.size() > MAX_KEY_SIZE) {
    return js.rejectedPromise<jsg::JsRef<jsg::JsValue>>(js.rangeError("Key too large."_kj));
  }

  auto readSpan = IoContext::current().makeTraceSpan("memory_cache_read"_kjc);
  auto userReadSpan = IoContext::current().makeUserTraceSpan("memory_cache_read"_kjc);

  KJ_IF_SOME(fallback, optionalFallback) {
    KJ_SWITCH_ONEOF(cacheUse->getWithFallback(key.value, readSpan)) {
      KJ_CASE_ONEOF(result, kj::Own<CacheValue>) {
        // Optimization: Don't even release the isolate lock if the value is already in cache.
        jsg::Deserializer deserializer(js, result->asBytes());
        auto value = jsg::JsRef(js, deserializer.readValue(js));

        return js.resolvedPromise(kj::mv(value));
      }
      KJ_CASE_ONEOF(promise, kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>) {
        return IoContext::current().awaitIo(js, kj::mv(promise),
            [fallback = kj::mv(fallback), key = kj::str(key.value), readSpan = kj::mv(readSpan),
                userSpan = kj::mv(userReadSpan), self = JSG_THIS](
                jsg::Lock& js, SharedMemoryCache::Use::GetWithFallbackOutcome cacheResult) mutable
            -> jsg::Promise<jsg::JsRef<jsg::JsValue>> {
          KJ_SWITCH_ONEOF(cacheResult) {
            KJ_CASE_ONEOF(serialized, kj::Own<CacheValue>) {
              readSpan.setTag("fallback_cache_hit"_kjc, true);
              readSpan.setTag("entry_size"_kjc, static_cast<double>(serialized->size()));

              jsg::Deserializer deserializer(js, serialized->asBytes());
              return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
            }
            KJ_CASE_ONEOF(callback, SharedMemoryCache::Use::FallbackDoneCallback) {
              auto& context = IoContext::current();
              // The callback's destructor reports the fallback as failed if it never settled,
              // which releases the cache's in-progress entry for the key so that other readers
              // of it, in any isolate sharing the cache, proceed instead of waiting on this one.
              // It must do so also when the turn running the fallback is abandoned without its
              // frames returning (a guest fault, see jsg::runInGuest), so the callback is owned
              // by the context before the fallback runs: the context destroys it when the
              // context is torn down, whatever the turn left on its stack. (The
              // WORKERD_GK_NEG_CACHE_FALLBACK build, a negative-test build never shipped, owns
              // it only once the fallback has returned, so a fault inside the fallback strands
              // the entry; memory-cache-fault-test.sh must then fail on the hung reader.)
#ifndef WORKERD_GK_NEG_CACHE_FALLBACK
              auto ownedCallback = context.addObject(kj::heap(kj::mv(callback)));
              auto callbackPtr = context.addObject(*ownedCallback);
#else
              auto heapCallback = kj::heap(kj::mv(callback));
#endif

              // Create a span for the fallback execution
              auto fallbackSpan = readSpan.newChild("memory_cache_fallback"_kjc);
              fallbackSpan.setTag("key"_kjc, key.asPtr());

              // Refcount the spans so they can be shared between then/catch.
              auto fallbackSpanRc = kj::rc<SpanBuilder>(kj::mv(fallbackSpan));
              auto readSpanRc = kj::rc<SpanBuilder>(kj::mv(readSpan));

              auto fallbackPromise = js.evalNow([&]() {
                maybeForceGuestFaultInFallbackForTest(key);
                return fallback(js, kj::mv(key));
              });
#ifdef WORKERD_GK_NEG_CACHE_FALLBACK
              auto callbackPtr = context.addObject(*heapCallback);
              auto ownedCallback = context.addObject(kj::mv(heapCallback));
#endif
              return fallbackPromise
                  .then(js,
                      [callback = kj::mv(callbackPtr), fallbackSpan = fallbackSpanRc.addRef(),
                          readSpan = readSpanRc.addRef()](jsg::Lock& js,
                          CacheValueProduceResult result) mutable -> jsg::JsRef<jsg::JsValue> {
                // NOTE: `callback` is IoPtr, not IoOwn. The catch block gets the IoOwn, which
                //   ensures the object still exists at this point.
                fallbackSpan->setTag("fallback_success"_kjc, true);

                auto serialized = hackySerialize(js, result.value);
                fallbackSpan->setTag(
                    "fallback_result_size"_kjc, static_cast<double>(serialized->size()));

                KJ_IF_SOME(expiration, result.expiration) {
                  JSG_REQUIRE(
                      !kj::isNaN(expiration), TypeError, "Expiration time must not be NaN.");
                  fallbackSpan->setTag("has_expiration"_kjc, true);
                } else {
                  fallbackSpan->setTag("has_expiration"_kjc, false);
                }
                (*callback)(
                    SharedMemoryCache::Use::FallbackResult{kj::mv(serialized), result.expiration},
                    *fallbackSpan);
                return kj::mv(result.value);
              })
                  .catch_(js,
                  JSG_VISITABLE_LAMBDA(
                      (self = kj::mv(self), callback = kj::mv(ownedCallback),
                          fallbackSpan = fallbackSpanRc.addRef(), readSpan = readSpanRc.addRef()),
                      (self),
                      (jsg::Lock & js, jsg::Value&& exception) mutable->jsg::JsRef<jsg::JsValue> {
                        fallbackSpan->setTag("fallback_success"_kjc, false);
                        fallbackSpan->setTag(
                            "fallback_error"_kjc, kj::str(exception.getHandle(js)));
                        (*callback)(kj::none, *fallbackSpan);
                        js.throwException(kj::mv(exception));
                      }));
            }
          }
          KJ_UNREACHABLE;
        });
      }
    }
    KJ_UNREACHABLE;
  } else {
    KJ_IF_SOME(cacheValue, cacheUse->getWithoutFallback(key.value, readSpan)) {
      jsg::Deserializer deserializer(js, cacheValue->asBytes());
      return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
    }
    return js.resolvedPromise(jsg::JsRef(js, js.undefined()));
  }
}

void MemoryCache::delete_(jsg::Lock& js, jsg::NonCoercible<kj::String> key) {
  // Ignore operations on keys exceeding key max size.
  if (key.value.size() > MAX_KEY_SIZE) {
    js.throwException(js.rangeError("Key too large."_kj));
    return;
  }

  auto deleteSpan = IoContext::current().makeTraceSpan("memory_cache_delete"_kjc);
  deleteSpan.setTag("key"_kjc, key.value.asPtr());

  cacheUse->delete_(key.value);

  deleteSpan.setTag("delete_completed"_kjc, true);
}

// =======================================================================================
// The provider.

MemoryCacheProvider::MemoryCacheProvider(const kj::MonotonicClock& timer,
    kj::Maybe<SharedMemoryCache::AdditionalResizeMemoryLimitHandler>
        additionalResizeMemoryLimitHandler)
    : additionalResizeMemoryLimitHandler(kj::mv(additionalResizeMemoryLimitHandler)),
      registry(hostOwned<Registry>()),
      timer(timer) {}

MemoryCacheProvider::~MemoryCacheProvider() noexcept(false) {
  // The provider must outlive every SharedMemoryCache instance referencing it.
  KJ_REQUIRE(*registry->head.lockShared() == nullptr,
      "There are still active SharedMemoryCache instances. Use-after-free errors are likely.");
}

kj::Own<const SharedMemoryCache> MemoryCacheProvider::getInstance(
    kj::Maybe<kj::StringPtr> cacheId) const {

  const auto makeCache = [this](kj::Maybe<const MemoryCacheProvider&> provider, kj::StringPtr id) {
    // The cache doesn't exist in the registry. Let's create it.
    auto handler = additionalResizeMemoryLimitHandler.map(
        [](const SharedMemoryCache::AdditionalResizeMemoryLimitHandler& handler)
            -> SharedMemoryCache::AdditionalResizeMemoryLimitHandler& {
      return const_cast<SharedMemoryCache::AdditionalResizeMemoryLimitHandler&>(handler);
    });
    return SharedMemoryCache::create(provider, id, handler, timer);
  };

  KJ_IF_SOME(cid, cacheId) {
    auto head = registry->head.lockExclusive();

    // First, let's see if the cache already exists. If it does, we'll just return
    // a strong reference to it.
    for (auto** p = &*head; *p != nullptr; p = &(*p)->registryNext) {
      auto* found = *p;
      if (found->getId() != cid) continue;
      KJ_IF_SOME(ref, kj::atomicAddRefWeak(*found)) {
        return kj::mv(ref);
      }
      // We found an entry in the registry, but atomicAddRefWeak failed: the instance is being
      // destroyed and has not yet removed itself. Replace the entry with a new instance.
      auto cache = makeCache(kj::Maybe<const MemoryCacheProvider&>(*this), cid);
      auto* instance = const_cast<SharedMemoryCache*>(cache.get());
      instance->registryNext = found->registryNext;
      found->registryNext = nullptr;
      *p = instance;
      return kj::mv(cache);
    }

    // The cache doesn't exist, let's create it and add it to the registry
    auto cache = makeCache(kj::Maybe<const MemoryCacheProvider&>(*this), cid);
    auto* instance = const_cast<SharedMemoryCache*>(cache.get());
    instance->registryNext = *head;
    *head = instance;
    return kj::mv(cache);
  }

  // Since we don't have a cache id, we'll just create a new cache and return it.
  return makeCache(kj::none, nullptr);
}

void MemoryCacheProvider::removeInstance(const SharedMemoryCache& instance) const {
  // Only the instance itself is removed: an entry with the same id may already be a newer
  // instance that replaced this one.
  auto head = registry->head.lockExclusive();
  for (auto** p = &*head; *p != nullptr; p = &(*p)->registryNext) {
    if (*p == &instance) {
      *p = instance.registryNext;
      return;
    }
  }
}

}  // namespace workerd::api
