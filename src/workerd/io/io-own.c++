#include "io-own.h"

#include "io-context.h"

#include <workerd/jsg/util.h>

#include <atomic>
#include <cerrno>
#include <cstring>

namespace workerd {

namespace {

// What an entry's companion holds (hostMemory::ResolveCompanion::kind): the head word of an
// owned list, or an owned node.
enum CompanionKind : uint64_t { NONE = 0, LIST = 1, NODE = 2 };

// A node of an owned list, in host memory (see OwnedObjectList).
struct OwnedNode {
  OwnedNode* next;
  OwnedNode** prev;  // the previous node's `next`, or the list's head word
  void* disposal;    // the object, as its disposer takes it
  const kj::Disposer* disposer;
  hostMemory::ResolveHandle handle;  // the entry naming the object
};

// The words a node's registration carries through the turn's journal (see OwnedObjectList::add).
enum AuxWord : size_t { AUX_KIND = 0, AUX_DISPOSER = 1, AUX_LIST = 2, AUX_DISPOSAL = 3 };
static_assert(hostMemory::RESOLVE_AUX_WORDS == 4);

std::atomic<size_t> nodesAllocated{0};
std::atomic<size_t> nodesFreed{0};

// WORKERD_GK_NEG_IOOWN_NODE is a negative-test build, never shipped: the nodes are placed on
// the ordinary heap, where a turn can write them, which is the state before the owned list
// moved to host memory. The guest kernel's ioown-test.sh must then fail, with the host's walk
// following the links and calling the disposer a turn wrote into a node; this is the check that
// the test tests the placement.
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

OwnedNode** headOf(hostMemory::ResolveCompanion& list) {
  return reinterpret_cast<OwnedNode**>(&list.ptr);
}

void linkNode(OwnedNode* node, hostMemory::ResolveCompanion& list) {
  OwnedNode** head = headOf(list);
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

// The drain operation of the OWNED_LIST hostcall.
constexpr uint64_t DRAIN = 1;

// Takes the node out of its list and retires its entry; the object is untouched, for the
// caller to dispose of before it frees the node. Host-side.
void detachNode(OwnedNode* node) {
  unlinkNode(node);
  hostMemory::retireResolve(node->handle);
  node->handle = 0;
}

}  // namespace

hostMemory::ResolveHandle OwnedObjectList::ensureRegistered() {
  if (handle != 0) return handle;
  // The entry's pointer is this list, in the context, which nothing dereferences through the
  // table; what the entry is for is its companion, the head word of the list in host memory.
  // Registered inside a turn, the companion is set by the committed hook after the turn, and
  // the nodes the turn adds name the list by this handle in the meantime.
  if (hostMemory::inTurn()) {
    uint64_t aux[hostMemory::RESOLVE_AUX_WORDS] = {LIST, 0, 0, 0};
    handle = hostMemory::registerResolve(this, aux);
  } else {
    handle = hostMemory::registerResolve(this);
    if (handle != 0 && !hostMemory::setResolveCompanion(handle, {LIST, nullptr})) {
      hostMemory::unregisterResolve(handle);
      handle = 0;
    }
  }
  KJ_REQUIRE(handle != 0, "the resolve table is full; cannot create another I/O object",
      hostMemory::RESOLVE_CAPACITY);
  return handle;
}

hostMemory::ResolveHandle OwnedObjectList::add(
    void* object, void* disposal, const kj::Disposer& disposer) {
  hostMemory::ResolveHandle list = ensureRegistered();
  uint64_t aux[hostMemory::RESOLVE_AUX_WORDS] = {
    NODE, reinterpret_cast<uintptr_t>(&disposer), list, reinterpret_cast<uintptr_t>(disposal)};
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
    case LIST:
      return hostMemory::setResolveCompanion(handle, {LIST, nullptr});
    case NODE: {
      auto* disposer = reinterpret_cast<const kj::Disposer*>(aux[AUX_DISPOSER]);
      auto* disposal = reinterpret_cast<void*>(aux[AUX_DISPOSAL]);
      if (disposer == nullptr || disposal == nullptr) {
        KJ_LOG(ERROR, "an owned object's registration names no disposer; it is refused",
            kj::hex(handle));
        return false;
      }
      KJ_IF_SOME(list, hostMemory::resolveCompanion(aux[AUX_LIST])) {
        if (list.kind != LIST) {
          KJ_LOG(ERROR,
              "an owned object's registration names a list that is not one; it is "
              "refused",
              kj::hex(handle), kj::hex(aux[AUX_LIST]));
          return false;
        }
        OwnedNode* node = allocateNode();
        node->disposal = disposal;
        node->disposer = disposer;
        node->handle = handle;
        linkNode(node, list);
        if (!hostMemory::setResolveCompanion(handle, {NODE, node})) {
          unlinkNode(node);
          freeNode(node);
          return false;
        }
        return true;
      }
      KJ_LOG(ERROR, "an owned object's registration names no live list; it is refused",
          kj::hex(handle), kj::hex(aux[AUX_LIST]));
      return false;
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
      case LIST: {
        // A list is drained before it is released (see ~OwnedObjectList); an object still in it
        // is one the turn did not dispose of, and it is not disposed of here either, which may
        // run inside a hostcall: the node is freed and the object leaks.
        OwnedNode** head = headOf(c);
        if (*head != nullptr) {
          KJ_LOG(ERROR, "an owned list was released with objects still in it; they leak",
              kj::hex(handle));
        }
        while (OwnedNode* node = *head) {
          detachNode(node);
          freeNode(node);
        }
        return;
      }
    }
  }
}

const hostMemory::ResolveTurnHooks OwnedObjectList::TURN_HOOKS = {
  &OwnedObjectList::committed, &OwnedObjectList::released};

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

void DeleteQueue::scheduleDeletion(
    hostMemory::ResolveHandle handle, const OwnedDisposal& disposal) const {
  if (isCurrentIoContext()) {
    // Deletion from the owning context on its own thread. No need to enqueue.
    kj::AllowAsyncDestructorsScope scope;
    OwnedObjectList::drop(handle, disposal);
  } else {
    auto lock = crossThreadDeleteQueue.lockExclusive();
    KJ_IF_SOME(state, *lock) {
      state.queue.add(handle);
    }
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
  if (handle == 0) return;
  hostMemory::ResolveHandle list = handle;
  handle = 0;
  if (hostMemory::inTurn()) {
    // Torn down inside a turn, where the nodes cannot be reached. What this turn registered
    // into the list is still in the journal, with the disposer, and is disposed of from there;
    // what was committed before is drained through the hostcall, which hands the objects over
    // with their disposers and frees their nodes. Each object is disposed of here, in the turn,
    // as it would be host-side. The list's own entry is released last; its hook frees the head.
    //
    // An object this turn has already released is disposed of, and its node still linked until
    // the release reaches the host; the retire list is flushed first so that the drain does not
    // hand such an object over again.
    hostMemory::flushReleasedInTurn();
    hostMemory::releaseJournaled([list](hostMemory::ResolveHandle, void*, const uint64_t* aux) {
      if (aux[AUX_KIND] != NODE || aux[AUX_LIST] != list) return false;
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
      frame.args[1] = list;
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
    hostMemory::unregisterResolve(list);
    return;
  }
  KJ_IF_SOME(c, hostMemory::resolveCompanion(list)) {
    if (c.kind == LIST) {
      // Each object is disposed of before the walk moves on to the next node, as a recursive
      // destructor would do it, without the recursion.
      OwnedNode** head = headOf(c);
      while (OwnedNode* node = *head) {
        detachNode(node);
        KJ_DEFER(freeNode(node));
        node->disposer->dispose(node->disposal);
      }
    } else {
      KJ_LOG(
          ERROR, "an owned list's handle does not name a list; nothing is released", kj::hex(list));
    }
  } else {
    KJ_LOG(ERROR, "an owned list's handle does not name a live entry; nothing is released",
        kj::hex(list));
  }
  hostMemory::unregisterResolve(list);
}

// The OWNED_LIST hostcall: a turn tearing a list down asks for its objects, up to a frame's worth
// per call (args[0] = DRAIN, args[1] = the list's handle). The handler takes each node off the list,
// hands the turn the object and disposer the node recorded, retires the entry and frees the
// node; the turn disposes of the objects. It validates the handle against the table and the
// companion's kind, reads nothing through the frame but the two arguments, and takes neither
// the process heap nor a lock but the guest kernel's own.
long OwnedObjectList::hostcallHandler(hostcall::Frame& frame) {
  if (frame.args[0] != DRAIN) return -EINVAL;
  struct Handed {
    void* disposal;
    const kj::Disposer* disposer;
  };
  constexpr size_t BATCH = hostcall::BYTES / sizeof(Handed);
  KJ_IF_SOME(c, hostMemory::resolveCompanion(frame.args[1])) {
    if (c.kind != LIST) return -EINVAL;
    OwnedNode** head = headOf(c);
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
  return -EINVAL;
}

const hostcall::Entry OwnedObjectList::HOSTCALL = {
  hostcall::OWNED_LIST, &OwnedObjectList::hostcallHandler};

void OwnedObjectList::installHostcall() {
  hostcall::install(HOSTCALL);
}

OwnedObjectList::Stats OwnedObjectList::statsForTest() {
  return {
    nodesAllocated.load(std::memory_order_relaxed), nodesFreed.load(std::memory_order_relaxed)};
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

void DeleteQueue::scheduleAction(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& action) const {
  if (tryScheduleAction(kj::mv(action))) {
    return;
  }

  // The queue was deleted, likely because the IoContext was destroyed and the
  // DeleteQueuePtr was invalidated. We are going to emit a warning and drop the
  // actions on the floor without scheduling them.
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

    // Safe to mutate here since we have the exclusive lock on the queue above.
    ioContext.logWarning(kj::str(stack));
  }
}

bool DeleteQueue::tryScheduleAction(kj::Function<void(jsg::Lock&)>&& action) const {
  auto lock = crossThreadDeleteQueue.lockExclusive();
  KJ_IF_SOME(state, *lock) {
    state.actions.add(kj::mv(action));
    KJ_REQUIRE_NONNULL(state.crossThreadFulfiller)->fulfill();
    return true;
  }
  // The queue was deleted, likely because the IoContext was destroyed and the DeleteQueuePtr
  // was invalidated. The action is dropped on the floor.
  return false;
}

bool DeleteQueue::isCurrentIoContext() const {
  return IoContext::hasCurrent() && IoContext::current().deleteQueue.queue.get() == this;
}

bool DeleteQueue::isDefunct() const {
  return *crossThreadDeleteQueue.lockShared() == kj::none;
}

void DeleteQueue::checkFarGet(const DeleteQueue& deleteQueue, const std::type_info& type) {
  IoContext::current().checkFarGet(deleteQueue, type);
}

void ReverseIoOwnValidity::checkValid() const {
  JSG_REQUIRE(
      isValid(), Error, "Couldn't complete operation because the execution context has ended.");
}

kj::Promise<void> DeleteQueue::resetCrossThreadSignal() const {
  auto lock = crossThreadDeleteQueue.lockExclusive();
  KJ_IF_SOME(state, *lock) {
    KJ_IF_SOME(fulfiller, state.crossThreadFulfiller) {
      // We should only reset the signal if it has been fulfilled.
      KJ_ASSERT(!fulfiller->isWaiting());
    }
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    state.crossThreadFulfiller = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  } else {
    return kj::NEVER_DONE;
  }
}

void IoCrossContextExecutor::execute(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& func) {
  deleteQueue->scheduleAction(js, kj::mv(func));
}

bool IoCrossContextExecutor::isCurrent() const {
  return deleteQueue->isCurrentIoContext();
}

bool IoCrossContextExecutor::tryExecute(kj::Function<void(jsg::Lock&)>&& func) const {
  return deleteQueue->tryScheduleAction(kj::mv(func));
}

bool IoCrossContextExecutor::isTargetDestroyed() const {
  return deleteQueue->isDefunct();
}

}  // namespace workerd
