#pragma once

#include <workerd/util/host-memory.h>
#include <workerd/util/hostcall.h>
#include <workerd/util/strong-bool.h>

#include <kj/async.h>
#include <kj/common.h>
#include <kj/function.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <typeinfo>

namespace workerd {
namespace jsg {
class Lock;
}

class IoContext;

// Tracks whether ReverseIoOwn instances can access their objects.
class ReverseIoOwnValidity final: public kj::AtomicRefcounted {
 public:
  bool isValid() const {
    return valid;
  }

  void checkValid() const;

 private:
  void invalidate() const {
    valid = false;
  }

  // ReverseIoOwn instances may outlive their IoContext, but only on its thread.
  mutable bool valid = true;

  friend class IoContext;
};

template <typename T>
class IoOwn;
template <typename T>
class IoPtr;
template <typename T>
class ReverseIoOwn;

template <typename T>
struct RemoveIoOwn_ {
  using Type = T;
  static constexpr bool is = false;
};
template <typename T>
struct RemoveIoOwn_<IoOwn<T>> {
  using Type = T;
  static constexpr bool is = true;
};

template <typename T>
constexpr bool isIoOwn() {
  return RemoveIoOwn_<T>::is;
}
template <typename T>
using RemoveIoOwn = RemoveIoOwn_<T>::Type;

WD_STRONG_BOOL(DestroyOwnedObject);

// How a holder disposes of its object inside a turn, where the context's record of the object
// (OwnedObjectList) cannot be reached: the disposer recorded when the object was registered,
// and the call to it for the object's type, bound at registration, where the type is complete,
// so that a holder of an incomplete type can be destroyed.
struct OwnedDisposal {
  const kj::Disposer* disposer;
  void (*dispose)(void* object, const kj::Disposer& disposer);
};
template <typename T>
void disposeOwnedAs(void* object, const kj::Disposer& disposer) {
  disposer.dispose(static_cast<T*>(object));
}

// The objects a context owns on behalf of the IoOwns and ReverseIoOwns that name them, each
// registered in the ring-0 resolve table (hostMemory::registerResolve) under a handle its holder
// keeps. The table's entry holds the object pointer, which a turn resolves on every dereference;
// what the host keeps about the object beyond that -- its place in this list, and how to dispose
// of it -- is a node in host memory, reached from the entry's companion (hostMemory::
// resolveCompanion), so that nothing the host follows when it walks the list or disposes of an
// object is in memory a turn can write. The list's own head is the companion of an entry of its
// own, registered when the first object is added; a node names its list by that handle, and a
// handle is the only thing about the list that lives in the context, in guest memory.
//
// Objects are added inside turns, where host memory cannot be written: the registration is
// journaled by the turn with the disposer and the list's handle as its words
// (hostMemory::RESOLVE_AUX_WORDS), and the host builds and links the node after the turn, from
// its committed hook (TURN_HOOKS); the holder uses its handle in the meantime, resolved through
// the journal. An object released inside a turn by its holder is disposed of by the turn, through
// the disposer the holder recorded beside its handle, and the entry's release is journaled, so the
// host unlinks and frees the node afterwards without touching the object. Released host-side --
// a holder dropped on the event loop, the delete-queue drain, the context's teardown -- the host
// disposes of the object through the node's own copy of the disposer. A list torn down inside a
// turn (a context destroyed by another context's turn) releases what the turn itself registered
// from the journal, and drains the rest through the OWNED_LIST hostcall, which hands the turn
// each object with its disposer and frees the nodes host-side.
//
// What this protects is the node: the links the host's walk follows and the disposer it calls
// are in host memory and were written by the host. What it does not protect is the object: the
// T is in the shared heap, placed by ordinary runtime code, so the host's disposal of it runs a
// destructor over memory a turn can write; and the disposer the node records is the one the
// turn journaled when it registered the object, which the commit admits unvalidated -- a turn
// that lies about the disposer of its own object sends the host's disposal of that object to a
// function of its choosing, the same exposure the object's own vtable and members carry. A
// turn's words can also name another context's live list, which links the turn's own object
// into that context's list (the ownership residual the table cannot close).
class OwnedObjectList {
 public:
  OwnedObjectList() = default;
  KJ_DISALLOW_COPY_AND_MOVE(OwnedObjectList);
  ~OwnedObjectList() noexcept(false);

