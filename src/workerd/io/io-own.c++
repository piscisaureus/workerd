#include "io-own.h"

#include "io-context.h"

#include <workerd/jsg/util.h>

#include <kj/mutex.h>

#include <atomic>
#include <cerrno>
#include <cstring>

namespace workerd {

namespace {

// What an entry's companion holds (hostMemory::ResolveCompanion::kind): a context's record, or
// an owned node.
enum CompanionKind : uint64_t { NONE = 0, CONTEXT = 1, NODE = 2 };

// A node of an owned list, in host memory (see OwnedObjectList).
struct OwnedNode {
  OwnedNode* next;
  OwnedNode** prev;  // the previous node's `next`, or the record's head word
  void* disposal;    // the object, as its disposer takes it
  const kj::Disposer* disposer;
  hostMemory::ResolveHandle handle;   // the entry naming the object
  hostMemory::ResolveHandle context;  // the entry of the context whose list this is
};

// The words a registration carries through the turn's journal (see OwnedObjectList::add and
// DeleteQueueOwner).
enum AuxWord : size_t { AUX_KIND = 0, AUX_DISPOSER = 1, AUX_CONTEXT = 2, AUX_DISPOSAL = 3 };
static_assert(hostMemory::RESOLVE_AUX_WORDS == 4);

// An action another context asked this one to run, boxed on the heap so that the queue holds
// one word per action, which the host moves and never calls through: the turn that queues the
// action builds the box, and the turn that runs it, or the teardown, destroys it.
using Action = kj::Function<void(jsg::Lock&)>;

// What a turn notes for the host (hostMemory::noteInTurn): a holder dropped off its context
// {DELETION, context, handle, 0}, or an action for another context {ACTION, context, box, 0}.
enum NoteKind : uint64_t { DELETION = 1, ACTION = 2 };

// A chunk of a queue, in host memory: handles for the deletions, boxes for the actions.
constexpr size_t CHUNK_WORDS = 126;  // a 1 KiB block
struct Chunk {
  Chunk* next;
  uint32_t count;
  uint64_t words[CHUNK_WORDS];
};
static_assert(sizeof(Chunk) == 1024);

std::atomic<size_t> nodesAllocated{0};
std::atomic<size_t> nodesFreed{0};
std::atomic<size_t> recordsAllocated{0};
std::atomic<size_t> recordsFreed{0};
std::atomic<size_t> chunksAllocated{0};
std::atomic<size_t> chunksFreed{0};

// A queue: a chain of chunks appended at the tail, and taken whole.
struct ChunkQueue {
  Chunk* head = nullptr;
  Chunk* tail = nullptr;

  void push(uint64_t word) {
    if (tail == nullptr || tail->count == CHUNK_WORDS) {
      chunksAllocated.fetch_add(1, std::memory_order_relaxed);
      auto* chunk = new (hostMemory::allocate(sizeof(Chunk), alignof(Chunk))) Chunk();
      if (tail == nullptr) {
        head = chunk;
      } else {
        tail->next = chunk;
      }
      tail = chunk;
    }
    tail->words[tail->count++] = word;
  }

  bool empty() const {
    return head == nullptr;
  }

