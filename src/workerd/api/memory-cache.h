#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/host-memory.h>
#include <workerd/util/hostcall.h>

#include <kj/async.h>
#include <kj/mutex.h>
#include <kj/time.h>

namespace workerd {
class SpanBuilder;
}

namespace workerd::api {

// The MemoryCache mechanism is an in-process, memory-resident data cache that
// can be configured for workers. A single cache instance can be unique to an
// individual worker or shared across multiple workers / isolates.
//
// Instances are configured as bindings on the worker (set up in the workers
// configuration) and accessible via the environment bindings passed into the
// worker handler functions:
//
//  async fetch(req, env) {
//    await env.MY_CACHE.read('key', () => {
//      // Called if the 'key' does not exist in the cache
//      return 'new value';
//    });
//  }
//
// The cache is only capable of storing values that are v8 serializable (so
// JS primitives other than Symbol, ordinary JavaScript objects but not class
// instances, etc). Objects that represent i/o (like streams or promises are
// explicitly not supported.

class CacheValueBacking {
 public:
  virtual kj::ArrayPtr<const kj::byte> asBytes() const = 0;
  virtual ~CacheValueBacking() noexcept(false) = default;
};

// A serialized value as a turn holds it: the bytes a fallback produced, or the copy of a cached
// value a read brought into the reader's own memory.
struct CacheValue: kj::AtomicRefcounted {
  CacheValue(kj::Array<kj::byte>&& bytes): data(kj::mv(bytes)) {}
  CacheValue(kj::Own<CacheValueBacking> backing): data(kj::mv(backing)) {}

  kj::ArrayPtr<const kj::byte> asBytes() const {
    KJ_SWITCH_ONEOF(data) {
      KJ_CASE_ONEOF(d, kj::Array<kj::byte>) {
        return d.asPtr();
      }
      KJ_CASE_ONEOF(b, kj::Own<CacheValueBacking>) {
        return b->asBytes();
      }
    }
    KJ_UNREACHABLE;
  }

  size_t size() const {
    return asBytes().size();
  }

 private:
  kj::OneOf<kj::Array<kj::byte>, kj::Own<CacheValueBacking>> data;
};

struct CacheValueProduceResult {
  jsg::JsRef<jsg::JsValue> value;
  jsg::Optional<double> expiration;
  JSG_STRUCT(value, expiration);
};

class MemoryCacheProvider;

// An in-memory cache that can be accessed by any number of workers/isolates
// within the same process.
//
// The instance is a host object (memory class H, util/host-memory.h): it, its tables, its keys
// and its values are in host memory, which no JS turn can read or write, and a turn operates on
// it through hostcalls (util/hostcall.h) whose handler runs host-side on the calling thread.
// What two workers share is decided by configuration -- bindings with the same cache id share
// one instance -- and the host enforces that contract rather than the memory layout: a binding
// is a host-side record of {instance, limits, the arena of the isolate that made it}, the
// MemoryCache object holds only a host slot handle to it, and the handler refuses a handle
// whose record was not made by the arena the calling thread runs under, by gk's own record of
// the thread. So a turn gets the values of the caches it is configured with, one copy per read
// in its own memory, and nothing of the tables, and a worker with no binding gets nothing. See
// "The shared memory cache" in experimental/guest-kernel/memory-classes.md.
//
// Reads, the leader's publish and abandon, and deletes are each one hostcall (one VM exit with
// the guest kernel active, a direct call without it), so the ordinary test suite exercises the
// same code. A hit copies the value into the caller's memory: through the frame when it fits,
// otherwise into a buffer the turn names within its own arena, which the handler checks
// against gk's record of that arena before writing. A read that finds its key in flight gets
// "wait" and nothing in host memory to wait on: the turn makes its promise in its own memory,
// registers its waiter record in the resolve table, and hands the host the handles of its
// context and of that record, both values the host validates before use; when the leader
// publishes or abandons, the handler moves the waiters to a wake list, and after the leader's
// turn the host, from the event loop, queues an action in each waiter's own context, where the
// waiter re-issues its read. Nothing the host dereferences is in a turn's memory, and no turn
// writes into another's.
class SharedMemoryCache: public kj::AtomicRefcounted {
 public:
  struct Limits {
    // The maximum number of keys that may exist within the cache at the same
    // time. The cache size grows at least linearly in the number of entries.
    uint32_t maxKeys;

    // The maximum size of each individual value, when serialized.
    uint32_t maxValueSize;

    // The maximum sum of all stored values. This is essentially the cache size,
    // except that it only includes the sizes of the values and does not account
    // for keys and the overhead of the data structures themselves.
    uint64_t maxTotalValueSize;

