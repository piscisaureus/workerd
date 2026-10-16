#pragma once

#include <workerd/util/host-memory.h>
#include <workerd/util/hostcall.h>
#include <workerd/util/strong-bool.h>

#include <kj/async.h>
#include <kj/common.h>
#include <kj/function.h>
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
// object is in memory a turn can write. The list's head is in the context's record, the host
// memory block behind the context's own entry (see DeleteQueueOwner), which also holds the
// context's cross-thread queue; a node names its context by that entry's handle, and that
// handle is the only thing about the list that lives in the context, in guest memory.
//
// Objects are added inside turns, where host memory cannot be written: the registration is
// journaled by the turn with the disposer and the context's handle as its words
// (hostMemory::RESOLVE_AUX_WORDS), and the host builds and links the node after the turn, from
// its committed hook (TURN_HOOKS); the holder uses its handle in the meantime, resolved through
// the journal. An object released inside a turn by its holder is disposed of by the turn, through
// the disposer the holder recorded beside its handle, and the entry's release is journaled, so the
// host unlinks and frees the node afterwards without touching the object. Released host-side --
// a holder dropped on the event loop, the delete-queue drain, the context's teardown -- the host
// disposes of the object through the node's own copy of the disposer. A list torn down inside a
// turn (a context destroyed by another context's turn) releases what the turn itself registered
// from the journal, and drains the rest through the IO_CONTEXT hostcall, which hands the turn
// each object with its disposer and frees the nodes host-side.
//
// What this protects is the node: the links the host's walk follows and the disposer it calls
// are in host memory and were written by the host. What it does not protect is the object: the
// T is in the shared heap, placed by ordinary runtime code, so the host's disposal of it runs a
// destructor over memory a turn can write; and the disposer the node records is the one the
// turn journaled when it registered the object, which the commit admits unvalidated -- a turn
// that lies about the disposer of its own object sends the host's disposal of that object to a
// function of its choosing, the same exposure the object's own vtable and members carry. A
// turn's words can also name another context's live record, which links the turn's own object
// into that context's list (the ownership residual the table cannot close).
class OwnedObjectList {
 public:
  // `context` is the handle of the context's entry, whose record holds the list's head.
  explicit OwnedObjectList(hostMemory::ResolveHandle context): context(context) {}
  KJ_DISALLOW_COPY_AND_MOVE(OwnedObjectList);
  ~OwnedObjectList() noexcept(false);

  // Registers `object` as owned by this list and returns the handle naming it, or throws if the
  // table is full. `object` is the pointer the holder resolves (a T*); `disposal` the pointer
  // its disposer takes (kj::castToVoid of it); `type` the tag of the class it is
  // (hostMemory::resolveTypeTag), which every later use of the handle is checked against.
  hostMemory::ResolveHandle add(
      void* object, uint64_t type, void* disposal, const kj::Disposer& disposer);

  // Host-side: releases the object `handle` names from its list, disposing of it when asked,
  // and retires the entry. Reports and does nothing if the handle does not name an owned
  // object, or names one of a class other than `type` -- which is a holder's handle written
  // over another of this isolate's, where releasing would free an object its own holder still
  // names. hostMemory::RESOLVE_TYPE_NONE asks for no comparison, for a caller that is working
  // from the list rather than from a holder.
  static void release(hostMemory::ResolveHandle handle, uint64_t type, DestroyOwnedObject destroy);

  // Host-side: takes the object `handle` names out of its list without disposing of it, retires
  // the entry, and returns the disposer the object is to be disposed of with. Throws if the
  // handle does not name an owned object of class `type`.
  static const kj::Disposer& take(hostMemory::ResolveHandle handle, uint64_t type);

  // Inside a turn: releases the entry `handle` names, which the turn is disposing of itself.
  static void releaseInTurn(hostMemory::ResolveHandle handle);