  // Takes the chain out of the queue, leaving it empty.
  Chunk* detach() {
    Chunk* chain = head;
    head = tail = nullptr;
    return chain;
  }
};

void freeChunk(Chunk* chunk) {
  chunksFreed.fetch_add(1, std::memory_order_relaxed);
  hostMemory::deallocate(chunk, sizeof(Chunk), alignof(Chunk));
}

// Visits every word of the chain, freeing the chunks as it goes.
template <typename Func>
void consumeChain(Chunk* chain, Func&& fn) {
  while (chain != nullptr) {
    Chunk* chunk = chain;
    chain = chunk->next;
    for (uint32_t i = 0; i < kj::min(chunk->count, static_cast<uint32_t>(CHUNK_WORDS)); i++) {
      fn(chunk->words[i]);
    }
    freeChunk(chunk);
  }
}

// The cross-thread queue of a context, under the record's lock.
struct CrossThreadQueue {
  // Set by the owned list's teardown before it walks the list, after which a holder of one of
  // the context's objects dropped by an object the walk disposes of queues nothing: the object
  // it names has gone, or is going, with the list.
  bool tearingDown = false;
  // Handles of objects whose holders were dropped off the context, released host-side when the
  // context drains the queue (see OwnedObjectList::release).
  ChunkQueue deletions;
  // Actions (boxed, as Action*) that other contexts have asked this one to run, taken by the
  // context when the signal below wakes it.
  ChunkQueue actions;
  // Wakes the context's signal task when an action is queued; null until the task installs
  // it (DeleteQueueOwner::resetCrossThreadSignal), and replaced each time the task has run.
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller;
};
static_assert(sizeof(kj::Own<kj::CrossThreadPromiseFulfiller<void>>) == 2 * sizeof(void*));

// A context's record, in host memory, behind its entry's companion (see DeleteQueueOwner).
struct ContextRecord {
  OwnedNode* head = nullptr;  // the owned list; touched only on the context's thread
  kj::MutexGuarded<CrossThreadQueue> cross;
};

// WORKERD_GK_NEG_IOOWN_NODE and WORKERD_GK_NEG_DELETE_QUEUE are negative-test builds, never
// shipped: the nodes, or the records, are placed on the ordinary heap, where a turn can write
// them, which is the state before each moved to host memory. The guest kernel's ioown-test.sh
// must then fail -- with the host's walk following the links and calling the disposer a turn
// wrote into a node, or the host calling the fulfiller a turn wrote into a record; this is the
// check that the test tests the placement.
OwnedNode* allocateNode() {
  nodesAllocated.fetch_add(1, std::memory_order_relaxed);
#ifdef WORKERD_GK_NEG_IOOWN_NODE
  return new OwnedNode();
#else
  return new (hostMemory::allocate(sizeof(OwnedNode), alignof(OwnedNode))) OwnedNode();
#endif
}

void freeNode(OwnedNode* node) {
  nodesFreed.fetch_add(1, std::memory_order_relaxed);
#ifdef WORKERD_GK_NEG_IOOWN_NODE
  delete node;
#else
  hostMemory::deallocate(node, sizeof(OwnedNode), alignof(OwnedNode));
#endif
}

ContextRecord* allocateRecord() {
  recordsAllocated.fetch_add(1, std::memory_order_relaxed);
#ifdef WORKERD_GK_NEG_DELETE_QUEUE
  return new ContextRecord();
#else
  return new (hostMemory::allocate(sizeof(ContextRecord), alignof(ContextRecord))) ContextRecord();
#endif
}

// Frees a record whose queue has been emptied and whose list has been walked. The caller holds
// no lock on it.
void freeRecord(ContextRecord* record) {
  recordsFreed.fetch_add(1, std::memory_order_relaxed);
#ifdef WORKERD_GK_NEG_DELETE_QUEUE
  delete record;
#else
  record->~ContextRecord();
  hostMemory::deallocate(record, sizeof(ContextRecord), alignof(ContextRecord));
#endif
}

// The registry lock: what keeps a record alive for a thread that has found it through the
// companion and is about to lock it, against the owning thread tearing the record down. A push
// from another thread holds it shared across the lookup and the push; a teardown holds it
// exclusive while it takes the record out of the companion. The owning thread's own drains
// take neither, since the teardown is on that thread. In host memory, reached through a host
// slot handle in a function-local static, like the companion directory (host-memory.c++): a
// rewritten handle resolves to nothing and the use is refused.
kj::MutexGuarded<int>* registryLock() {
  // Never freed: records are torn down from threads that may outlive static destruction.
  static HostSlot<kj::MutexGuarded<int>>* slot =
      new HostSlot<kj::MutexGuarded<int>>(hostSlot<kj::MutexGuarded<int>>());
  return slot->get();
}

OwnedNode** headOf(ContextRecord& record) {
  return &record.head;
}

void linkNode(OwnedNode* node, ContextRecord& record) {
  OwnedNode** head = headOf(record);
  node->next = *head;
  node->prev = head;
  if (node->next != nullptr) node->next->prev = &node->next;
  *head = node;
}

void unlinkNode(OwnedNode* node) {
  if (node->next != nullptr) node->next->prev = node->prev;
  *node->prev = node->next;
  node->next = nullptr;
  node->prev = nullptr;
}

// The node `handle` names, or null: the entry must be live and its companion a node. Host-side.
OwnedNode* nodeOf(hostMemory::ResolveHandle handle) {
  KJ_IF_SOME(c, hostMemory::resolveCompanion(handle)) {
    if (c.kind == NODE) return static_cast<OwnedNode*>(c.ptr);
  }
  return nullptr;
}

// The record `context` names, or null: the entry must be live and its companion a record.
// Host-side. A null answer for a context that has ended is the normal case for a holder that
// outlived it, and is silent.
ContextRecord* recordOf(hostMemory::ResolveHandle context) {
  KJ_IF_SOME(c, hostMemory::resolveCompanion(context)) {
    if (c.kind == CONTEXT) return static_cast<ContextRecord*>(c.ptr);
  }
  return nullptr;
}

// The operations of the IO_CONTEXT hostcall.
constexpr uint64_t DRAIN = 1;
constexpr uint64_t TEARDOWN = 2;

// Takes the node out of its list and retires its entry; the object is untouched, for the
// caller to dispose of before it frees the node. Host-side.
void detachNode(OwnedNode* node) {
  unlinkNode(node);
  hostMemory::retireResolve(node->handle);
  node->handle = 0;
}

// Host-side, on any thread: queues `handle` for `context` to release, after checking that the
// context is live and that the handle names an object of that context's own list. A handle
// that fails the check is reported and queues nothing: the object, if there is one, stays with
// its list and is disposed of when that list is torn down.
void queueDeletionHost(hostMemory::ResolveHandle context, hostMemory::ResolveHandle handle) {
  kj::MutexGuarded<int>* registry = registryLock();
  if (registry == nullptr) {
    KJ_LOG(ERROR, "the delete queue registry does not resolve; nothing is queued");
    return;
  }
  auto guard = registry->lockShared();
  ContextRecord* record = recordOf(context);
  if (record == nullptr) return;  // the context has ended, and its list took the object
  auto lock = record->cross.lockExclusive();
  if (lock->tearingDown) return;  // the context's list is taking the object
  OwnedNode* node = nodeOf(handle);
  if (node == nullptr) {
    KJ_LOG(ERROR, "IoOwn handle does not name a live owned object; nothing is released",
        kj::hex(handle));
    return;
  }
  if (node->context != context) {
    KJ_LOG(ERROR, "IoOwn handle names an object of another context; nothing is released",
        kj::hex(handle), kj::hex(context), kj::hex(node->context));
    return;
  }
  lock->deletions.push(handle);
}

// Host-side, on any thread: queues the boxed action for `context` and wakes it. False, with the
// box untouched, if the context has ended.
bool queueActionHost(hostMemory::ResolveHandle context, Action* box) {
  kj::MutexGuarded<int>* registry = registryLock();
  if (registry == nullptr) {
    KJ_LOG(ERROR, "the delete queue registry does not resolve; nothing is queued");
    return false;
  }
  auto guard = registry->lockShared();
  ContextRecord* record = recordOf(context);
  if (record == nullptr) return false;
  auto lock = record->cross.lockExclusive();
  lock->actions.push(reinterpret_cast<uintptr_t>(box));
  if (lock->fulfiller.get() != nullptr) lock->fulfiller->fulfill();
  return true;
}

// What a teardown takes out of a record, to destroy once no lock is held: the fulfiller, and the
// actions, which may run arbitrary destructors. The deletions are dropped: the objects went with
// the list. Host-side; takes no heap. The record's own lock is held by the caller.
struct TakenQueue {
  kj::CrossThreadPromiseFulfiller<void>* fulfiller = nullptr;
  const kj::Disposer* fulfillerDisposer = nullptr;
  Chunk* actions = nullptr;
};
TakenQueue takeQueue(CrossThreadQueue& queue) {
  TakenQueue taken;
  if (queue.fulfiller.get() != nullptr) {
    taken.fulfillerDisposer = &queue.fulfiller.getDisposer();
    taken.fulfiller = queue.fulfiller.disown(taken.fulfillerDisposer);
  }
  taken.actions = queue.actions.detach();
  consumeChain(queue.deletions.detach(), [](uint64_t) {});
  return taken;
}

}  // namespace

hostMemory::ResolveHandle OwnedObjectList::add(
    void* object, void* disposal, const kj::Disposer& disposer) {
  KJ_REQUIRE(context != 0, "the owned list has no context");
  uint64_t aux[hostMemory::RESOLVE_AUX_WORDS] = {
    NODE, reinterpret_cast<uintptr_t>(&disposer), context, reinterpret_cast<uintptr_t>(disposal)};
  hostMemory::ResolveHandle result;
  if (hostMemory::inTurn()) {
    result = hostMemory::registerResolve(object, aux);
  } else {
    result = hostMemory::registerResolve(object);
    if (result != 0 && !committed(result, object, aux)) {
      hostMemory::unregisterResolve(result);
      result = 0;
    }
  }
  KJ_REQUIRE(result != 0, "the resolve table is full; cannot create another I/O object",
      hostMemory::RESOLVE_CAPACITY);
  return result;
}

bool OwnedObjectList::committed(hostMemory::ResolveHandle handle, void* ptr, const uint64_t* aux) {
  switch (aux[AUX_KIND]) {
    case NONE:
      return true;
    case CONTEXT:
      return hostMemory::setResolveCompanion(handle, {CONTEXT, allocateRecord()});
    case NODE: {
      auto* disposer = reinterpret_cast<const kj::Disposer*>(aux[AUX_DISPOSER]);
      auto* disposal = reinterpret_cast<void*>(aux[AUX_DISPOSAL]);
      if (disposer == nullptr || disposal == nullptr) {
        KJ_LOG(ERROR, "an owned object's registration names no disposer; it is refused",
            kj::hex(handle));
        return false;
      }
      hostMemory::ResolveHandle context = aux[AUX_CONTEXT];
      ContextRecord* record = recordOf(context);
      if (record == nullptr) {
        KJ_LOG(ERROR, "an owned object's registration names no live context; it is refused",
            kj::hex(handle), kj::hex(context));
        return false;
      }
      OwnedNode* node = allocateNode();
      node->disposal = disposal;
      node->disposer = disposer;
      node->handle = handle;
      node->context = context;
      linkNode(node, *record);
      if (!hostMemory::setResolveCompanion(handle, {NODE, node})) {
        unlinkNode(node);
        freeNode(node);
        return false;
      }
      return true;
    }
  }
  KJ_LOG(ERROR, "a resolve registration carries an unknown kind; it is refused", kj::hex(handle),
      aux[AUX_KIND]);
  return false;
}

void OwnedObjectList::released(hostMemory::ResolveHandle handle) {
  KJ_IF_SOME(c, hostMemory::resolveCompanion(handle)) {
    switch (c.kind) {
      case NODE: {
        // Released by the turn, which disposed of the object itself.
        auto* node = static_cast<OwnedNode*>(c.ptr);
        unlinkNode(node);
        freeNode(node);
        return;
      }
      case CONTEXT: {
        // A context's entry is retired by its teardown, which empties the record first (see
        // DeleteQueueOwner); a record still here was left by a teardown that could not reach
        // it. Nothing in it can be disposed of here, which may run inside a hostcall: the
        // nodes and chunks are freed and the objects and actions leak.
        auto* record = static_cast<ContextRecord*>(c.ptr);
        OwnedNode** head = headOf(*record);
        if (*head != nullptr) {
          KJ_LOG(ERROR, "a context's record was released with objects still in it; they leak",
              kj::hex(handle));
        }
        while (OwnedNode* node = *head) {
          detachNode(node);
          freeNode(node);
        }
        TakenQueue taken;
        {
          auto lock = record->cross.lockExclusive();
          taken = takeQueue(*lock);
        }
        if (taken.fulfiller != nullptr || taken.actions != nullptr) {
          KJ_LOG(ERROR, "a context's record was released with its queue still in it; it leaks",
              kj::hex(handle));
        }
        consumeChain(taken.actions, [](uint64_t) {});
        freeRecord(record);
        return;
      }
    }
  }
}

bool OwnedObjectList::noted(const uint64_t* words, bool inHandler) {
  switch (words[0]) {
    case DELETION:
      queueDeletionHost(words[1], words[2]);
      return true;
    case ACTION: {
      auto* box = reinterpret_cast<Action*>(static_cast<uintptr_t>(words[2]));
      if (box == nullptr) return true;
      if (queueActionHost(words[1], box)) return true;
      // The context ended before the turn did: the action is dropped. Destroying it runs the
      // closure's destructor, which may take the heap, so not inside a hostcall; the note waits
      // for the end of the turn.
      if (inHandler) return false;
      delete box;
      return true;
    }
  }
  KJ_LOG(ERROR, "a turn's note carries an unknown kind; it is ignored", words[0]);
  return true;
}

const hostMemory::ResolveTurnHooks OwnedObjectList::TURN_HOOKS = {
  &OwnedObjectList::committed, &OwnedObjectList::released, &OwnedObjectList::noted};

void OwnedObjectList::releaseInTurn(hostMemory::ResolveHandle handle) {
  hostMemory::unregisterResolve(handle);
}

void OwnedObjectList::drop(hostMemory::ResolveHandle handle, const OwnedDisposal& disposal) {
  if (hostMemory::inTurn()) {
    // Inside the turn, where the list cannot be reached: dispose of the object here and release
    // the entry through the journal; the host frees the node after the turn.
    void* object = hostMemory::resolve(handle);
    releaseInTurn(handle);
    if (object != nullptr) disposal.dispose(object, *disposal.disposer);
  } else {
    release(handle, DestroyOwnedObject::YES);
  }
}

void OwnedObjectList::release(hostMemory::ResolveHandle handle, DestroyOwnedObject destroy) {
  OwnedNode* node = nodeOf(handle);
  if (node == nullptr) {
    KJ_LOG(ERROR, "IoOwn handle does not name a live owned object; nothing is released",
        kj::hex(handle));
    return;
  }
  detachNode(node);
  KJ_DEFER(freeNode(node));
  if (destroy) node->disposer->dispose(node->disposal);
}

const kj::Disposer& OwnedObjectList::take(hostMemory::ResolveHandle handle) {
  OwnedNode* node = nodeOf(handle);
  if (node == nullptr) {
    KJ_LOG(
        ERROR, "IoOwn handle does not name a live owned object; nothing is taken", kj::hex(handle));
    KJ_FAIL_REQUIRE("IoOwn handle does not name a live owned object", kj::hex(handle));
  }
  detachNode(node);
  const kj::Disposer& disposer = *node->disposer;
  freeNode(node);
  return disposer;
}

OwnedObjectList::~OwnedObjectList() noexcept(false) {
  if (context == 0) return;
  hostMemory::ResolveHandle ctx = context;
  context = 0;
  if (hostMemory::inTurn()) {
    // Torn down inside a turn, where the nodes cannot be reached. What this turn registered
    // into the list is still in the journal, with the disposer, and is disposed of from there;
    // what was committed before is drained through the hostcall, which hands the objects over
    // with their disposers and frees their nodes. Each object is disposed of here, in the turn,
    // as it would be host-side.
    //
    // An object this turn has already released is disposed of, and its node still linked until
    // the release reaches the host; the retire list is flushed first so that the drain does not
    // hand such an object over again.
    hostMemory::flushReleasedInTurn();
    hostMemory::releaseJournaled([ctx](hostMemory::ResolveHandle, void*, const uint64_t* aux) {
      if (aux[AUX_KIND] != NODE || aux[AUX_CONTEXT] != ctx) return false;
      auto* disposer = reinterpret_cast<const kj::Disposer*>(aux[AUX_DISPOSER]);
      auto* disposal = reinterpret_cast<void*>(aux[AUX_DISPOSAL]);
      if (disposer != nullptr && disposal != nullptr) disposer->dispose(disposal);
      return true;
    });
    struct Handed {
      void* disposal;
      const kj::Disposer* disposer;
    };
    constexpr size_t BATCH = hostcall::BYTES / sizeof(Handed);
    for (;;) {
      hostcall::Frame frame;
      frame.args[0] = DRAIN;
      frame.args[1] = ctx;
      frame.args[2] = frame.args[3] = 0;
      frame.inLen = 0;
      long n = hostcall::call(HOSTCALL, frame);
      if (n <= 0) break;
      size_t count = kj::min(static_cast<size_t>(n), BATCH);
      if (frame.outLen < count * sizeof(Handed)) break;
      Handed handed[BATCH];
      memcpy(handed, frame.buf, count * sizeof(Handed));
      for (size_t i = 0; i < count; i++) {
        if (handed[i].disposer != nullptr && handed[i].disposal != nullptr) {
          handed[i].disposer->dispose(handed[i].disposal);
        }
      }
    }
    return;
  }
  ContextRecord* record = recordOf(ctx);
  if (record == nullptr) {
    KJ_LOG(ERROR, "an owned list's context does not name a live record; nothing is released",
        kj::hex(ctx));
    return;
  }
  record->cross.lockExclusive()->tearingDown = true;
  // Each object is disposed of before the walk moves on to the next node, as a recursive
  // destructor would do it, without the recursion.
  OwnedNode** head = headOf(*record);
  while (OwnedNode* node = *head) {
    detachNode(node);
    KJ_DEFER(freeNode(node));
    node->disposer->dispose(node->disposal);
  }
}

// The IO_CONTEXT hostcall, for a context torn down inside a turn (args[1] = the context's
// handle). DRAIN asks for the owned list's objects, up to a frame's worth per call: the handler
// takes each node off the list, hands the turn the object and disposer the node recorded,
// retires the entry and frees the node; the turn disposes of the objects. TEARDOWN takes the
// record's queue apart: the handler hands the turn the signal's fulfiller (first call) and the
// queued actions, up to a frame's worth per call, to destroy, frees the chunks, and on the last
// call frees the record and retires the entry. Both validate the handle against the table and
// the companion's kind, read nothing through the frame but the two arguments, and take neither
// the process heap nor a lock but the guest kernel's own, the registry's and the record's.
long OwnedObjectList::hostcallHandler(hostcall::Frame& frame) {
  hostMemory::ResolveHandle ctx = frame.args[1];
  switch (frame.args[0]) {
    case DRAIN: {
      struct Handed {
        void* disposal;
        const kj::Disposer* disposer;
      };
      constexpr size_t BATCH = hostcall::BYTES / sizeof(Handed);
      ContextRecord* record = recordOf(ctx);
      if (record == nullptr) return -EINVAL;
      record->cross.lockExclusive()->tearingDown = true;
      OwnedNode** head = headOf(*record);
      Handed* out = reinterpret_cast<Handed*>(frame.buf);
      size_t n = 0;
      while (n < BATCH) {
        OwnedNode* node = *head;
        if (node == nullptr) break;
        detachNode(node);
        out[n++] = {node->disposal, node->disposer};
        freeNode(node);
      }
      frame.outLen = n * sizeof(Handed);
      return static_cast<long>(n);
    }
    case TEARDOWN: {
      // The result: [done, fulfiller, fulfiller's disposer, boxes...].
      constexpr size_t HEADER = 3;
      constexpr size_t BATCH = hostcall::BYTES / sizeof(uint64_t) - HEADER;
      kj::MutexGuarded<int>* registry = registryLock();
      if (registry == nullptr) return -EINVAL;
      auto guard = registry->lockExclusive();
      ContextRecord* record = recordOf(ctx);
      if (record == nullptr) return -EINVAL;
      uint64_t* out = reinterpret_cast<uint64_t*>(frame.buf);
      size_t n = 0;
      bool done;
      {
        auto lock = record->cross.lockExclusive();
        out[1] = out[2] = 0;
        if (lock->fulfiller.get() != nullptr) {
          const kj::Disposer* disposer = &lock->fulfiller.getDisposer();
          out[1] = reinterpret_cast<uintptr_t>(lock->fulfiller.disown(disposer));
          out[2] = reinterpret_cast<uintptr_t>(disposer);
        }
        ChunkQueue& actions = lock->actions;
        while (n < BATCH && actions.head != nullptr) {
          Chunk* chunk = actions.head;
          // Boxes are taken from the front of the first chunk; `count` shrinks as they go.
          uint32_t take = kj::min(chunk->count, static_cast<uint32_t>(BATCH - n));
          for (uint32_t i = 0; i < take; i++) out[HEADER + n++] = chunk->words[i];
          if (take < chunk->count) {
            memmove(chunk->words, chunk->words + take, (chunk->count - take) * sizeof(uint64_t));
            chunk->count -= take;
          } else {
            actions.head = chunk->next;
            if (actions.head == nullptr) actions.tail = nullptr;
            freeChunk(chunk);
          }
        }
        done = actions.head == nullptr;
        if (done) {
          consumeChain(lock->deletions.detach(), [](uint64_t) {});
          OwnedNode** head = headOf(*record);
          if (*head != nullptr) {
            KJ_LOG(ERROR, "a context's record was torn down with objects still in it; they leak",
                kj::hex(ctx));
          }
          while (OwnedNode* node = *head) {
            detachNode(node);
            freeNode(node);
          }
        }
      }
      out[0] = done ? 1 : 0;
      frame.outLen = (HEADER + n) * sizeof(uint64_t);
      if (done) {
        // The entry goes first, so that nothing finds the record again; then the record.
        hostMemory::retireResolve(ctx);
        freeRecord(record);
      }
      return static_cast<long>(n);
    }
  }
  return -EINVAL;
}

const hostcall::Entry OwnedObjectList::HOSTCALL = {
  hostcall::IO_CONTEXT, &OwnedObjectList::hostcallHandler};

void OwnedObjectList::installHostcall() {
  hostcall::install(HOSTCALL);
}

OwnedObjectList::Stats OwnedObjectList::statsForTest() {
  return {
    nodesAllocated.load(std::memory_order_relaxed),
    nodesFreed.load(std::memory_order_relaxed),
    recordsAllocated.load(std::memory_order_relaxed),
    recordsFreed.load(std::memory_order_relaxed),
    chunksAllocated.load(std::memory_order_relaxed),
    chunksFreed.load(std::memory_order_relaxed),
  };
}

OwnedObjectList::NodeForTest OwnedObjectList::nodeForTest(hostMemory::ResolveHandle handle) {
  OwnedNode* node = nodeOf(handle);
  if (node == nullptr) return {nullptr, nullptr, nullptr};
  return {node, reinterpret_cast<void**>(&node->prev), &node->disposer};
}

void* DeleteQueue::resolveOwned(hostMemory::ResolveHandle handle, const std::type_info& type) {
  void* object = hostMemory::resolve(handle);
  if (object == nullptr) {
    KJ_LOG(ERROR, "IoOwn handle does not name a live owned object; the use is refused",
        kj::hex(handle), type.name());
    KJ_FAIL_REQUIRE("IoOwn handle does not name a live owned object", kj::hex(handle), type.name());
  }
  return object;
}

void DeleteQueue::scheduleDeletion(
    hostMemory::ResolveHandle handle, const OwnedDisposal& disposal) const {
  if (isCurrentIoContext()) {
    // Deletion from the owning context on its own thread. No need to enqueue.
    kj::AllowAsyncDestructorsScope scope;
    OwnedObjectList::drop(handle, disposal);
    return;
  }
  if (hostMemory::inTurn()) {
    // Inside a turn, where the queue cannot be written: noted for the host, unless the context
    // has ended, in which case its list has already disposed of the object.
    if (hostMemory::resolve(context) == nullptr) return;
    uint64_t words[hostMemory::NOTE_WORDS] = {DELETION, context, handle, 0};
    if (!hostMemory::noteInTurn(words)) {
      KJ_LOG(ERROR, "a dropped IoOwn could not be noted; its object is released with its context",
          kj::hex(handle));
    }
    return;
  }
  queueDeletionHost(context, handle);
}

void DeleteQueue::scheduleAction(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& action) const {
  if (tryScheduleAction(kj::mv(action))) {
    return;
  }

  // The context that owns the queue has ended. We are going to emit a warning and drop the
  // action on the floor without scheduling it.
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    // We are creating an error here just so we can include the JavaScript stack
    // with the warning if it exists. We are not going to throw this error.
    auto err = v8::Exception::Error(
        js.str("A promise was resolved or rejected from a different request context than "
               "the one it was created in. However, the creating request has already been "
               "completed or canceled. Continuations for that request are unlikely to "
               "run safely and have been canceled. If this behavior breaks your worker, "
               "consider setting the `no_handle_cross_request_promise_resolution` "
               "compatibility flag for your worker."_kj))
                   .As<v8::Object>();
    // TODO(soon): Add documentation link to this warning.
    // Changing the name property to "Warning" will make the serialize stack start with
    // "Warning: " rather that "Error: "
    jsg::check(err->Set(js.v8Context(), js.str("name"_kj), js.str("Warning"_kj)));
    auto stack = jsg::check(err->Get(js.v8Context(), js.str("stack"_kj)));

    ioContext.logWarning(kj::str(stack));
  }
}