  // Registers `object` as owned by this list and returns the handle naming it, or throws if the
  // table is full. `object` is the pointer the holder resolves (a T*); `disposal` the pointer
  // its disposer takes (kj::castToVoid of it).
  hostMemory::ResolveHandle add(void* object, void* disposal, const kj::Disposer& disposer);

  // Host-side: releases the object `handle` names from its list, disposing of it when asked,
  // and retires the entry. Reports and does nothing if the handle does not name an owned
  // object.
  static void release(hostMemory::ResolveHandle handle, DestroyOwnedObject destroy);

  // Host-side: takes the object `handle` names out of its list without disposing of it, retires
  // the entry, and returns the disposer the object is to be disposed of with. Throws if the
  // handle does not name an owned object.
  static const kj::Disposer& take(hostMemory::ResolveHandle handle);

  // Inside a turn: releases the entry `handle` names, which the turn is disposing of itself.
  static void releaseInTurn(hostMemory::ResolveHandle handle);

  // Drops the object `handle` names on the thread of the context that owns it: inside a turn,
  // through `disposal` and releaseInTurn(); outside one, host-side through the list.
  static void drop(hostMemory::ResolveHandle handle, const OwnedDisposal& disposal);

  // The hooks a ResolveTurn runs host-side for the entries a turn registered and released.
  static const hostMemory::ResolveTurnHooks TURN_HOOKS;

  // Registers the OWNED_LIST hostcall. Called once, host-side, before any turn runs.
  static void installHostcall();

  // Nodes allocated and freed since the process started, for tests and diagnostics.
  struct Stats {
    size_t nodesAllocated;
    size_t nodesFreed;
  };
  static Stats statsForTest();

  // The node behind `handle`, for a test that corrupts it from inside a turn: the node's
  // address, and its link and disposer fields; null if the handle names no owned object.
  struct NodeForTest {
    void* node;
    void** prev;
    const kj::Disposer** disposer;
  };
  static NodeForTest nodeForTest(hostMemory::ResolveHandle handle);

 private:
  hostMemory::ResolveHandle handle = 0;  // the list's entry, 0 until the first object is added

  hostMemory::ResolveHandle ensureRegistered();

  static bool committed(hostMemory::ResolveHandle handle, void* ptr, const uint64_t* aux);
  static void released(hostMemory::ResolveHandle handle);
  static long hostcallHandler(hostcall::Frame& frame);
  static const hostcall::Entry HOSTCALL;
};

// Object which receives possibly-cross-thread deletions of owned objects.
class DeleteQueue: public kj::AtomicRefcounted {
 public:
  DeleteQueue(): crossThreadDeleteQueue(State{kj::Vector<hostMemory::ResolveHandle>()}) {}

  // Drops the object the handle names, which the holder of `handle` and `disposal` is giving
  // up: at once if the owning IoContext is current on this thread -- inside a turn, by the
  // holder's own disposal; otherwise host-side through the owned list -- and otherwise when
  // that context next runs. A handle that names nothing (never normal: a holder keeps a live
  // entry for its lifetime) is reported and deletes nothing.
  void scheduleDeletion(hostMemory::ResolveHandle handle, const OwnedDisposal& disposal) const;

  // Schedules the given action to run in the IoContext that owns this queue, the next time
  // that context drains its queue (in its own thread, under the isolate lock). If the owning
  // IoContext has already been destroyed, the action is dropped and a warning about
  // cross-request promise resolution is logged to the current context, if any.
  void scheduleAction(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& action) const;