  // Drops the object `handle` names on the thread of the context that owns it: inside a turn,
  // through `disposal` and releaseInTurn(); outside one, host-side through the list. Refuses a
  // handle that names a live object of a class other than `type`.
  static void drop(hostMemory::ResolveHandle handle, uint64_t type, const OwnedDisposal& disposal);

  // The hooks a ResolveTurn runs host-side for the entries a turn registered and released and
  // the notes it made (the delete queue's, see DeleteQueue).
  static const hostMemory::ResolveTurnHooks TURN_HOOKS;

  // Registers the IO_CONTEXT hostcall. Called once, host-side, before any turn runs.
  static void installHostcall();

  // Host-memory blocks allocated and freed since the process started, for tests and
  // diagnostics: the owned nodes, the context records (DeleteQueueOwner) and the queue chunks
  // the records' queues are kept in.
  struct Stats {
    size_t nodesAllocated;
    size_t nodesFreed;
    size_t recordsAllocated;
    size_t recordsFreed;
    size_t chunksAllocated;
    size_t chunksFreed;
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
  hostMemory::ResolveHandle context;

  static bool committed(hostMemory::ResolveHandle handle, void* ptr, const uint64_t* aux);
  static void released(hostMemory::ResolveHandle handle);
  static bool noted(const uint64_t* words, bool inHandler);
  static long hostcallHandler(hostcall::Frame& frame);
  static const hostcall::Entry HOSTCALL;

  friend class DeleteQueue;
  friend class DeleteQueueOwner;
};

// A context's delete queue, as its holders name it: the handle of the context's entry in the
// resolve table, and nothing else. An IoOwn, an IoPtr and an IoCrossContextExecutor each carry
// one, in memory a JS turn can write, so there is no pointer here for the host to dereference
// and no refcount for a turn to corrupt. The queue itself -- the handles of holders dropped off
// their context, the actions other contexts have asked this one to run, the signal that wakes
// the context for them, and the lock that guards them -- is in the context's record, in host
// memory, reached from the entry's companion (see DeleteQueueOwner), and the record lives as
// long as the context does: a handle whose entry has been retired names nothing, which is how a
// holder that outlives its context finds out, and resolving one is a table lookup -- at ring 0
// inside a turn -- rather than a load through this value.
//
// Inside a turn the queue cannot be written, so a turn that drops a holder of another context's
// object, or schedules an action for another context, notes it in the turn's journal
// (hostMemory::noteInTurn) for the host to queue once the turn has returned, after checking
// that the context's entry is still live; a holder of a context that has ended is dropped
// without a note, since that context's list has already disposed of its object. Outside a turn
// the host queues directly, under the record's lock, on whichever thread it is on. The context
// drains the queue on its own thread, host-side, before it enters the guest: the deletions are
// released through the owned list before the turn, and the actions, which the turn itself
// runs, are taken out of the record first and handed to it. What the host takes from a note
// is validated against the table before anything is queued -- the context must be live, and a
// deletion's handle must name an object of that context's list -- so a turn can queue nothing
// for a context other than the object's own.
class DeleteQueue {
 public:
  DeleteQueue(): context(0) {}
  DeleteQueue(decltype(nullptr)): context(0) {}
  explicit DeleteQueue(hostMemory::ResolveHandle context): context(context) {}

  // Drops the object the handle names, which the holder of `handle` and `disposal` is giving
  // up: at once if the owning IoContext is current on this thread -- inside a turn, by the
  // holder's own disposal; otherwise host-side through the owned list -- and otherwise when
  // that context next runs. A handle that names nothing (never normal: a holder keeps a live
  // entry for its lifetime) is reported and deletes nothing.
  void scheduleDeletion(
      hostMemory::ResolveHandle handle, uint64_t type, const OwnedDisposal& disposal) const;

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

  // The handle of the owning context's entry; 0 for a null queue.
  hostMemory::ResolveHandle getContext() const {
    return context;
  }

  bool operator==(const DeleteQueue& other) const {
    return context == other.context;
  }

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
  // The object `handle` names, which must be a live entry of this isolate holding an object of
  // class `tag`; throws otherwise. `type` names the class in the message.
  static void* resolveOwned(
      hostMemory::ResolveHandle handle, uint64_t tag, const std::type_info& type);