bool DeleteQueue::tryScheduleAction(kj::Function<void(jsg::Lock&)>&& action) const {
  if (hostMemory::inTurn()) {
    // Inside a turn: the box is noted for the host to queue after the turn, if the context is
    // still live. A context that ends before the turn does drops the action then, as it would
    // have dropped one queued directly.
    if (hostMemory::resolve(context) == nullptr) return false;
    auto* box = new Action(kj::mv(action));
    uint64_t words[hostMemory::NOTE_WORDS] = {ACTION, context, reinterpret_cast<uintptr_t>(box), 0};
    if (hostMemory::noteInTurn(words)) return true;
    delete box;
    return false;
  }
  auto* box = new Action(kj::mv(action));
  if (queueActionHost(context, box)) return true;
  delete box;
  return false;
}

bool DeleteQueue::isCurrentIoContext() const {
  return context != 0 && IoContext::hasCurrent() &&
      IoContext::current().deleteQueue.queue.context == context;
}

bool DeleteQueue::isDefunct() const {
  return hostMemory::resolve(context) == nullptr;
}

void DeleteQueue::checkFarGet(const DeleteQueue& deleteQueue, const std::type_info& type) {
  IoContext::current().checkFarGet(deleteQueue, type);
}

DeleteQueue::RecordForTest DeleteQueue::recordForTest(hostMemory::ResolveHandle context) {
  ContextRecord* record = recordOf(context);
  if (record == nullptr) return {nullptr, 0, nullptr};
  // The fulfiller's pointer is the second word of its kj::Own, after the disposer.
  auto lock = record->cross.lockExclusive();
  void** fulfiller = reinterpret_cast<void**>(&lock->fulfiller) + 1;
  KJ_ASSERT(*fulfiller == lock->fulfiller.get());
  return {record, sizeof(ContextRecord), fulfiller};
}