  // Like scheduleAction(), but for callers where a destroyed target context is an expected,
  // benign outcome: returns true if the action was queued, or false — dropping the action
  // silently — if the owning IoContext has already been destroyed.
  bool tryScheduleAction(kj::Function<void(jsg::Lock&)>&& action) const;

  // True if the IoContext that owns this queue is the calling thread's current IoContext.
  bool isCurrentIoContext() const;

  // True if the IoContext that owns this queue has been destroyed, meaning scheduled actions
  // and deletions are dropped. Useful for reclaiming registrations that target this queue.
  bool isDefunct() const;

  struct State {
    // Handles of objects whose holders were dropped in other threads, released host-side when
    // the owning IoContext drains the queue (see OwnedObjectList::release).
    kj::Vector<hostMemory::ResolveHandle> queue;
    // Actions that some other IoContext has requested be executed in this IoContext. When
    // adding an action to this list, crossThreadFulfiller should be fulfilled, signaling the
    // target IoContext to wake up and run actions. After draining the actions queue, the target
    // IoContext should replace crossThreadFulfiller with a new one which will wake it up again.
    //
    // In particular, these actions are used to implement cross-context promise resolution.
    //
    // Keep in mind the IoContext could be destroyed before the cross-thread signal runs, in
    // which case the actions will never run.
    kj::Vector<kj::Function<void(jsg::Lock&)>> actions;
    kj::Maybe<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> crossThreadFulfiller;
  };

  // Handles from IoOwns that were dropped in other threads, and therefore should be deleted
  // whenever the IoContext gets around to it. The maybe is changed to kj::none when the
  // IoContext goes away, at which point all owned objects have already been deleted so
  // cross-thread deletions can just be ignored.
  kj::MutexGuarded<kj::Maybe<State>> crossThreadDeleteQueue;

  // Implements the corresponding methods of IoContext and ActorContext.
  template <typename T>
  IoOwn<T> addObject(kj::Own<T> obj, OwnedObjectList& ownedObjects) const;

  template <typename T>
  ReverseIoOwn<T> addObjectReverse(
      kj::Arc<ReverseIoOwnValidity> validity, kj::Own<T> obj, OwnedObjectList& ownedObjects) const;

  static void checkFarGet(const DeleteQueue& deleteQueue, const std::type_info& type);

  // The object a live holder's handle names, resolved against the table (hostMemory::resolve)
  // for this one use. Throws if the handle names nothing: the holder registered a live entry
  // and keeps it until it releases it, so a handle that fails to resolve was rewritten, or
  // names an object already released, and is reported as such.
  static void* resolveOwned(hostMemory::ResolveHandle handle, const std::type_info& type);

  // Adds the object to the owned list and returns the handle naming it with the disposer the
  // holder records; on a full table, disposes of the object and throws.
  struct OwnedRegistration {
    hostMemory::ResolveHandle handle;
    OwnedDisposal disposal;
  };
  template <typename T>
  static OwnedRegistration registerOwned(kj::Own<T> obj, OwnedObjectList& ownedObjects);

 private:
  kj::Promise<void> resetCrossThreadSignal() const;

  friend class IoContext;
};

// Object which can push actions into a specific DeleteQueue then signal its
// owning IoContext to wake up to process the queue. This is a bit of a hack of
// the DeleteQueue concept that allows us to use the same queue for more than
// just deletions.
class IoCrossContextExecutor {
 public:
  IoCrossContextExecutor(kj::Arc<DeleteQueue> deleteQueue): deleteQueue(kj::mv(deleteQueue)) {}

  // Tries to execute the specified action to the owning IoContext.
  // The target IoContext will be signaled to run the action as soon as it is able.
  void execute(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& action);