  // Retires the entry `handle` names, for a holder that owns the entry rather than the object
  // it names (IoPtr). The entry must be a live one of class `tag`, or name nothing; one that
  // names a live object of another class is a holder's handle written over another of this
  // isolate's, where retiring would leave that object's own holder unable to resolve it, so it
  // is reported and nothing is retired. Inside a turn the release is journaled and the host
  // checks it against the arena the turn ran under; outside one there is no caller to compare,
  // which is the residual every host-side retire of a guest-supplied handle carries.
  static void releaseReference(hostMemory::ResolveHandle handle, uint64_t tag);

  // Adds the object to the owned list and returns the handle naming it with the disposer the
  // holder records; on a full table, disposes of the object and throws.
  struct OwnedRegistration {
    hostMemory::ResolveHandle handle;
    OwnedDisposal disposal;
  };
  template <typename T>
  static OwnedRegistration registerOwned(kj::Own<T> obj, OwnedObjectList& ownedObjects);

  // The record behind `context`, for a test that corrupts it from inside a turn: the record's
  // address and size, and the word holding the pointer to the cross-thread signal's fulfiller,
  // which the host calls through when it queues an action; all null if the handle names no
  // live context. Host-side.
  struct RecordForTest {
    void* record;
    size_t size;
    void** fulfiller;
  };
  static RecordForTest recordForTest(hostMemory::ResolveHandle context);

 private:
  hostMemory::ResolveHandle context;
};

// Object which can push actions into a specific DeleteQueue then signal its
// owning IoContext to wake up to process the queue. This is a bit of a hack of
// the DeleteQueue concept that allows us to use the same queue for more than
// just deletions.
class IoCrossContextExecutor {
 public:
  IoCrossContextExecutor(DeleteQueue deleteQueue): deleteQueue(deleteQueue) {}

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

  DeleteQueue deleteQueue;
};

template <typename T>
inline DeleteQueue::OwnedRegistration DeleteQueue::registerOwned(
    kj::Own<T> obj, OwnedObjectList& ownedObjects) {
  const kj::Disposer& disposer = obj.getDisposer();
  // The disposer is told the object's address as it was allocated (kj::castToVoid, which is
  // what kj::Own hands it), which for a polymorphic T need not be the T* the holder resolves.
  T* object = obj.get();
  // Throws if the table is full, with `obj` still owning the object.
  hostMemory::ResolveHandle handle = ownedObjects.add(
      object, hostMemory::resolveTypeTag<T>(), kj::_::castToVoid(object), disposer);
  obj.disown(&disposer);
  return {handle, {&disposer, &disposeOwnedAs<T>}};
}

template <typename T>
inline IoOwn<T> DeleteQueue::addObject(kj::Own<T> obj, OwnedObjectList& ownedObjects) const {
  auto registration = registerOwned(kj::mv(obj), ownedObjects);
  return IoOwn<T>(*this, registration.handle, registration.disposal);
}

template <typename T>
inline ReverseIoOwn<T> DeleteQueue::addObjectReverse(
    kj::Arc<ReverseIoOwnValidity> validity, kj::Own<T> obj, OwnedObjectList& ownedObjects) const {
  auto registration = registerOwned(kj::mv(obj), ownedObjects);
  return ReverseIoOwn<T>(kj::mv(validity), registration.handle, registration.disposal);
}

// The context's side of its delete queue: registers the context's entry and builds its record
// when the context is created, drains the record on the context's thread, and tears the record
// down when the context is destroyed. The record -- the owned list's head, the queue and its
// lock -- is in host memory; the owner keeps only the entry's handle, which is what every
// holder of the context's objects keeps too.
//
// A context torn down inside a turn cannot reach its record: it releases its entry from the
// turn's journal if the entry was registered in this turn, and otherwise tears the record down
// through the IO_CONTEXT hostcall, which hands the turn the signal's fulfiller and the queued
// actions to destroy, as the turn would have destroyed them itself, frees the record host-side
// and retires the entry. Destroying the record last among the context's members gets the
// teardown order right: the owned list's objects are disposed of first, and a holder they drop
// on the way finds the context's entry still live. The IoContext declares this member above
// the owned list for that reason.
//
// What a turn can still do is drop a holder whose record of the queue is a rewritten handle:
// one that names nothing is refused with the object left to the context's list, and one that
// names another live context queues nothing there either, since the handle the turn drops must
// name an object of that context's own list.
class DeleteQueueOwner {
 public:
  // `arena` is the guest-kernel arena of the isolate whose turns will resolve the context's
  // handle (hostMemory::IsolateArena): a context is created on the event loop, outside the
  // isolate lock, so the entry names the isolate rather than inheriting the thread's.
  explicit DeleteQueueOwner(hostMemory::IsolateArena arena);
  KJ_DISALLOW_COPY_AND_MOVE(DeleteQueueOwner);
  ~DeleteQueueOwner() noexcept(false);