void ReverseIoOwnValidity::checkValid() const {
  JSG_REQUIRE(
      isValid(), Error, "Couldn't complete operation because the execution context has ended.");
}

DeleteQueueOwner::DeleteQueueOwner() {
  // The entry's pointer is this owner, in the context, which nothing dereferences through the
  // table; what the entry is for is its companion, the record in host memory. Registered inside
  // a turn, the record is built by the committed hook after the turn; the holders the turn
  // creates meanwhile need only the handle.
  hostMemory::ResolveHandle handle;
  if (hostMemory::inTurn()) {
    uint64_t aux[hostMemory::RESOLVE_AUX_WORDS] = {CONTEXT, 0, 0, 0};
    handle = hostMemory::registerResolve(this, aux);
  } else {
    handle = hostMemory::registerResolve(this);
    if (handle != 0 && !hostMemory::setResolveCompanion(handle, {CONTEXT, allocateRecord()})) {
      hostMemory::unregisterResolve(handle);
      handle = 0;
    }
  }
  KJ_REQUIRE(handle != 0, "the resolve table is full; cannot create another I/O context",
      hostMemory::RESOLVE_CAPACITY);
  queue = DeleteQueue(handle);
}

DeleteQueueOwner::~DeleteQueueOwner() noexcept(false) {
  hostMemory::ResolveHandle ctx = queue.getContext();
  if (ctx == 0) return;
  queue = nullptr;
  if (hostMemory::inTurn()) {
    // Registered in this turn: never committed, so there is no record; the entry is released
    // from the journal.
    bool journaled = false;
    hostMemory::releaseJournaled(
        [ctx, &journaled](hostMemory::ResolveHandle handle, void*, const uint64_t*) {
      if (handle != ctx) return false;
      journaled = true;
      return true;
    });
    if (journaled) return;
    // Committed before: the record is taken apart through the hostcall, which hands over the
    // fulfiller and the actions for this turn to destroy, as the context would have destroyed
    // them itself, and retires the entry.
    constexpr size_t HEADER = 3;
    bool first = true;
    for (;;) {
      hostcall::Frame frame;
      frame.args[0] = TEARDOWN;
      frame.args[1] = ctx;
      frame.args[2] = frame.args[3] = 0;
      frame.inLen = 0;
      long n = hostcall::call(OwnedObjectList::HOSTCALL, frame);
      if (n < 0) {
        KJ_LOG(ERROR, "a context's record could not be torn down inside the turn", kj::hex(ctx), n);
        break;
      }
      size_t count = static_cast<size_t>(n);
      if (frame.outLen < (HEADER + count) * sizeof(uint64_t)) break;
      uint64_t words[hostcall::BYTES / sizeof(uint64_t)];
      memcpy(words, frame.buf, (HEADER + count) * sizeof(uint64_t));
      if (first) {
        first = false;
        auto* fulfiller = reinterpret_cast<kj::CrossThreadPromiseFulfiller<void>*>(
            static_cast<uintptr_t>(words[1]));
        auto* disposer = reinterpret_cast<const kj::Disposer*>(static_cast<uintptr_t>(words[2]));
        if (fulfiller != nullptr && disposer != nullptr) {
          // The fulfiller is an async object; destroying it here is what the context's own
          // destructor does.
          kj::AllowAsyncDestructorsScope scope;
          kj::Own<kj::CrossThreadPromiseFulfiller<void>> own(fulfiller, *disposer);
        }
      }
      for (size_t i = 0; i < count; i++) {
        delete reinterpret_cast<Action*>(static_cast<uintptr_t>(words[HEADER + i]));
      }
      if (words[0] != 0) break;
    }
    return;
  }
  kj::MutexGuarded<int>* registry = registryLock();
  if (registry == nullptr) {
    KJ_LOG(ERROR, "the delete queue registry does not resolve; the record leaks", kj::hex(ctx));
    hostMemory::unregisterResolve(ctx);
    return;
  }
  TakenQueue taken;
  ContextRecord* record;
  {
    // Exclusive: no other thread is between finding the record and locking it once this is
    // held, so the record can go once the companion no longer names it.
    auto guard = registry->lockExclusive();
    record = recordOf(ctx);
    if (record != nullptr) {
      auto lock = record->cross.lockExclusive();
      taken = takeQueue(*lock);
      OwnedNode** head = headOf(*record);
      if (*head != nullptr) {
        KJ_LOG(ERROR, "a context's record was torn down with objects still in it; they leak",
            kj::hex(ctx));
      }
      while (OwnedNode* node = *head) {
        detachNode(node);
        freeNode(node);
      }
    }
    hostMemory::unregisterResolve(ctx);
  }
  if (record != nullptr) freeRecord(record);
  if (taken.fulfiller != nullptr) {
    // The delete queue state may include a kj::CrossThreadPromiseFulfiller that needs to be
    // destroyed. To do so, we need to allow async destructors here. We only want to destroy
    // the fulfiller in this scope though, not everything that was in the queue.
    kj::AllowAsyncDestructorsScope scope;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> own(taken.fulfiller, *taken.fulfillerDisposer);
  }
  consumeChain(taken.actions,
      [](uint64_t word) { delete reinterpret_cast<Action*>(static_cast<uintptr_t>(word)); });
}