    bool operator==(const Limits& b) const = default;

    Limits normalize() const KJ_WARN_UNUSED_RESULT {
      // Avoid surprises due to misconfigured bindings that set one or more limits to 0.
      if (maxKeys == 0 || maxValueSize == 0 || maxTotalValueSize == 0) {
        return min();
      }

      // If a binding specifies a maxValueSize that exceeds the maxTotalValueSize, remedy
      // that by reducing the maxValueSize.
      return Limits{
        .maxKeys = maxKeys,
        .maxValueSize = static_cast<uint32_t>(kj::min(maxValueSize, maxTotalValueSize)),
        .maxTotalValueSize = maxTotalValueSize,
      };
    }

    static constexpr Limits min() {
      return {0, 0, 0};
    }

    static Limits max(const Limits& a, const Limits& b) {
      return Limits{
        kj::max(a.maxKeys, b.maxKeys),
        kj::max(a.maxValueSize, b.maxValueSize),
        kj::max(a.maxTotalValueSize, b.maxTotalValueSize),
      };
    }
  };

  // What an embedder's resize handler sees of the instance: the effective limits, which it may
  // lower (a process-wide cap, see MemoryCachePolicy).
  struct ThreadUnsafeData {
    Limits effectiveLimits = Limits::min();
  };
  using AdditionalResizeMemoryLimitHandler = kj::Function<void(ThreadUnsafeData&)>;

  KJ_DISALLOW_COPY_AND_MOVE(SharedMemoryCache);

  SharedMemoryCache(kj::Maybe<const MemoryCacheProvider&> provider,
      kj::StringPtr id,
      kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler,
      const kj::MonotonicClock& timer);
  ~SharedMemoryCache() noexcept(false);

  // The instance is constructed in host memory: kj::atomicRefcounted<SharedMemoryCache>() and
  // the refcount's `delete this` go through these.
  static void* operator new(size_t size);
  static void operator delete(void* ptr, size_t size);

  kj::StringPtr getId() const {
    return kj::StringPtr(id, idLength);
  }

  static kj::Own<const SharedMemoryCache> create(kj::Maybe<const MemoryCacheProvider&> provider,
      kj::StringPtr id,
      kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler,
      const kj::MonotonicClock& timer);

  // A binding's record: in host memory, named by a host slot handle, made host-side when the
  // binding is created under the binder's isolate lock. The arena is gk's record of that thread,
  // which the hostcall handler compares against the arena it runs under (see the class
  // comment); the tag tells this slot's object from another host-memory tenant's.
  struct WD_MEMORY_CLASS_HOST Binding {
    uint64_t tag;
    kj::Own<const SharedMemoryCache> cache;
    Limits limits;
    hostMemory::IsolateArena arena;
  };
  static constexpr uint64_t BINDING_TAG = 0x6d63616368652d62ull;

  // RAII class that attaches itself to a cache, suggests cache limits to the
  // cache it is attached to, and allows interacting with the cache.
  class Use {
   public:
    KJ_DISALLOW_COPY(Use);

    Use(kj::Own<const SharedMemoryCache> cache, const Limits& limits);
    Use(Use&& other) = default;
    ~Use() noexcept(false);

    // Returns a cached value for the given key if one exists (and has not
    // expired). If no such value exists, nothing is returned, regardless of any
    // in-progress fallbacks trying to produce such a value.
    kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
        const kj::String& key, SpanBuilder& readSpan) const;

    struct FallbackResult {
      kj::Own<CacheValue> value;
      kj::Maybe<double> expiration;
    };
    using FallbackDoneCallback = kj::Function<void(kj::Maybe<FallbackResult>, SpanBuilder&)>;
    using GetWithFallbackOutcome = kj::OneOf<kj::Own<CacheValue>, FallbackDoneCallback>;

    // Returns either:
    // 1. The immediate value, if already in cache.
    // 2. A Promise that will eventually resolve either to the cached value
    //    or to a FallbackDoneCallback. In the latter case, the caller should
    //    invoke the fallback function.
    kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
        const kj::String& key, SpanBuilder& readSpan) const;

    void delete_(const kj::String& key) const;

    // A waiter inside a request; see getWithFallback().
    class WaitHolder;