  // Host-side, on the context's thread: releases, disposing of each, the objects whose holders
  // were dropped off the context since the last drain.
  void drainDeletions();

  // Host-side, on the context's thread: takes the actions queued for the context, for the
  // context's next turn to run.
  kj::Vector<kj::Own<kj::Function<void(jsg::Lock&)>>> takeActions();

  // Host-side, on the context's thread: installs a fresh cross-thread signal and returns the
  // promise it fulfills, which the next action queued for the context fulfills -- at once if
  // actions are already queued. Never resolves if the context's record is gone.
  kj::Promise<void> resetCrossThreadSignal();

  // The queue, as holders name it. Set once, when the entry is registered.
  DeleteQueue queue;
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
// to. The context that owns the object is named the same way, by its own entry's handle
// (DeleteQueue), compared with the current context's on every use and resolved by the host when
// the IoOwn is dropped off its context. Beside the two handles the IoOwn records how to dispose
// of the object (OwnedDisposal), which only the turn uses: an IoOwn dropped inside a turn
// disposes of its object there, since the context's record of the object (OwnedObjectList) is
// in host memory, which the turn cannot reach; the host never reads this copy, and disposes of
// the object through the record's own. The object itself is still in the shared heap, and a
// handle rewritten to another live entry's names that entry's object.
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

  DeleteQueue deleteQueue;
  hostMemory::ResolveHandle handle;
  OwnedDisposal disposal;

  IoOwn(DeleteQueue deleteQueue, hostMemory::ResolveHandle handle, OwnedDisposal disposal)
      : deleteQueue(deleteQueue),
        handle(handle),
        disposal(disposal) {}

  // The owned object, resolved for this one use after checkFarGet.
  T* object() {
    DeleteQueue::checkFarGet(deleteQueue, typeid(T));
    return static_cast<T*>(
        DeleteQueue::resolveOwned(handle, hostMemory::resolveTypeTag<T>(), typeid(T)));
  }
};

// Reference held by a V8 heap object, pointing to a KJ event loop object. Cannot be
// dereferenced unless the isolate is executing on the appropriate event loop thread.
//
// Like an IoOwn, the IoPtr names its object by a resolve handle rather than a pointer, with an
// entry of its own that lives as long as the IoPtr does; it owns nothing, so there is no node
// behind the entry. The class of the object is checked against the entry's type word at the
// dereference and at the drop, as an IoOwn's is: a handle written over another of this
// isolate's names that entry's object, and retiring it would leave the holder that really owns
// it resolving nothing.
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

  DeleteQueue deleteQueue;
  hostMemory::ResolveHandle handle;