void DeleteQueueOwner::drainDeletions() {
  ContextRecord* record = recordOf(queue.getContext());
  if (record == nullptr) return;
  Chunk* chain;
  {
    auto lock = record->cross.lockExclusive();
    if (lock->deletions.empty()) return;
    chain = lock->deletions.detach();
  }
  // Released with no lock held: disposing of an object may queue more, for this context or
  // another.
  consumeChain(
      chain, [](uint64_t handle) { OwnedObjectList::release(handle, DestroyOwnedObject::YES); });
}

kj::Vector<kj::Own<Action>> DeleteQueueOwner::takeActions() {
  kj::Vector<kj::Own<Action>> actions;
  ContextRecord* record = recordOf(queue.getContext());
  if (record == nullptr) return actions;
  Chunk* chain;
  {
    auto lock = record->cross.lockExclusive();
    chain = lock->actions.detach();
  }
  consumeChain(chain, [&](uint64_t word) {
    actions.add(kj::Own<Action>(reinterpret_cast<Action*>(static_cast<uintptr_t>(word)),
        kj::_::HeapDisposer<Action>::instance));
  });
  return actions;
}

kj::Promise<void> DeleteQueueOwner::resetCrossThreadSignal() {
  ContextRecord* record = recordOf(queue.getContext());
  if (record == nullptr) return kj::NEVER_DONE;
  auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
  {
    auto lock = record->cross.lockExclusive();
    if (lock->fulfiller.get() != nullptr) {
      // We should only reset the signal if it has been fulfilled.
      KJ_ASSERT(!lock->fulfiller->isWaiting());
    }
    lock->fulfiller = kj::mv(paf.fulfiller);
    // Actions queued while there was no signal to wake, or since the last one woke, are
    // waiting already.
    if (!lock->actions.empty()) lock->fulfiller->fulfill();
  }
  return kj::mv(paf.promise);
}

void IoCrossContextExecutor::execute(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& func) {
  deleteQueue.scheduleAction(js, kj::mv(func));
}

bool IoCrossContextExecutor::isCurrent() const {
  return deleteQueue.isCurrentIoContext();
}

bool IoCrossContextExecutor::tryExecute(kj::Function<void(jsg::Lock&)>&& func) const {
  return deleteQueue.tryScheduleAction(kj::mv(func));
}

bool IoCrossContextExecutor::isTargetDestroyed() const {
  return deleteQueue.isDefunct();
}

}  // namespace workerd