   private:
    HostSlot<Binding> binding;
    Limits limits;
  };

  // The hostcall every operation of a Use goes through, installed by workerd's main once
  // gk_init() has run; installHostcall() also readies the host-side state the handler and the
  // end-of-turn wake draw on, so that the handler's first use is not its initialization.
  static const hostcall::Entry HOSTCALL;
  static void installHostcall();

  // Operations of the hostcall, in args[0]; args[1] is the binding's handle.
  enum class Op : uint64_t {
    READ = 1,        // in: ReadIn + key; out: ReadOut (+ the value when it fits the frame)
    READ_RANGE = 2,  // args: a buffer in the caller's arena; the pinned value is copied there
    READ_CHUNK = 3,  // args: an offset; out: the pinned value's bytes from there, one frame
    PUBLISH = 4,     // in: PublishIn + key (+ the value inline or in an arena range); out: WriteOut
    PUBLISH_BEGIN = 5,  // in: PublishIn + key: a value larger than the frame begins, staged
    PUBLISH_CHUNK = 6,  // in: the staged value's next bytes
    PUBLISH_END = 7,    // in: PublishIn + key: the staged value is stored; out: WriteOut
    ABANDON = 8,        // in: AbandonIn + key: the leader gives the key up; out: WriteOut
    DELETE = 9,         // in: DeleteIn + key
  };

  // Host-side: wakes the waiters every instance has set aside since the last drain. Run at the
  // end of each turn by the host (hostMemory::setTurnEndHook) and directly by a host-side
  // publish or abandon.
  static void drainWakes();

  // Host-side, for a caller outside any turn that found its key in flight: waits on `fulfiller`
  // with the key, if the key is still in flight under `ticket`.
  bool addHostSideWaiter(kj::StringPtr key,
      uint64_t ticket,
      kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller) const;

  // The instance's data structures, named here for the file-local helpers that operate on them.
  struct Data;
  struct Entry;
  struct Value;
  struct InFlight;
  struct Waiter;
  struct Wake;
  struct SuggestedLimit;
  struct PinState;

 private:
  friend class MemoryCacheProvider;

  static long hostcallHandler(hostcall::Frame& frame);
  static long handleCall(hostcall::Frame& frame);
  long handleRead(hostcall::Frame& frame, const Binding& binding, PinState& pin) const;
  long handleReadRange(hostcall::Frame& frame, PinState& pin) const;
  long handleReadChunk(hostcall::Frame& frame, PinState& pin) const;
  long handlePublish(hostcall::Frame& frame, const Binding& binding, PinState& pin, Op op) const;
  long handleAbandon(hostcall::Frame& frame) const;
  long handleDelete(hostcall::Frame& frame) const;
  void unpin(PinState& pin) const;
  // The wake action a waiter's context runs: resolves the waiter's record by its cookie, in the
  // waiter's own turn, and has it read again.
  static void wakeHolder(uint64_t cookie);

  // Called when initializing globals (i.e., bindings) for an isolate. Each
  // cache binding holds one SharedMemoryCache::Use, which automatically calls
  // this function when created. This call will never reduce the effective cache
  // limits, but might increase them.
  void suggest(const Limits& limits) const;

  // Called when a cache global and its associated SharedMemoryCache::Use is
  // destroyed. This call might reduce the effective cache limits. If all uses
  // have been destroyed, the effective limits will be reset to Limits::min(),
  // effectively clearing the cache. Every key the binding is leading a fallback for is
  // abandoned first, so that its waiters are woken rather than stranded.
  void unsuggest(const Binding& binding) const;

  void resize(Data& data) const;
  void setAsideWaiters(Data& data, InFlight& inFlight) const;

  // To ensure thread-safety, all mutable data is guarded by a mutex. Each cache
  // operation requires an exclusive lock. Even read-only operations need to
  // update the recency of cache entries, which requires a lock.
  HostOwn<kj::MutexGuarded<Data>> data;

  // The MemoryCacheProvider instance needs to be guaranteed to outlive the SharedMemoryCache
  // instance. When the SharedMemoryCache is destroyed, it will remove itself from the provider.
  kj::Maybe<const MemoryCacheProvider&> provider;

  // The id, copied here because the provider's registry entry can be removed before the
  // instance is fully destroyed. Host memory, like the instance.
  char* id;
  size_t idLength;

  // The provider owns the handler, and outlives the instance.
  kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler;

  const kj::MonotonicClock& timer;

  // The provider's registry of shared instances, and the queue of instances with waiters to
  // wake: both intrusive, both in host memory.
  SharedMemoryCache* registryNext = nullptr;
  SharedMemoryCache* wakeNext = nullptr;
};

struct MemoryCachePolicy {
  kj::Maybe<uint64_t> maxTotalValueSize;
};