  IoPtr(DeleteQueue deleteQueue, T* ptr);
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
    : deleteQueue(other.deleteQueue),
      handle(other.handle),
      disposal(other.disposal) {
  other.handle = 0;
}

template <typename T>
IoOwn<T>::~IoOwn() noexcept(false) {
  if (handle != 0) {
    deleteQueue.scheduleDeletion(handle, hostMemory::resolveTypeTag<T>(), disposal);
  }
}

template <typename T>
IoOwn<T>& IoOwn<T>::operator=(IoOwn<T>&& other) {
  if (handle != 0) {
    deleteQueue.scheduleDeletion(handle, hostMemory::resolveTypeTag<T>(), disposal);
  }
  deleteQueue = other.deleteQueue;
  handle = other.handle;
  disposal = other.disposal;
  other.handle = 0;
  return *this;
}

template <typename T>
IoOwn<T>& IoOwn<T>::operator=(decltype(nullptr)) {
  if (handle != 0) {
    deleteQueue.scheduleDeletion(handle, hostMemory::resolveTypeTag<T>(), disposal);
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
    d = &OwnedObjectList::take(handle, hostMemory::resolveTypeTag<T>());
  }
  handle = 0;
  disposal = {nullptr, nullptr};
  deleteQueue = nullptr;
  return kj::Own<T>(result, *d);
}

template <typename T>
inline IoPtr<T>::IoPtr(DeleteQueue deleteQueue, T* ptr)
    : deleteQueue(deleteQueue),
      handle(hostMemory::registerResolve(ptr, hostMemory::resolveTypeTag<T>())) {
  KJ_REQUIRE(handle != 0, "the resolve table is full; cannot create another I/O reference",
      hostMemory::RESOLVE_CAPACITY);
}

template <typename T>
inline IoPtr<T>::IoPtr(const IoPtr& other): deleteQueue(other.deleteQueue),
                                            handle(0) {
  // A copy names the same object under an entry of its own; one whose handle no longer
  // resolves is a copy that resolves to nothing, like its original.
  void* object = hostMemory::resolveOfType(other.handle, hostMemory::resolveTypeTag<T>());
  if (object != nullptr) {
    handle = hostMemory::registerResolve(object, hostMemory::resolveTypeTag<T>());
  }
}

template <typename T>
inline IoPtr<T>::IoPtr(IoPtr&& other) noexcept
    : deleteQueue(other.deleteQueue),
      handle(other.handle) {
  other.handle = 0;
}

template <typename T>
inline IoPtr<T>::~IoPtr() noexcept(false) {
  if (handle != 0) DeleteQueue::releaseReference(handle, hostMemory::resolveTypeTag<T>());
}

template <typename T>
IoPtr<T>& IoPtr<T>::operator=(decltype(nullptr)) {
  if (handle != 0) DeleteQueue::releaseReference(handle, hostMemory::resolveTypeTag<T>());
  deleteQueue = nullptr;
  handle = 0;
  return *this;
}

template <typename T>
inline T* IoPtr<T>::operator->() {
  DeleteQueue::checkFarGet(deleteQueue, typeid(T));
  return static_cast<T*>(
      DeleteQueue::resolveOwned(handle, hostMemory::resolveTypeTag<T>(), typeid(T)));
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
  OwnedObjectList::drop(handle, hostMemory::resolveTypeTag<T>(), disposal);
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
  return static_cast<T*>(
      DeleteQueue::resolveOwned(handle, hostMemory::resolveTypeTag<T>(), typeid(T)));
}

template <typename T>
inline ReverseIoOwn<T>::operator kj::Own<T>() && {
  validity->checkValid();
  T* result = static_cast<T*>(
      DeleteQueue::resolveOwned(handle, hostMemory::resolveTypeTag<T>(), typeid(T)));
  const kj::Disposer* d;
  if (hostMemory::inTurn()) {
    d = disposal.disposer;
    OwnedObjectList::releaseInTurn(handle);
  } else {
    d = &OwnedObjectList::take(handle, hostMemory::resolveTypeTag<T>());
  }
  handle = 0;
  disposal = {nullptr, nullptr};
  validity = nullptr;  // not needed anymore, might as well drop the refcount
  return kj::Own<T>(result, *d);
}

}  // namespace workerd