  // True if the IoContext this executor targets is the calling thread's current IoContext —
  // i.e. the caller could run the work synchronously instead of deferring through
  // tryExecute().
  bool isCurrent() const;

  // Like execute(), but for callers where a destroyed target context is an expected, benign
  // outcome: returns true if the action was queued, or false — dropping the action silently —
  // if the target IoContext has already been destroyed.
  bool tryExecute(kj::Function<void(jsg::Lock&)>&& action) const;

  // True if the target IoContext has been destroyed, i.e. tryExecute() would drop the action.
  bool isTargetDestroyed() const;

 private:
  friend class IoContext;
  friend class DeleteQueue;

  kj::Arc<DeleteQueue> deleteQueue;
};

template <typename T>
inline DeleteQueue::OwnedRegistration DeleteQueue::registerOwned(
    kj::Own<T> obj, OwnedObjectList& ownedObjects) {
  const kj::Disposer& disposer = obj.getDisposer();
  // The disposer is told the object's address as it was allocated (kj::castToVoid, which is
  // what kj::Own hands it), which for a polymorphic T need not be the T* the holder resolves.
  T* object = obj.get();
  // Throws if the table is full, with `obj` still owning the object.
  hostMemory::ResolveHandle handle = ownedObjects.add(object, kj::_::castToVoid(object), disposer);
  obj.disown(&disposer);
  return {handle, {&disposer, &disposeOwnedAs<T>}};
}

template <typename T>
inline IoOwn<T> DeleteQueue::addObject(kj::Own<T> obj, OwnedObjectList& ownedObjects) const {
  auto registration = registerOwned(kj::mv(obj), ownedObjects);
  return IoOwn<T>(addRefToThis(), registration.handle, registration.disposal);
}

template <typename T>
inline ReverseIoOwn<T> DeleteQueue::addObjectReverse(
    kj::Arc<ReverseIoOwnValidity> validity, kj::Own<T> obj, OwnedObjectList& ownedObjects) const {
  auto registration = registerOwned(kj::mv(obj), ownedObjects);
  return ReverseIoOwn<T>(kj::mv(validity), registration.handle, registration.disposal);
}

// When the IoContext is destroyed, we need to null out the DeleteQueue. Complicating
// matters a bit, we need to cancel all tasks (destroy the TaskSet) before this happens, so
// we can't just do it in IoContext's destructor. As a hack, we customize our pointer
// to the delete queue to get the tear-down order right.
class DeleteQueuePtr {
 public:
  DeleteQueuePtr(kj::Arc<DeleteQueue> queue): queue(kj::mv(queue)) {}
  KJ_DISALLOW_COPY_AND_MOVE(DeleteQueuePtr);
  ~DeleteQueuePtr() noexcept(false) {
    auto ptr = queue.get();
    if (ptr != nullptr) {
      auto lock = ptr->crossThreadDeleteQueue.lockExclusive();
      KJ_IF_SOME(state, *lock) {
        // The delete queue state may include a kj::CrossThreadPromiseFulfiller that
        // needs to be destroyed. To do so, we need to allow async destructors here.
        // We only want to destroy the crossThreadFulfiller in this scope tho, not
        // everything that may be in the queue.
        kj::AllowAsyncDestructorsScope scope;
        state.crossThreadFulfiller = kj::none;
      }
      *lock = kj::none;
    }
  }
  kj::Arc<DeleteQueue> queue;
};

// Owned reference held by a V8 heap object, naming a KJ event loop object. Cannot be
// dereferenced unless the isolate is executing on the appropriate event loop thread.
//
// The object is named by a resolve handle (hostMemory::ResolveHandle) rather than by a pointer:
// the IoOwn sits in memory a JS turn can write, and under guest-kernel isolation a turn runs at
// guest ring 3, so a pointer here would be one the host dereferences after the guest could have
// rewritten it. The handle is resolved against a table the guest cannot write on every use --
// in a turn through a trap into ring 0 inside the guest, on the host directly -- and a handle
// that no longer names a live entry fails the use rather than reaching whatever it was changed
// to. Beside the handle the IoOwn records how to dispose of the object (OwnedDisposal), which
// only the turn uses: an IoOwn dropped inside a turn disposes of its object there, since the
// context's record of the object (OwnedObjectList) is in host memory, which the turn cannot
// reach; the host never reads this copy, and disposes of the object through the record's own.
// The object itself is still in the shared heap, and a handle rewritten to another live entry's
// names that entry's object.
template <typename T>
class IoOwn {