class MemoryCacheUse {
 public:
  using FallbackResult = SharedMemoryCache::Use::FallbackResult;
  using FallbackDoneCallback = SharedMemoryCache::Use::FallbackDoneCallback;
  using GetWithFallbackOutcome = SharedMemoryCache::Use::GetWithFallbackOutcome;

  virtual ~MemoryCacheUse() noexcept(false) = default;
  virtual kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
      const kj::String& key, SpanBuilder& readSpan) const = 0;
  virtual kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
      const kj::String& key, SpanBuilder& readSpan) const = 0;
  virtual void delete_(const kj::String& key) const = 0;
};

class MemoryCacheNamespace {
 public:
  static kj::Own<MemoryCacheNamespace> create(MemoryCachePolicy policy);
  virtual ~MemoryCacheNamespace() noexcept(false) = default;
  virtual kj::Own<MemoryCacheUse> getBinding(
      kj::Maybe<kj::StringPtr> id, SharedMemoryCache::Limits limits) const = 0;
};

// JavaScript class that allows accessing an in-memory cache.
// Each instance forwards JavaScript calls to the selected backend lease.
class MemoryCache: public jsg::Object {
 public:
  MemoryCache(kj::Own<MemoryCacheUse> use): cacheUse(kj::mv(use)) {}

  using FallbackFunction = jsg::Function<jsg::Promise<CacheValueProduceResult>(kj::String)>;

  // Reads a value from the cache or invokes a fallback function to obtain the
  // value, if a fallback function was given.
  jsg::Promise<jsg::JsRef<jsg::JsValue>> read(jsg::Lock& js,
      jsg::NonCoercible<kj::String> key,
      jsg::Optional<FallbackFunction> optionalFallback);

  // Delete a value from the cache.
  void delete_(jsg::Lock& js, jsg::NonCoercible<kj::String> key);

  JSG_RESOURCE_TYPE(MemoryCache, CompatibilityFlags::Reader flags) {
    JSG_METHOD(read);
    if (flags.getMemoryCacheDelete()) {
      JSG_METHOD_NAMED(delete, delete_);
    }
  }

 private:
  kj::Own<MemoryCacheUse> cacheUse;
};

// The MemoryCacheProvider provides the internal implementation of the MemoryCache mechanism.
// It is responsible for owning the SharedMemoryCache instances and providing them to the
// bindings as needed. The selected implementation is fixed when the provider is constructed.
// TODO(later): It may be worth considering some kind of metrics observer for the provider
// that can be passed along to the individual cache instances so we can monitor just how much
// the in memory cache is being used.
class MemoryCacheProvider {
 public:
  explicit MemoryCacheProvider(const kj::MonotonicClock& timer);
  MemoryCacheProvider(const kj::MonotonicClock& timer,
      kj::Maybe<SharedMemoryCache::AdditionalResizeMemoryLimitHandler>
          additionalResizeMemoryLimitHandler);
  MemoryCacheProvider(const kj::MonotonicClock& timer, MemoryCachePolicy policy);
  KJ_DISALLOW_COPY_AND_MOVE(MemoryCacheProvider);
  ~MemoryCacheProvider() noexcept(false);

  kj::Own<MemoryCacheUse> getUse(
      kj::Maybe<kj::StringPtr> cacheId, SharedMemoryCache::Limits limits) const;
  kj::Own<const SharedMemoryCache> getInstance(kj::Maybe<kj::StringPtr> cacheId = kj::none) const;

  void removeInstance(const SharedMemoryCache& instance) const;

 private:
  kj::Maybe<SharedMemoryCache::AdditionalResizeMemoryLimitHandler>
      additionalResizeMemoryLimitHandler;

  // All existing in-memory *shared* caches, by id: an intrusive list through the instances,
  // whose head is in host memory, since an instance is reached from here host-side at binding
  // creation and from its own destructor. Caches created without an id (and therefore not
  // shareable) are not in it. An instance removes itself by calling removeInstance.
  struct WD_MEMORY_CLASS_HOST Registry {
    Registry(): head(nullptr) {}
    kj::MutexGuarded<SharedMemoryCache*> head;
  };
  HostOwn<Registry> registry;

  kj::Maybe<kj::Own<MemoryCacheNamespace>> namespaceV2;

  const kj::MonotonicClock& timer;

  friend bool isMemoryCacheV2ForTest(const MemoryCacheProvider& provider);
};

// clang-format off
#define EW_MEMORY_CACHE_ISOLATE_TYPES                                                   \
  api::MemoryCache,                                                                     \
  api::CacheValueProduceResult
// clang-format on

}  // namespace workerd::api