 public:
  IoOwn(IoOwn&& other) noexcept;
  IoOwn(decltype(nullptr)): handle(0), disposal{nullptr, nullptr} {}
  ~IoOwn() noexcept(false);
  KJ_DISALLOW_COPY(IoOwn);

  T* operator->();
  T& operator*() {
    return *operator->();
  }
  operator kj::Own<T>() &&;
  IoOwn& operator=(IoOwn&& other);
  IoOwn& operator=(decltype(nullptr));

  // Releases this object from the IoOwn, but instead of deleting it, attaches it to the
  // IoContext (or ActorContext) such that it won't be destroyed until that context is torn
  // down.
  //
  // This may need to be used in cases where an application could directly observe the destruction
  // of this object. If that's the case, then the object cannot be destroyed during GC, as this
  // would let the application observe GC, which might enable side channels. So, the destructor
  // of the owning object must manually call `deferGcToContext()` to pass all such objects away
  // to their respective contexts.
  //
  // Since this is expected to be called during GC, it is safe to call from a thread other than
  // the one that owns the IoContext.
  void deferGcToContext() &&;

 private:
  friend class IoContext;
  friend class DeleteQueue;

  kj::Arc<DeleteQueue> deleteQueue;
  hostMemory::ResolveHandle handle;
  OwnedDisposal disposal;

  IoOwn(kj::Arc<DeleteQueue> deleteQueue, hostMemory::ResolveHandle handle, OwnedDisposal disposal)
      : deleteQueue(kj::mv(deleteQueue)),
        handle(handle),
        disposal(disposal) {}

  // The owned object, resolved for this one use after checkFarGet.
  T* object() {
    DeleteQueue::checkFarGet(*deleteQueue, typeid(T));
    return static_cast<T*>(DeleteQueue::resolveOwned(handle, typeid(T)));
  }
};

// Reference held by a V8 heap object, pointing to a KJ event loop object. Cannot be
// dereferenced unless the isolate is executing on the appropriate event loop thread.
//
// Like an IoOwn, the IoPtr names its object by a resolve handle rather than a pointer, with an
// entry of its own that lives as long as the IoPtr does; it owns nothing, so there is no node
// behind the entry.
template <typename T>
class IoPtr {
 public:
  IoPtr(const IoPtr& other);
  IoPtr(IoPtr&& other) noexcept;
  ~IoPtr() noexcept(false);

  T* operator->();
  T& operator*() {
    return *operator->();
  }
  IoPtr& operator=(decltype(nullptr));

 private:
  friend class IoContext;
  friend class DeleteQueue;

  kj::Arc<DeleteQueue> deleteQueue;
  hostMemory::ResolveHandle handle;

  IoPtr(kj::Arc<DeleteQueue> deleteQueue, T* ptr);
};

// Owned pointer held by a KJ I/O object living in the same thread as an IoContext. The underlying
// object is destroyed when the ReverseIoOwn is dropped OR when the IoContext is destroyed,
// whichever comes first. Accessing the ReverseIoOwn after the IoContext is destroyed will throw.
//
// Use this when you have a KJ I/O object that could outlive an IoContext, but wants to hold onto
// some information that itself should not outlive the IoContext. In particular, if a KJ I/O object
// wants to hold JS handles (`jsg::JsRef`), this is normally safe as long as the handles do not
// outlive the isolate they point into. But if the holder could outlive the IoContext, then it
// could also outlive the isolate. In that case, the handles should be wrapped in an object held
// using `ReverseIoOwn`.
//
// The object is named by a resolve handle with its disposal recorded beside it, as an IoOwn's
// is: a ReverseIoOwn's holder lives in the shared heap, and the ReverseIoOwn is dereferenced
// and dropped inside turns as well as outside them.
template <typename T>
class ReverseIoOwn {
 public:
  ReverseIoOwn(ReverseIoOwn&& other) noexcept;
  ReverseIoOwn(decltype(nullptr)): validity(nullptr), handle(0), disposal{nullptr, nullptr} {}
  ~ReverseIoOwn() noexcept(false);
  KJ_DISALLOW_COPY(ReverseIoOwn);

  T* operator->();
  T& operator*() {
    return *operator->();
  }
  operator kj::Own<T>() &&;
  ReverseIoOwn& operator=(ReverseIoOwn&& other);
  ReverseIoOwn& operator=(decltype(nullptr));

  // Try to get the underlying object if safe to dereference.
  // Returns kj::none if the IoContext has been destroyed or if this is null.
  // This is a safe alternative to operator->() that won't throw or crash.
  kj::Maybe<T&> tryGet() {
    if (handle != 0 && validity != nullptr && validity->isValid()) {
      T* object = static_cast<T*>(hostMemory::resolve(handle));
      if (object != nullptr) return *object;
    }
    return kj::none;
  }

 private:
  friend class IoContext;
  friend class DeleteQueue;

  kj::Arc<ReverseIoOwnValidity> validity;
  hostMemory::ResolveHandle handle;
  OwnedDisposal disposal;

  ReverseIoOwn(kj::Arc<ReverseIoOwnValidity> validity,
      hostMemory::ResolveHandle handle,
      OwnedDisposal disposal)
      : validity(kj::mv(validity)),
        handle(handle),
        disposal(disposal) {}

  // Drops the object, if the context still owns it.
  void drop();
};

template <typename T>
IoOwn<T>::IoOwn(IoOwn&& other) noexcept
    : deleteQueue(kj::mv(other.deleteQueue)),
      handle(other.handle),
      disposal(other.disposal) {
  other.handle = 0;
}

template <typename T>
IoOwn<T>::~IoOwn() noexcept(false) {
  if (handle != 0) {
    deleteQueue->scheduleDeletion(handle, disposal);
  }
}

template <typename T>
IoOwn<T>& IoOwn<T>::operator=(IoOwn<T>&& other) {
  if (handle != 0) {
    deleteQueue->scheduleDeletion(handle, disposal);
  }
  deleteQueue = kj::mv(other.deleteQueue);
  handle = other.handle;
  disposal = other.disposal;
  other.handle = 0;
  return *this;
}

template <typename T>
IoOwn<T>& IoOwn<T>::operator=(decltype(nullptr)) {
  if (handle != 0) {
    deleteQueue->scheduleDeletion(handle, disposal);
  }
  deleteQueue = nullptr;
  handle = 0;
  disposal = {nullptr, nullptr};
  return *this;
}

template <typename T>
void IoOwn<T>::deferGcToContext() && {
  // Turns out, if we simply *don't* enqueue the item for deletion, we get the behavior we want.
  // So we can just forget the handle here; the context's OwnedObjectList still owns the object
  // and its entry, and releases both when it is torn down.
  handle = 0;
  disposal = {nullptr, nullptr};
  deleteQueue = nullptr;
}

template <typename T>
inline T* IoOwn<T>::operator->() {
  return object();
}

template <typename T>
inline IoOwn<T>::operator kj::Own<T>() && {
  T* result = object();
  const kj::Disposer* d;
  if (hostMemory::inTurn()) {
    d = disposal.disposer;
    OwnedObjectList::releaseInTurn(handle);
  } else {
    d = &OwnedObjectList::take(handle);
  }
  handle = 0;
  disposal = {nullptr, nullptr};
  deleteQueue = nullptr;  // not needed anymore, might as well drop the refcount
  return kj::Own<T>(result, *d);
}

template <typename T>
inline IoPtr<T>::IoPtr(kj::Arc<DeleteQueue> deleteQueue, T* ptr)
    : deleteQueue(kj::mv(deleteQueue)),
      handle(hostMemory::registerResolve(ptr)) {
  KJ_REQUIRE(handle != 0, "the resolve table is full; cannot create another I/O reference",
      hostMemory::RESOLVE_CAPACITY);
}

template <typename T>
inline IoPtr<T>::IoPtr(const IoPtr& other): deleteQueue(other.deleteQueue.addRef()),
                                            handle(0) {
  // A copy names the same object under an entry of its own; one whose handle no longer
  // resolves is a copy that resolves to nothing, like its original.
  void* object = hostMemory::resolve(other.handle);
  if (object != nullptr) handle = hostMemory::registerResolve(object);
}

template <typename T>
inline IoPtr<T>::IoPtr(IoPtr&& other) noexcept
    : deleteQueue(kj::mv(other.deleteQueue)),
      handle(other.handle) {
  other.handle = 0;
}

template <typename T>
inline IoPtr<T>::~IoPtr() noexcept(false) {
  if (handle != 0) hostMemory::unregisterResolve(handle);
}

template <typename T>
IoPtr<T>& IoPtr<T>::operator=(decltype(nullptr)) {
  if (handle != 0) hostMemory::unregisterResolve(handle);
  deleteQueue = nullptr;
  handle = 0;
  return *this;
}

template <typename T>
inline T* IoPtr<T>::operator->() {
  DeleteQueue::checkFarGet(*deleteQueue, typeid(T));
  return static_cast<T*>(DeleteQueue::resolveOwned(handle, typeid(T)));
}

template <typename T>
ReverseIoOwn<T>::ReverseIoOwn(ReverseIoOwn&& other) noexcept
    : validity(kj::mv(other.validity)),
      handle(other.handle),
      disposal(other.disposal) {
  other.handle = 0;
}

template <typename T>
void ReverseIoOwn<T>::drop() {
  if (handle == 0 || !validity->isValid()) return;
  OwnedObjectList::drop(handle, disposal);
}

template <typename T>
ReverseIoOwn<T>::~ReverseIoOwn() noexcept(false) {
  drop();
}

template <typename T>
ReverseIoOwn<T>& ReverseIoOwn<T>::operator=(ReverseIoOwn<T>&& other) {
  drop();
  validity = kj::mv(other.validity);
  handle = other.handle;
  disposal = other.disposal;
  other.handle = 0;
  return *this;
}

template <typename T>
ReverseIoOwn<T>& ReverseIoOwn<T>::operator=(decltype(nullptr)) {
  drop();
  validity = nullptr;
  handle = 0;
  disposal = {nullptr, nullptr};
  return *this;
}

template <typename T>
inline T* ReverseIoOwn<T>::operator->() {
  validity->checkValid();
  return static_cast<T*>(DeleteQueue::resolveOwned(handle, typeid(T)));
}

template <typename T>
inline ReverseIoOwn<T>::operator kj::Own<T>() && {
  validity->checkValid();
  T* result = static_cast<T*>(DeleteQueue::resolveOwned(handle, typeid(T)));
  const kj::Disposer* d;
  if (hostMemory::inTurn()) {
    d = disposal.disposer;
    OwnedObjectList::releaseInTurn(handle);
  } else {
    d = &OwnedObjectList::take(handle);
  }
  handle = 0;
  disposal = {nullptr, nullptr};
  validity = nullptr;  // not needed anymore, might as well drop the refcount
  return kj::Own<T>(result, *d);
}

}  // namespace workerd
