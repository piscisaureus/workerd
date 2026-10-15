# Memory classes for runtime C++ objects

Guest-kernel isolation (see `README.md`) maps each isolate's memory into a
private page-table arena, so another isolate's arena is unmapped during a
turn and a stray access faults. The enforcement exists; what is missing is a
decision, for every runtime object, about which side of the boundary it lives
on. Without that decision the arena covers whatever the allocator happens to
place in it, which is how shared state ends up inside one isolate's arena and
faults every other isolate that reads it.

This file is that decision procedure, and the inventory of where the code
stands against it.

## The classes

**Isolate (I).** Lives in the isolate's arena. Mapped only while that
isolate's turn runs, so no other isolate and no host-side code can address it.
The host does not read or write an I object: what the host needs from one is
serialized out of the arena first, as a Cap'n Proto message in the boundary
region. An I object may hold raw pointers to other I objects in the same
arena, and to read-only runtime text and rodata. Everything else it refers to
is either an opaque handle naming a host object, or a bounds- and
owner-checked descriptor for a binary buffer. In particular an I object holds
no `kj::Own` whose disposer lives outside the arena, and no vtable that the
host will later call through.

**Host (H).** Lives outside every arena, in memory no arena maps, so ring 3
can neither read nor write it. Which of gk's two protected classes it takes
follows from who serves the crossing: state a ring-0 helper inside the guest
has to reach is `GK_PROT_SUPER` (mapped, `PTE_U` clear, so ring 0 reaches it
and ring 3 faults), and state only the host proper touches is
`GK_PROT_REFUSE`, never mapped into any guest root at all. Both are how gk
already protects its own control state; see `prot_add` in `gk.c`. REFUSE is
the default, since a SUPER range is reachable by any ring-0 code the guest
can get to run, and there is no in-guest ring-0 helper yet that would need
SUPER. What backs H today is `gk_host_alloc` (`gk.h`): regions mapped by the
host and registered REFUSE once each, suballocated by size class, so the
fixed-size protection registry is spent per region rather than per object.
`util/host-memory.h` wraps it for C++: `hostOwned<T>(...)` constructs a `T`
there and `HostOwn<T>` owns it, as `kj::heap` and `kj::Own` do on the heap,
and without the guest kernel the same code uses the ordinary heap. May hold
raw pointers to other H objects. An H object whose owner is not itself in H
memory is held through a host slot instead (`hostSlot<T>(...)` and
`HostSlot<T>`): the owner keeps a handle into gk's host slot table, which is
in the supervisor control block, and every access resolves the handle
against that table -- bounds, liveness and generation -- so a rewritten
handle resolves to nothing rather than to an address the guest chose. Refers
to an I object only as an opaque handle together with the arena it belongs
to; dereferencing one means entering that arena, which only a turn or a
ring-0 helper does. A turn that needs an H object's help synchronously --
an operation on a secret whose JS caller gets the result directly -- makes a
hostcall (`gk_hostcall` in `gk.h`, `util/hostcall.h` in C++): the host
handler it names by a validated index runs host-side, on a copy of the
arguments in H memory, and the turn resumes with the result.

**Boundary (B).** The one region both sides touch, one per isolate. Holds the
messages the two sides exchange, the handle tables' isolate-visible halves,
and buffer descriptors. Contains no pointers, only offsets, lengths and
handles. Everything the host reads from it is untrusted input and is validated
on every read, with no assumption that a value read twice is the same value.

**Provenance.** A class is not safe merely because its own memory is on the
right side. What matters as much is where the host got the pointer it used to
reach the object: a pointer the host loads out of I or B memory names whatever
the guest put there. Every host-side flaw found in this codebase so far has
this shape -- the page-table root the host installed, the arena the host
destroyed, the address the page allocator mapped, the turn-depth counter, the
`jsg::Lock` the host re-installs, the buffer a signal handler writes. So the
rule is: a pointer the host dereferences, and any value the host uses to
decide what to dereference, comes from H memory. An index into an H-owned
table may come from anywhere, because the host validates it against the
table; a pointer may not. The host slot table (`gk_host_slot_alloc` in
`gk.h`) is that table for the runtime's H-memory tenants, and it has the
residual every such table has: validation tells a handle that names nothing
from one that names a live entry, but not a handle rewritten to another live
tenant's from the owner's own. That is an ownership bound, the same one
`gk_arena_enter` and `gk_arena_destroy` carry for a pointer rewritten to
another live arena.

The first two classes are the whole point: an object is one isolate's or it is
the runtime's. B is not a third kind of object but the channel, and it exists
because without a region both sides can touch there is nothing to carry a
message. Keeping it a named class with its own rules is what stops it growing
into "shared memory" by accident.

## Deciding a class

For a given class, in order. The first question that answers decides it.

1. Is its lifetime owned by a JS object -- a `jsg::Ref`, a `JSG_RESOURCE_TYPE`
   wrapper, a cppgc-traced object -- and does nothing outside the isolate hold
   a reference to it? **I.**
2. Is it reached from the KJ event loop, a timer, an I/O completion, a
   background thread, or more than one isolate? **H.**
3. Does it exist once per process or once per thread rather than once per
   isolate? **H.** A singleton, a registry, a file-scope cache and the
   allocator's own metadata are all H, and an I object may not own storage
   that one of them will later read.
4. Is it a leaf utility with no state of its own beyond what its owner gives
   it -- a string view, a parser, a small value type? It takes the class of
   whatever owns it, and needs no annotation.
5. Otherwise it is used from both sides, and it does not have a class: it has
   a **split**. Naming the split is the answer -- which members go to which
   side, and whether the interface between the halves is a message, a checked
   buffer, or a handle.

A class that fails rule 1 only because something outside the isolate holds a
reference to it is usually a split, not an H class. The reference is the thing
to replace, with a handle.

## What replaces the patterns that cross today

**`kj::Own` across the boundary becomes a handle.** An I object that needs a
host resource holds an integer handle into a table that lives in H memory. The
host owns the resource and the table entry; the entry carries a generation
counter, and the host validates index and generation on every use, so a
rewritten handle names nothing or names a dead entry rather than an object of
the attacker's choosing. Isolate teardown walks the table host-side instead of
running disposers inside the arena.

This is what `IoOwn` and `DeleteQueue` (`src/workerd/io/io-own.h`) already do
in spirit -- make a JS-heap reference to a KJ I/O object safe across context
teardown -- with the ownership and the validity check moved to the side the
guest cannot write.

**A returned object becomes a message.** A turn that produces a value for the
host writes it into the boundary region in a form with no pointers and no
disposers. This is why the non-void results of `IoContext::run` cannot be
fixed by marshalling the out-parameter: 25 of its 27 non-void call sites
return a `kj::Promise`, whose node is built in the arena and carries a vtable
the host would call through. The promise has to be created host-side and the
turn has to resolve it through the channel.

**A synchronous in-turn use of H state becomes a hostcall.** When the JS
API is synchronous, there is no promise to return, so the turn cannot defer
the use to the event loop; it submits a hostcall instead. The handler is
selected by a small integer the host checks against its table, the
arguments and the result cross through a per-thread exchange frame the host
owns and snapshots into H memory before the handler runs, and the handler
validates every field it reads, because the turn calls it whenever and with
whatever it likes. `ActorIdFactoryImpl` (`server/actor-id-impl.h`) is the
pattern: the key is H, reached by a host slot handle the frame carries, and
the HMAC runs in the handler. The Wasm signal descriptor
(`io/tracked-wasm-instance.h`) is the other user: the turn submits a
buffer by offset and length and the handler has gk validate it against the
arena gk records the thread as running in. A hostcall costs a VM exit, so it
is for the rare operation, not the bulk of a turn's crossings (see Cost).

**Binary data becomes a descriptor.** An `ArrayBuffer` backing store or a body
chunk crosses as `{handle, offset, length}` against a buffer the host owns,
checked on every use, rather than as a pointer the host dereferences. This is
the one case where the boundary carries bulk data rather than a message, and
the reason the rubric permits it is that the content is opaque bytes: nothing
the host does with it depends on its value.

## Enforcing it

Three layers, each catching what the one before cannot.

1. **Placement.** The allocator decides the class in practice: an I object has
   to come from the isolate's arena and an H object from memory no arena maps.
   Per-isolate heap arenas are the mechanism for I (`jsg::HeapArena`); for H
   it is `gk_host_alloc` behind `HostOwn<T>` for an owner that is itself in
   H memory and `gk_host_slot_alloc` behind `HostSlot<T>` for one that is
   not, chosen per object by the code that owns it rather than by the
   allocator.
2. **A static check.** The classes are a type-level property, so the
   clang-tidy check `workerd-memory-class` (`tools/clang-tidy/memory-class.c++`)
   makes an H class with an I-class member, or an I class holding a `kj::Own`
   of an H class, a lint error rather than a fault in production. A class opts
   in by carrying one of `WD_MEMORY_CLASS_HOST`, `WD_MEMORY_CLASS_ISOLATE` or
   `WD_MEMORY_CLASS_BOUNDARY` (`src/workerd/util/memory-class.h`) between the
   class-key and its name, as in `class WD_MEMORY_CLASS_HOST V8System`; the
   annotation compiles to nothing. For an annotated class the check examines
   every non-static data member and base class, through typedefs, references
   and the `kj::Own`, `kj::Arc`, `kj::Rc`, `kj::Maybe`, `kj::Array`,
   `kj::Vector` and `kj::OneOf` wrappers, and rejects:
   - in an H class, an I class by value, pointer or reference, or any of the
     R2 handle types `v8::Global`, `v8::TracedReference`, `jsg::Data`,
     `jsg::V8Ref`, `jsg::Value`, `jsg::JsRef`, `jsg::Ref`, `jsg::Promise`,
     `jsg::Function` and `jsg::JsContext`;
   - in an I class, an H class by value, pointer, reference or ownership, or
     any of the R3 primitives `kj::Promise`, `kj::ForkedPromise`,
     `kj::TaskSet`, `kj::PromiseFulfiller`, `kj::Canceler`, `kj::MutexGuarded`
     and `kj::Executor`;
   - in a B class, anything but integers, enums, bools, floating-point values,
     arrays of those, the R6 value types `kj::Date` and `kj::Duration`, and
     other B classes: no pointer, reference, vtable or non-trivial destructor,
     and the B class itself is not polymorphic.

   Unannotated classes are not examined, so adoption is per class: a class is
   annotated once it passes, and a class that needs a split (see the
   inventories below) stays unannotated until it is split. The check decides
   by type alone. It does not see provenance, a pointer laundered through an
   integer, a static data member (which lives in process memory whatever its
   class says), or a crossing made through a method rather than a member;
   those are what measurement is for.
3. **Measurement.** `GK_TRACE_FOREIGN` (on the `guest-kernel-trace` branch)
   records every ring-3 access to memory outside the arena, with the faulting
   RIP. Any access to an address in H memory is a violation of the
   classification, found by running the test suite rather than by reading
   code. A hello-world turn currently makes about 43 such accesses, across
   about 32 distinct pages.

The order matters: the static check is what makes the classification hold as
the code changes, and the measurement is what finds the places the static
check cannot see, such as a write through a pointer that was laundered
through an integer.

## Cost

The classification implies a crossing wherever a turn reaches H state today.
The crossing that exists is the hostcall, a VM exit: **6.4 us per round trip,
measured** (`hostcall_check` in `gk_test.c`), which is affordable for an
operation a turn makes a few times (an actor id) and not for the bulk of its
crossings. For those the crossing should be a ring 3 to ring 0 transition
inside the guest, not an exit: **22 ns, measured, 212 times cheaper than the
4.75 us of an exit** (see `m10_ringtrap` in the benchmark suite). At the ~43
crossings a hello turn takes that is about 1 us, roughly 1% of the request.
The explicit trap is 9.5 times cheaper than servicing a demand fault, so that
interface should be a call into ring 0 rather than fault-and-service; a
ring-0 helper is what would need the SUPER class of H memory.

Most of the foreign accesses a hello turn makes -- roughly seven in ten of
the recorded hits -- are to the allocator's global state, which per-isolate
heap arenas remove rather than convert into crossings, so the crossing count
is an upper bound that the placement work reduces first.

## Inventory: rules that classify the bulk

Each rule has a test that can be applied without reading the class body.

| Rule | Test | Class |
|---|---|---|
| R1 | Declares `JSG_RESOURCE_TYPE(`, equivalently derives from `jsg::Object` | **I**. `jsg::Object` inherits `Wrappable` (`jsg/jsg.h:1384`); the only allocation path is `Lock::alloc` -> `kj::refcounted<T>` (`jsg.h:2753`); lifetime is GC-driven through the `v8::TracedReference` wrapper (`wrappable.h:309`). Its vtable is only ever called by the heap tracer and the memory tracker, both under the lock |
| R2 | Has a member of type `v8::Global`, `v8::TracedReference`, `jsg::Data`, `V8Ref`, `JsRef`, `Ref`, `WeakRef`, `jsg::Promise`, `Resolver`, `jsg::Function`, `JsContext` | **I**, or a split site if the enclosing class is otherwise H. All of them bottom out in `v8::Global<v8::Data>` (`jsg.h:969`) or a `kj::Own` of a `Wrappable` (`jsg.h:1594`) |
| R3 | Has a member of, or implements, `kj::Promise`, `ForkedPromise`, `TaskSet`, `PromiseFulfiller`, `Canceler`, `kj::Timer&`, `AsyncInputStream`/`AsyncIoStream`, `HttpClient`/`HttpService`, `capnp::*::Client`/`Server`, `kj::Executor`, `EventLoopLocal`, `MutexGuarded` | **H**. The codebase already enforces this split at the type level in the other direction: `jsg::Promise<kj::Promise<T>>` is a `static_assert` failure naming `IoOwn` as the bridge (`jsg/promise.h:470`), and `PromiseWrapper` deletes `kj::Promise<v8::Global<T>>` (`io/promise-wrapper.h:464`) |
| R4 | Abstract class of embedder hooks whose implementation lives outside `io/` and `jsg/` (the `observer.h` convention) | **H**, and every in-turn call through its vtable is a message site. Covers `RequestObserver` (`io/observer.h:93`), `LimitEnforcer` (`io/limit-enforcer.h:221`), `IoChannelFactory` and its channels (`io/io-channels.h:119`), `TimerChannel` (`:69`), `CacheClient` (`:44`), `ActorIdFactory` (`io/actor-id.h:41`), the gate hooks (`io/io-gate.h:44`), `jsg::IsolateObserver` (`jsg/observer.h:25`), `Serializer::ExternalHandler` (`jsg/ser.h:73`) |
| R5 | No path from a `Wrappable` except through `IoOwn`/`IoPtr`/`ReverseIoOwn` or a `kj::Function` captured into a KJ promise | **H**. Promise nodes, `TaskSet` entries, timer tasks, HTTP client objects, `WorkerInterface` instances, capnp RPC state |
| R6 | Every member is an integer, enum, `bool`, `kj::Date`, `kj::Duration`, or a `kj::String`/`kj::Array<byte>`/`kj::Vector` of the same | Side-neutral; crosses by copying into B. `IoContext::Id` (`io-context.h:521`), `TimeoutId` (`io/io-timers.h:17`), `tracing::SpanId`/`TraceId` (`io/trace.h:45`, `:128`), every event struct in `io/trace.h:373-985`, `Trace` (`:989`, which already has capnp `copyTo`/`mergeFrom` at `:1049`), `jsg::GuestTurnOutcome`. Caveat: a `kj::String` carries an `ArrayDisposer*`, so it crosses as bytes plus length and is re-owned on arrival, never moved as a `kj::String` |
| R7 | `thread_local`, or a function-local `static` with mutable state | Reachable by the guest, so a value the host trusts after a turn, or that a signal handler or a forwarded syscall consumes mid-turn, must not come from it. Two remedies are in use. A value the host reads only *after* the turn is saved on the host stack frame that runs the turn and put back when the guest returns (the frame is read-only to the guest under the host-frame wall): `GuestTurnThreadLocals` (`io/guest-turn-thread-locals.h`) does this for `threadLocalRequest` (`io-context.c++:29`), `currentApi` (`worker.c++:557`, with `Worker::Lock` also restoring it from the stack rather than its heap `Impl` at release), `tmpDirStorageScope` (`worker-fs.c++:57`), `symbolicLinkGuard` (`worker-fs.c++:53`), `SqliteMemoryScope::threadLocalScope` (`sqlite-metering.c++:36`) and the `AllowV8BackgroundThreads`/`WarnAboutIsolateLock` scope depths (`thread-scopes.c++`); `guestTurnDepth` (`guest-run.c++:22`) was the first. A value consumed *during* the turn with host privilege is kept out of the guest's reach instead: `activeProgressCounter` (`thread-scopes.c++:27`), read by the watchdog's signal handler, lives in gk's per-thread supervisor words (`gk_thread_words_*`), and `currentVfsRoot` (`sqlite.c++:1860`), the dirfd of SQLite's forwarded path syscalls, is bounded by gk's turn directory (`gk_confine_paths`/`gk_turn_dir_set`), which the wrapped-native VFS sets around each native call that may name a file through a hostcall whose handler reads the descriptor from a host slot. `guestTurnArena` (`guest-run.c++:35`) needs neither: a rewrite only weakens a diagnostic the guest draws at ring 3, and the host never dereferences it. `vfsErrorListener` (`sqlite.c++:196`) needs neither: `SqliteCallScope` clears it unconditionally and it is consumed only at ring 3 within the turn. Still open: `inCppgcShimDestructor` (`wrappable.c++:29`), `CompileCache::get()`'s static (`jsg/compile-cache.h:47`), `AsyncLockQueue::threadCurrentWaiter` (`io/async-lock-scheduler.h:159`) |
| R8 | Every public method takes `jsg::Lock&` and the class has no R3 member | **I**. `jsg::Serializer`/`Deserializer` (`jsg/ser.h:60`), `MemoryTracker` (`jsg/memory.h:176`), `jsg::Url` and friends, the `TypeWrapper`/`ResourceWrapper` machinery (`jsg/type-wrapper.h:635`), `ModuleRegistryImpl` (`jsg/modules.h:280`), `AsyncContextFrame` (`jsg/async-context.h:70`) |
| R9 | Process-wide singleton of the V8 embedding | **H**. `V8System` (`jsg/setup.h:53`), `V8PlatformWrapper` (`jsg/v8-platform-wrapper.h:13`), `GuestArena` (`setup.h:194`). `ArenaPageAllocator` (`setup.h:121`) is the exception: V8 calls it from in-arena threads, so its code runs in the guest and its range checks are not a host-side guarantee -- the enforcement for it is the syscall filter's `gk_mapping_syscall_ok` |

## Inventory: `io/` and `jsg/`

This is where almost every split lives. The turn lambda
(`io-context.c++:1476-1623`) touches host state before any API code runs:
`registerPendingEvent()` (`:1483`), `limitEnforcer->enterJs` (`:1486`),
`js.pumpMsgLoop()` (`:1541`), `getLimitsExceeded()` (`:1543`),
`runnable.run(workerLock)` through a vtable on a host-stack object (`:1582`),
`abortException` (`:1593`), `logUncaughtException` (`:1616`). Then every API
call reaches `IoContext::current()`, `addObject`/`IoOwn`, `awaitIo`,
`getSubrequest*`, `getIoChannelFactory()`, `makeTraceSpan`, the actor cache,
the input gate and the timeout manager -- all by direct pointer today.

The splits, with the half each member goes to:

| Class | Split |
|---|---|
| `IoContext` (`io-context.h:277`) | H keeps the event-loop state: `tasks`/`waitUntilTasks` (`:1235`, `:1245`), `abortFulfiller`/`abortPromise` (`:1210`), `canceler` (`:1250`), `ownedObjects` (`:1221`), `localCapSet` (`:1187`), `thread` (`:1172`), `limitEnforcer` (`:1180`). I gets a mirror: the `Id`, the JsRefs (`:1269`), the invocation span context, the policy constants, and the handle tables. `IoContext::current()` becomes a read of the mirror through a slot the host writes before each turn |
| `IoContext::Runnable`/`RunnableImpl` (`:1272`, `:1450`) | Becomes a B message `{closure handle, input descriptors, result slot}`. This is where the non-void result problem lives |
| `IoOwn`/`IoPtr`/`ReverseIoOwn` (`io-own.h:245`, `:290`, `:322`) | I side becomes `{context slot, object index}`; `operator->` is deleted for an H `T`, so every use becomes a message keyed by the handle. H keeps `OwnedObjectList`, split by `T`: it owns H objects outright and holds an arena handle for I ones (the functors from `addFunctor`, the `SpanBuilder` tuple at `:629`) |
| `DeleteQueue` (`io-own.h:89`) | `State::queue`/`actions` become a B ring of `{release handle}` and `{closure handle}` records, so `~IoOwn` stops taking a host mutex from inside the arena during GC (`io-own.c++:15`). The cross-thread wakeup stays H |
| `jsg::Lock` (`jsg.h:2746`) | H: the `v8::Locker` and `GuestArenaScope`. I: `v8Isolate`, `isolateScope`, `warningsLogged` and the wrap/alloc methods. The lock slot `Lock::from` reads is neither: it is written by the host and put back from the host frame that entered the guest (violation 1, closed). The object itself is still one heap allocation holding both halves (`server/workerd-api.c++:305`), reached from the stack `Worker::Lock` through its heap `Impl` |
| `jsg::IsolateBase` (`setup.h:244`) | I: `ptr`, the behaviour flags, `typeHandlerRegistry`, the V8 global templates, `heapTracer`. H: `v8System`, `observer`, the external-memory target. B: the deferred-destruction queue becomes a ring of `{kind, handle}` records rather than `kj::OneOf<GlobalToDelete, RefToDelete, kj::Own<void>>` (`setup.h:584`), and `codeMap` (`:739`) becomes a B array of `{start, size, name offset}` the host signal handler bounds-checks |
| `Worker` / `Script` / `Isolate` / `Actor` (`worker.h:104`, `:246`, `:333`, `:887`) | Each splits along `X` versus `X::Impl`: the `Impl`s are I (contexts, `env`, `ctxExports`, `namedHandlers` of `jsg::Function`, `classInstance`, `transient`), the outer objects keep scheduling, metrics, limits, the inspector transport, condemnation, the actor cache and the gates |
| `Worker::Lock` (`worker.h:732`) | The object is on the host stack but `impl` is heap (`worker.c++:2508`) and `~Lock` calls `checkInWithLimitEnforcer` through it (`:2512`). H keeps the lock record and the limit check-in; in-turn users move to the I mirror |
| `TimeoutManager` (`io/io-timers.h:66`, `io-context.c++:45`) | H: `{id -> when, fulfiller, repeat}` and the timer task. I: `{id -> jsg::Function}`. Messages `schedule(id, delayMs, repeat)`, `clear(id)`, and `fire(id)` as a turn request |
| Trace types (`io/trace.h:1185`, `:1123`) | H: `Span`, the `SpanObserver` chain, the tracer. I: `SpanBuilder`/`SpanParent`/`TraceContext` become `{span handle}` plus a tag buffer, with messages `spanOpen`/`spanSetTag`/`spanAddLog`/`spanClose`. `trace.capnp` already serializes the payloads |
| `IoChannelFactory` and the `WorkerInterface`/`kj::HttpService` plumbing (`io-channels.h:119`, `io/worker-interface.h:36`) | H only. The `uint channel` the I side holds is already the handle. A subrequest becomes a message in the http-over-capnp shape plus B ring buffers for bodies; the host is the only side that touches sockets |
| `ActorCache` and storage (`io/actor-cache.h:345`, `io/actor-sqlite.h:16`) | H only; messages for get/put/delete/list/transaction, with a cache hit returning a checked buffer immediately and a miss returning a promise handle |
| `InputGate`/`OutputGate` (`io/io-gate.h:34`, `:250`) | H only; `Lock` and `CriticalSection` become handles, and `blockConcurrencyWhile` sends `startCriticalSection(parent handle)` |
| `Frankenvalue` (`io/frankenvalue.h:19`) | Bytes plus a cap table; `IoChannelCapTableEntry` (`io-channels.h:627`) is already `{type, channel}`, which is the right form |
| `modules::ModuleRegistry` (`jsg/modules-new.h:692`) | H: the builder, bundle ownership, the mutex taken on every lookup (`:801`). I: the per-isolate compiled-module table, plus the bundles mapped read-only into the arena since they are immutable after `finish()` |
| `TrackedWasmInstanceList` (`io/tracked-wasm-instance.h`) | **Split, done.** H is gk's signal field table of validated `{arena, address, kind}` entries, keyed by arena; the heap entry keeps the memory pin, `instanceRef`, the GC filter and the handles for unregistering. See violation 2 |
| `AsyncContextFrame::StorageKey` (`jsg/async-context.h:73`) | Identity is `this` (`:101`) and H holds it for identity only (`worker.h:605`, `setup.h:669`); replace with a 64-bit key the host mints |
| `V8PlatformWrapper::JobTaskWrapper` (`jsg/v8-platform-wrapper.h:86`) | H, but it needs a per-job `gk_arena_enter`, and the job must carry the arena id -- which V8's `JobTask` does not currently expose |

### Two violations that were full escapes, both closed

**1. The isolate's lock slot, closed.** `Lock::from()` does
`*reinterpret_cast<Lock*>(v8Isolate->GetData(SET_DATA_LOCK))` (`jsg.h`) with
no validation, and `jsg::Lock` is polymorphic, so whoever calls it then calls
virtuals through a vtable pointer read from the slot. The slot is in
`IsolateData`, a member of the C++ isolate object, which is not in the
isolate's arena at all: it is in the shared runtime heap, which gk maps
writable into every arena, so the value comes out of memory *any* isolate's
turn can write, not just the owning one. What made this an escape was host
code acting on it: host code holding the lock after a turn calls `Lock::from`
(V8 callbacks, GC callbacks, the lock's own teardown) and `jsg::Lock` is
heap-allocated (`server/workerd-api.c++:305`), so a destructor that restored
the slot from a member of that object wrote a pointer the turn chose into the
slot, and a constructor that decided from the slot whether the lock was
recursive could be made to refuse (release) or abort (debug) the next lock
on the isolate.

What closes it is that the host never reads the slot to decide anything, so
the value in it when host code calls `Lock::from` was put there by the host:

- The frame that enters the guest (`runTrampolineInGuest`,
  `jsg/guest-run.c++`) saves the isolate's jsg data slots -- the lock, the
  `IsolateBase`, the type wrapper, the `Worker::Isolate`, the cage base and
  the Rust realm -- on the host stack before `gk_run_here_user` and puts them
  back when it returns, whatever the turn left in them, on the fault path
  too. This is the same remedy as `GuestTurnThreadLocals` (R7): the frame is
  read-only to the turn under the host-frame wall. The value restored is the
  one the host read before the turn, which is the `jsg::Lock` the host frames
  hold.
- `Lock::~Lock()` clears the slot. There is no `previousData`: a recursive
  lock on the same isolate and thread is refused instead, in release as well
  as debug, and whether a lock is recursive is decided from
  `v8::Locker::IsLocked` -- V8's thread manager -- not from the slot.
- `Lock::Lock()` writes the slot and reads nothing from it.

`lock-slot-test.sh` shows a turn overwriting its own isolate's slot, and
another isolate's turn overwriting it, with host code holding the lock after
each turn getting its own lock from `Lock::from`, the slot clear once each
lock is released, and no lock refused as recursive.

What remains, stated precisely. *In-turn*, `Lock::from` still reads the slot,
so a turn with a write primitive makes its own in-turn callers of `Lock::from`
call through a vtable at an address it chose. That call happens at the turn's
guest ring 3, in the turn's own arena: it can only reach code already mapped
executable there -- the runtime image, which gk maps executable and not
writable (`map4k_root` in `gk.c`), and the isolate's own JIT pages -- and
it runs with the reach ring 3 already has (the shared heap, this thread's
stack below the wall, other threads' stacks, the allowlisted syscalls), not
with host privilege; every heap vtable and `kj::Own` disposer the turn calls
through offers the same, so this is no more than an escape with a write
primitive has anyway. *Cross-isolate*, the slot is one shared-heap word among
all the others: isolate A's turn can write isolate B's slot at any moment,
including while B's turn runs on another thread, or after B's host frame has
restored it and before B's host code calls `Lock::from` -- the host-frame
wall holds only against the thread's own turn (gk.h). The host-frame restore
therefore closes the sequential case, where the write precedes B's restore,
and leaves the concurrent one, which is the shared-heap limitation itself (A
can rewrite any heap object B's turn or B's host code uses) and is closed by
moving the shared heap out of ring 3's reach, not by anything specific to
this slot. `Lock::from` does not have to go for that: it reads a word the
host wrote, and what has to change is who can write the word.

The lock object itself stays on the heap. Putting it on the stack means the
callback-style locking noted at `worker.h:635`, and buys the same-thread
case only -- the host-side chain from the stack `Worker::Lock` to the
`jsg::Lock` runs through the heap `Worker::Lock::Impl` (its `inner` reference
and the `kj::Own<jsg::Lock>` whose disposer the host calls through at
release), which is the general `kj::Own`-across-the-boundary problem of the
KJ types inventory below, not something this slot's fix changes.

**2. The Wasm signal descriptor, closed.** `TrackedWasmInstanceList::writeShutdownSignal`
(`io/tracked-wasm-instance.c++`) runs in a signal handler, deliberately
without a lock, and writes four bytes into a Wasm module's linear memory to
tell it its CPU budget is nearly spent. The entry describing the module is a
node of a list in the isolate's limit enforcer (`server.c++`), an ordinary
heap object, so with the guest kernel on it is memory a ring-3 escape can
write; a descriptor read from there -- a pointer and a size, checked against
each other -- is an arbitrary write of a known constant, triggered by burning
CPU. So with the guest kernel on the signal handler reads nothing from the
entry. Registration runs inside the turn that instantiates the module, so it
is a hostcall (`hostcall::WASM_SIGNAL`): the turn submits the linear memory
by its offset within the isolate's arena and its length, and the field by its
offset within the memory, never a pointer, and the handler has gk validate
all of it against gk's own record of the arena the calling thread is running
in -- the arena is never taken from the turn -- and keep the field's absolute
address, arena and kind in gk's signal field table, which lives in the
supervisor control block beside the host slot table (`gk_signal_field_register`
in `gk.h`). The signal handler calls `gk_signal_field_write`, which finds the
calling thread's record by its kernel tid rather than through a thread-local,
takes the arena from that record, walks the arena's entries without a lock
and allocating nothing, and bounds-checks each address against the arena
again before writing. Mutation of an arena's entries happens only under the
isolate lock on the thread that has the arena active, which is the thread the
signal interrupts, and the entries are published and retired in an order a
walk interrupting the mutation sees as consistent. The heap entry keeps what
only the turn needs: the memory pin, the weak instance handle, and the
handles gk issued, which the GC filter and teardown use to unregister, and
which gk validates like host slot handles and accepts only from the owning
arena's thread. Whether the writer takes this path is decided by
`gk_initialized()`, a word in supervisor memory, not by a flag in the heap a
turn could flip to send the writer back to the list. What remains: the
write lands at the address the memory had when it was registered, so a
memory the isolate has since grown by relocation gets it at the old address
-- still inside that isolate's own arena, so the isolate at worst mislays its
own signal -- and a page the isolate has decommitted since faults the host's
write, a crash the isolate could cause by other means. `wasm-signal-test.sh`
shows a module at ring 3 observing the signal, and the signal landing in the
module's memory rather than in a buffer the turn pointed the heap descriptor
at.

### Other violations

3. **Guest-writable thread-locals, closed.** `threadLocalRequest`
   (`io-context.c++:29`), read by `IoContext::current()` from host-side
   destructors (`io-own.c++:10`), and `currentApi` (`worker.c++:557`), behind
   `Worker::Api::current()`, are saved on the host turn frame and restored when
   the guest returns (`GuestTurnThreadLocals`), with `Worker::Lock` taking
   `currentApi` from its stack rather than its heap `Impl` at release; the same
   frame covers the SQLite memory scope and the VFS scopes. `activeProgressCounter`
   (`thread-scopes.c++:27`), which the watchdog's signal handler dereferences
   mid-turn, is in gk's per-thread supervisor words instead. `currentVfsRoot`
   (`sqlite.c++:1860`) is bounded by gk's turn directory, which the
   wrapped-native VFS sets around each native call that may name a file (see
   R7). `guestTurnArena` (`guest-run.c++:35`) needs nothing: a rewrite only
   weakens a ring-3 diagnostic the host never acts on.
4. **Guest code takes host mutexes**, so corrupting a `kj::Mutex` word
   deadlocks the next host locker: `DeleteQueue::crossThreadDeleteQueue` from
   `~IoOwn` during in-arena GC (`io-own.c++:15`), `IsolateBase::queue` from
   `Data::destroy` (`setup.h:755`), the module registry on every lookup
   (`modules-new.h:801`), `CompileCache` (`compile-cache.h:54`), the actor
   cache LRU (`actor-cache.h:1084`).
5. **Guest-built KJ objects with vtables are consumed by the event loop.**
   `awaitIoImpl` builds promise nodes in-turn and hands them to `addTask`
   (`io-context.h:1600`); `IoContext::abort` stores a guest-built
   `kj::Exception` that the host later clones and throws
   (`io-context.c++:542`).
6. **Guest-written strings are freed by the host**: `Trace::logs`/`exceptions`
   (`worker.c++:262`, `:2436`), `Span::tags` (`trace.h:1250`),
   `warningOnceDescriptions` (`worker.c++:619`).
7. **H objects hold V8 handles and `jsg::Ref`s**: `IoContext`'s JsRefs
   (`io-context.h:1269`), `Worker::Actor::Impl` (`worker.c++:3797`),
   `TimeoutState::params.function` (`io-timers.h:79`),
   `HibernatableWebSocket::websocket` (`io/legacy-hibernation-manager.h:110`).
8. **The guest calls through host enforcement vtables**, and can swap the
   `limitEnforcer` `kj::Own` in `IoContext` (`io-context.h:1180`) for a stub.
   The CPU timer is host-driven, so this is a confinement gap rather than an
   escape, but `newSubrequest` and `getBufferingLimit` are enforced nowhere
   else.
9. **`jsg::WeakRef<T>::tryGet()` is documented for lock-free KJ-side use**
   (`jsg.h:1660`), which is the H-touches-I pattern the rubric forbids; same
   for `workerd::WeakRef<IoContext>::tryGet()` from `makeReentryCallbackImpl`
   (`io-context.h:1811`).

## Inventory: `api/`

Almost all of it falls out of four rules, on top of R1-R9 above.

| Rule | Test | Class |
|---|---|---|
| A | `JSG_RESOURCE_TYPE` (232 declarations across 70 headers) | **I**, by R1. The compliance sub-test is what produces the escapees: every member must itself be I, a handle, or a read-only pointer |
| B | `JSG_STRUCT` (201 declarations) | **I**. Translation is by value (`jsg/struct.h:7`), so the C++ instance exists only within a turn or as a field of an I object |
| C | Reached through `IoOwn<T>`/`IoPtr<T>`, or created by `IoContext::addObject` | The pointed-to `T` is **H**; the `IoOwn` becomes a handle. Roughly 60 sites, all consistent -- no work beyond the handle rewrite. The `kj::OneOf<uint, IoOwn<...>>` pattern in `Fetcher` (`api/http.h:575`), `DurableObjectNamespace` (`api/actor.h:321`), `KvNamespace` (`api/kv.h:291`) and `R2Bucket` (`api/r2-bucket.h:594`) is instructive: the `uint` arm is already a channel number the host resolves, which is exactly the target shape; the `IoOwn` arm is what has to change |
| D | Derives from `WorkerInterface::CustomEvent`, `ReadableStreamSource`, `WritableStreamSink`, `kj::AsyncInputStream`/`AsyncOutputStream`, `Fetcher::OutgoingFactory`, `rpc::*::Server` or `capnp::MembranePolicy` | **H**. These are documented as running without the lock: `ReadableStreamSource::tryReadSync` "must not acquire the isolate lock and must not enter JavaScript" (`api/streams/common.h:313`), and `newSystemStream` requires its inner stream to hold no JS references because "the stream is allowed to outlive the isolate" (`api/system-streams.h:21`) |
| E | No JSG macro, no `kj::Promise`, no `IoOwn`, no event-loop base class | **I**. URL/URLPattern, encoding, base64, form-data, the `CryptoKey::Impl` hierarchy, the zlib/brotli/zstd contexts, lol_html tokens. These hold C-library state through `kj::Own<EVP_PKEY>` and the like, which the host never touches -- so they become I only once BoringSSL's `OPENSSL_malloc`, zlib's `zalloc` (already hooked by `CompressionAllocator`, `api/compression.h:36`), brotli/zstd and lol_html's Rust allocator point at the arena. An allocator problem, not a class problem |

The escapees, condensed:

| Class | Split |
|---|---|
| `Fetcher` (`api/http.h:575`, `:326`) | I shell plus an H factory and an H `WorkerInterface` per subrequest, reached by handle. `fetch()` currently creates and drives the `kj::Own<WorkerInterface>` from inside the turn (`api/http.c++:1818`) |
| `SqlStorage`/`CachedStatement`/`Cursor::State` (`api/sql.h:96`, `:154`, `:249`) | The largest synchronous host-in-turn surface in `api/`: `CachedStatement` embeds a `SqliteDatabase::Statement` by value and a `SqliteDatabase&`, and `State` keeps bindings alive because the query "might contain pointers into strings and blobs" -- that is, into sandbox memory. I keeps `SqlStorage` and `Cursor` plus a statement handle; rows arrive through B |
| `WebSocket` (`api/web-socket.h:855`) | Already structurally split -- I shell versus `Native` holding `kj::Own<kj::WebSocket>` and a `kj::Canceler` -- except for the raw `AutoResponse&` cached beside its own `IoOwn` at `:904-905` |
| `DurableObjectNamespace` (`api/actor.h:322`) | I namespace plus the H `ActorIdFactory`, whose vtable is currently called in-turn; the factory's keyed operations are already a hostcall (`server/actor-id-impl.h`), so what the in-turn call reaches is the heap object that holds the key's slot handle, not the key |
| `DeferredProxy<T>` (`api/deferred-proxy.h:37`) | The `IoContext::run` result problem in its natural habitat: a promise node graph allocated in-turn and run by the host |
| `MemoryCache` (`api/memory-cache.h:526`) | I object plus a handle. `SharedMemoryCache` is `kj::AtomicRefcounted` with a `MutexGuarded` map and is shared across isolates by design (`:119`, `:477`) |
| `CapnpServer` (`api/capnp.h:36`) | An H capnp server holding a `jsg::V8Ref<v8::Object>` and a wrapper reference its own comment marks "only valid if isolate is locked" |
| `AbortSignal` registration cells (`api/basics.h:804`, `:887`) | The cells are `kj::Arc` + `MutexGuarded` and are taken from *other* IoContexts through `DeleteQueue::scheduleAction`; they become H objects the signal holds by handle |
| `Rewriter` (`api/html-rewriter.c++:214`) | An H `WritableStreamSink` holding `IoContext&`, the lol_html parser, and handler registrations that call back into JS |
| `jsg::ExternalMemoryAdjustment` (`api/actor.h:402`, `api/worker-rpc.h:645`) | Holds a `kj::Arc<const ExternalMemoryTarget>` with an `IsolateLiveness` weak ref touched outside the turn, so H objects hold per-isolate state and I objects hold an Arc shared with the GC thread |
| `PyodideBundleManager` (`api/pyodide/pyodide.h:33`) | H, process-wide and `MutexGuarded`; `ReadOnlyBuffer::source` is an `ArrayPtr` into a process-lifetime bundle read in-turn. Admissible only if those pages are mapped read-only into every arena |
| `HibernatableSocketParams` (`api/hibernation-event-params.h:14`) | B. Host-produced bytes plus a `kj::Exception` consumed in the turn -- the reverse of the turn outcome, and it needs the same bounded-bytes treatment |

### The cross-isolate work list

This is the set that faults under per-isolate arenas, and it matches what the
placement experiment hit.

- **JS RPC.** The caller builds a `MallocMessageBuilder` plus V8-serialized
  bytes under its own lock (`api/worker-rpc.h:186`, built at
  `worker-rpc.c++:745`); the callee's turn reads the params straight out of
  the call context, which its own comment describes as "a pointer into the RPC
  params reader" (`worker-rpc.c++:1527`), and deserializes from
  `reader.getV8Serialized()` (`:298`). Results go back the same way (`:1639`,
  `:1918`). Both directions are B message buffers. Whether Cap'n Proto's
  in-process path copies the message or lets the callee alias the caller's
  builder could not be settled from the sources available, so aliasing must be
  assumed.
- **Streams over RPC.** The receiver's `ExternalPusherImpl::pushByteStream`
  creates a `kj::newOneWayPipe` (`io/external-pusher.c++:150`) and
  `unwrapStream` short-circuits through `getLocalServer` to hand the
  receiver's turn the pipe's input end directly (`:165`, consumed at
  `api/streams/readable.c++:1109`). One KJ pipe is written by isolate A's pump
  and read by isolate B's: both ends H, each currently owned from inside an
  isolate's objects.
- **Service bindings.** `getSubrequestChannel(channel)` then
  `startRequest()` returns an H `kj::Own<WorkerInterface>`
  (`api/http.c++:2900`); the body is pumped with
  `jsBody.pumpTo(js, newSystemStream(nr.body))` (`:1820`) and arrives on the
  callee side wrapped by `newSystemStream` (`api/global-scope.c++:343`).
- **Service stubs.** `Fetcher::deserialize` does
  `IoContext::current().addObject(kj::addRef(channel))`
  (`api/http.c++:2466`), so one refcounted `SubrequestChannel` is owned by two
  IoContexts. The same mechanism serves `DurableObjectClass`, `KvNamespace`,
  `R2Bucket` and `WrappedBinding`.
- **AbortSignal over RPC**, **Durable Object replica storage** (which holds
  the primary's `kj::Own<ActorChannel>` directly in an I object,
  `api/actor-state.h:206`), **hibernatable WebSockets** (a
  `kj::Rc<kj::WebSocket>` shared with a manager that outlives the isolate,
  `api/web-socket.h:676`), the **shared memory cache** (isolate B's
  `jsg::Deserializer` reads bytes isolate A's turn allocated,
  `api/memory-cache.c++:440` then `:475`), **Pyodide bundles**, and
  **WorkerLoader** (the loading isolate's module sources kept alive for the
  new isolate by `OwnContentWrapper`, `api/worker-loader.c++:112`, with a
  `CompiledWasmModule` shared through V8's process-wide Wasm engine).

Not cross-isolate, despite appearances: a stream `tee()`'s shared
`kj::Rc<Entry>` (`api/streams/queue.h:739`) and `MessagePort`
(`api/messagechannel.h:27`) stay within one isolate.

### Binary buffers

The governing mechanism is `jsg::asBytes`: for an ordinary ArrayBuffer it
returns `bytes.attach(kj::mv(backing))` (`jsg/util.c++:688`), an aliasing
`kj::Array` over the V8 backing store, deep-copying only resizable or
immutable buffers. So **every `kj::Array<byte>` argument in `api/` is a raw
pointer into the sandbox**, which is arena memory. In the other direction,
wrapping adopts the array only if it already lies inside the sandbox and
otherwise copies into a fresh backing store (`jsg/value.h:1037`).

Because of that, nearly every crossing already copies, and the arena changes
not whether but *where*: an in-turn `kj::heapArray` becomes arena memory the
host cannot read. That is the mechanism behind the faults the placement
experiment produced. The actionable lists are therefore short and specific.

Allocated in-turn, consumed by a host pump, sink or RPC -- must come from B
with a descriptor: `api/web-socket.c++:891` (the send path, whose comment
already says "data aliases the V8 BackingStore ... Copy while we still hold
the lock"), `api/streams/internal.c++:1366`, `api/js-readable-stream.h:50`
(whose comment says the bytes "must not sit in the V8 sandbox"),
`api/worker-rpc.h:187`, `api/memory-cache.c++:440`.

Filled by the host and read in-turn -- must become a B descriptor the isolate
bounds-checks and copies from: `api/streams/internal.c++:717`,
`api/js-readable-stream.h:496`, `api/kv.c++:463`, `api/queue.h:179`,
`api/hibernation-event-params.h:22`, `api/worker-rpc.c++:298`,
`api/memory-cache.c++:475`.

One case resists both: `api/sql.h:251` holds raw pointers into sandbox strings
and blobs as SQLite query bindings, so the host reads sandbox memory while
stepping the query, and `:255` reads rows back out of SQLite's result memory
in-turn.

## Inventory: `util/`

The common case is rule 4: a leaf whose instances take their owner's class and
need no annotation. That covers the value types and header templates --
`mimetype.h`, `strings.h`, `strong-bool.h`, `uuid.h`'s `UUID`, `string-buffer.h`,
`state-machine.h`, `ring-buffer.h`, `small-weak-vector.h` and the rest. The
`constexpr` tables in `strings.h:16` and `header-validation.h` are the ideal
shape for an I-readable dependency: rodata, so every arena can map them
read-only and no crossing is needed.

The exceptions are the headers holding process-wide or thread-wide mutable
state. These are H by rule 3, and a turn reads or writes them today:

| State | Where | Why it is H, and what a guest write does |
|---|---|---|
| `globalAutogate`, a `bool[22]` | `autogate.c++:17`, read by `isEnabled` `autogate.c++:94` | Read on hot paths from a dozen `api/` files inside turns. A write flips code paths for every isolate in the process; `COMPRESSION_RS` (`autogate.h:125`) routes all zlib work. Cheap fix: the bitmap is 22 bytes, so copy it into each arena at isolate creation and have in-turn code read the arena copy |
| `std::atomic<bool> useZlibRs` | `zlib-router.c++` | Consulted on every zlib call a turn makes; a write switches implementation process-wide mid-stream |
| `ThreadProgressCounter* activeProgressCounter` | `thread-scopes.c++:27` | Read by the watchdog's signal handler while the turn runs, so it cannot use technique A (a host-frame save restores only after the turn): a pointer the handler loaded from TLS, which the turn can write, would be a pointer of the turn's choosing dereferenced with host privilege. **Technique B, done.** With the guest kernel on, the active counter's identity, the address of its `uint64_t` and the saved value live in gk's per-thread supervisor words (`gk_thread_words_set`/`gk_thread_words_get`, three keys the file owns), which ring 3 cannot write; `hasProgress()`/`acknowledgeProgress()` and the handler read them there. A counter constructed inside a turn is always a reentrant no-op (the turn's own lock holds the outer one). Without the guest kernel the thread-local pointer stays. `thread-scopes.h` describes it; `gk_test.c`'s `thread_word_check` shows the words set host-side, read from a `SIGUSR1` handler interrupting a ring-3 turn, and unreachable from ring 3 |
| `multiTenantProcess`, `predictableMode`, `gcStressMode` | `thread-scopes.c++:20` | File-scope, read from turns |
| `thread_local BufferState state`, 4 KiB of unconsumed CSPRNG output | `entropy.c++`, behind `getEntropy()` | The buffer is on the thread the turn runs on, so a turn can read the bytes a later draw on that thread returns, or choose them. **Split by caller, not moved.** The host-privileged draws -- the channel token key at startup and the nonce of every RPC token (`channel-token.c++`) -- use `getHostEntropy()` (`host-entropy.h`) instead: its buffer, cursor and lock are one H block reached through a host slot handle, and it refills from the operating system's generator rather than from BoringSSL's in-process DRBG, whose per-thread state is in the shared heap as well and, on a CPU without a fast RDRAND, determines the DRBG's output outright. So neither the bytes a token nonce is drawn from nor the generator state behind them is in memory a guest can reach; `host-memory-test.sh` shows a turn's overwrite of the thread-local buffer reaching the host's next `getEntropy()` draw and not the nonce. `getEntropy()` keeps its thread-local buffer for the draws a turn makes for itself (`crypto.getRandomValues`, multipart boundaries, hyperdrive suffixes, retry seeds), whose bytes go to the isolate anyway, so their confidentiality from it is meaningless and a crossing would buy nothing. The one in-turn draw whose value is *not* only for that turn, the 16 random bytes of a `newUniqueId()` actor id, is no longer made from it: the id is minted by the actor-id hostcall (`server/actor-id-impl.c++`), host-side, and the handler draws the random half from `getHostEntropy()` in the same call that HMACs it, so neither half of an id is a turn's to read or choose. `sentry.c++`'s internal error ids are the remaining in-turn draw whose value leaves the turn; they correlate a log line with a user-visible message and are not a secret, so a chosen id can at worst mislabel a log line |
| `thread_local int currentVfsRoot` | `sqlite.c++:1860` | It is the directory descriptor for the `openat`, `newfstatat`, `unlinkat`, `faccessat`, `mkdirat` and `readlinkat` that SQLite's wrapped-native VFS issues (`sqlite.c++:1867-1893`), all on the syscall allowlist, so a rewrite would redirect where SQLite's file operations land -- the one thread-local with filesystem reach. It cannot use technique A, because the dirfd is consumed by a forwarded syscall mid-turn. **Technique B, done.** gk's turn directory (`gk_confine_paths`/`gk_turn_dir_set`, gk.h) bounds a turn's forwarded path syscalls to a directory the host set: `openat` runs as `openat2` with `RESOLVE_BENEATH`, a stat as an `O_PATH` open under the same rules, and the name-acting syscalls against their parent opened the same way, so a rewritten dirfd is refused and a lookup cannot leave the directory. The wrapped-native VFS sets the turn directory around each native call that may name a file (`NativeVfsCall`, `sqlite.c++:1941`), through the VFS root hostcall (`hostcall::VFS_ROOT`) whose handler takes the descriptor from a host slot (`Vfs::RootDir`, `sqlite.c++:1850`) named by the handle the turn passes, never from the turn's argument. `gk_test.c`'s `turn_dir_check` shows the bound at the syscall level, its negative build shows file operations landing at a guest-chosen directory once the confinement is removed, and `vfs-root-test.sh` shows a turn's rewrite of `currentVfsRoot` leaving a Durable Object's open refused and its files in the data directory, and the same rewrite landing the database in the chosen directory with the confinement off |
| `SqliteObserver DEFAULT` with a mutable `dbWalSize`; `vfsErrorListener` | `sqlite.h:55`, `sqlite.c++:196` | File-scope and thread-local mutable state on a path a turn drives synchronously |
| `static kj::NullStream nullStream` | `stream-utils.c++:250` | A file-scope object with a vtable, handed out by reference and by `kj::Own` with a null disposer. Must be rodata or supervisor-mapped |
| `static const MimeType PLAINTEXT` | `mimetype.h:115` | `const`, but its `kj::String` members point at shared-heap buffers that `const` does not protect. The `ConstMimeType` forms below it (`mimetype.h:117`) are the right shape |
| `LOG_ONCE` / `LOG_PERIODICALLY` statics | `sentry.h:65`, `:74`, `:86` | Function-local `static bool`s in code that runs at ring 3. Low severity -- a write suppresses or unsuppresses a log line -- but they are H data scattered through I-class code |

The cross-thread primitives are H by construction and need no further argument:
`xthreadnotifier.h:23`, `sync-wait-list.h:65`, `wait-list.h:60` (an
`EventLoopLocal`), and `AtomicWeakRef` (`weak-refs.h:21`).

`SqliteDatabase` is a **split**, and the largest one in `util/`. SQLite runs
synchronously inside the turn (`api/sql.c++:47`, `:118`). The database, `Vfs`,
`LockManager` and `Lock` (`sqlite.h:833-1040`, holding a `kj::Directory&`, a
`sqlite3_vfs&` and file descriptors) are H; `Statement` and `Query`
(`sqlite.h:483`, `:547`) are the per-call objects a JS cursor walks. The
in-turn side holds a statement handle and receives rows through B. SQLite's
own global configuration, page cache and allocator hook
(`sqlite-metering.c++:133`) cannot live in an arena, so queries execute
host-side.

## Inventory: `server/`

The rule is that `Server` and everything nested in it is H, and it holds: the
sockets, TLS contexts, `kj::Network&`, `kj::Filesystem&`, timer and entropy
source (`server.h:140-146`), the service map (`server.h:208`), the actor
namespaces and the `ChannelTokenHandler` (`server.h:165`). None of it is
referenced from a V8 wrapper; it is reached through the `IoChannelFactory` and
`TimerChannel` interfaces that `WorkerService` implements
(`server.c++:3450`). `workerd.c++`'s `CliMain`, the config loading and the gk
bring-up all run before any isolate exists.

What a turn nevertheless reaches, every path starting at
`IoContext::getIoChannelFactory()` (`io-context.h:1165`):

| Reached | Path | Class |
|---|---|---|
| `WorkerService::ioChannels` | `Fetcher::fetch` -> `startSubrequest(channel, ...)` (`server.c++:4269`), `getCache()` (`:4364`), `getActorChannel` (`:4440`), `getRpcChannel` (`:4490`) -- host arrays indexed by a guest-supplied channel number, then dispatched through another service's vtable | H. The `uint channel` a JS `Fetcher` carries (`api/http.h:213`) is already the right shape: an index the host validates. What is wrong is that the validation and dispatch run inside the turn |
| `Server&` itself | `workerdDebugPortServer` (`server.c++:3466`) -> `getWorkerdDebugPort()` (`:4553`) | H. A worker with that binding reaches the `Server` object by pointer from inside its turn |
| `ChannelTokenHandler::tokenKey` | a `HostSlot<kj::FixedArray<byte, 32>>` (`channel-token.h`), reached from `getTokenMaybeSync` (`server.c++`) when a `Fetcher` is serialized over JS RPC, in-turn, and from `*FromToken` when one is deserialized | **In H memory, reached by handle; the crossing is interim.** The key bytes are in host memory, so an escaped isolate cannot read the key that mints subrequest-channel tokens, and the handler, which is in the shared heap, holds only a host slot handle to them: a write primitive that rewrites the handle makes the host's next use resolve to nothing and throw, rather than encrypt or decrypt with a key at an address the guest chose. What it can still do is rewrite the handle to another live host slot's, the ownership residual above; with one tenant there is no other slot to name. The two functions that read the key run on the event loop, outside the guest, when the guest kernel is on: a turn that mints an RPC token gets a `kj::Promise` for the bytes, and one that decodes a token gets a promised channel, both forms the callers already handle. That is a crossing by way of a guest-built promise (violation 5 below), not yet the B message the turn should submit, which the final split removes by moving the handler's host half to H. The key and the nonce of every token are drawn with `getHostEntropy()`, from a buffer in H memory, not from the thread-local one above |
| `ActorIdFactoryImpl::key`, a SHA-256-sized HMAC key | a `HostSlot<Key>` (`actor-id-impl.h`) in a refcounted holder the factory owns, the factory itself owned by the JS-visible `DurableObjectNamespace` as a `kj::Own` (`api/actor.h:322`); `newUniqueId()`, `idFromName()` and `idFromString()` are called in-turn | **In H memory, reached by a hostcall.** The key bytes are in host memory, tagged, so an escaped isolate cannot read the key that mints and validates its namespace's ids, and the factory, in the shared heap, holds only a host slot handle to them. Every keyed operation is the actor-id hostcall (`hostcall::ACTOR_ID`): the turn puts the operation, the handle and the bytes in the exchange frame, the handler resolves the handle through gk's table, checks the tag, and HMACs host-side; a unique id's random half is drawn there from `getHostEntropy()`; a name longer than a frame crosses in chunks with the running HMAC in the frame's host-side state. A write primitive that rewrites the handle makes the handler refuse the call (a handle that resolves to nothing, or to a slot without the tag) rather than HMAC with bytes at an address the turn chose; what it can still do is rewrite the handle to another namespace's live key handle and mint that namespace's ids, the ownership residual, and it can still swap the factory's vtable, since the factory is a heap object the turn calls through. Without the guest kernel the same handler is called directly |
| `CompatibilityFlags::Reader` | `WorkerdApi::getFeatureFlags()` (`workerd-api.c++:308`), called from nearly every API entry point during a turn; a capnp `Reader` is a pointer into a host-owned message | I only by copying: snapshot the struct into the arena at isolate creation. `jsg::JsgConfig` (`workerd-api.c++:228`) is already such a by-value copy |
| `EntropySourceImpl` | `IoContext::getEntropySource()` (`io-context.h:926`) from `crypto.getRandomValues` (`api/crypto/crypto.c++:692`) and key generation | H; in-turn callers take bytes through B |
| `static Frankenvalue EMPTY_PROPS` | `server.c++:3550`, a mutable file-scope object passed by non-const reference | H data reached in-turn. Fixable by not using a static |
| `AlarmScheduler`, `ContainerClient`, `FallbackServiceClient` | `storage.setAlarm()` (`server.c++:1767`); `api/container.c++` through a capnp client; module resolution blocking on a thread with a mutex (`fallback-service.h:46`) | H |
| `WorkerdPlatform` | V8 calls `PostTaskOnWorkerThreadImpl` from inside the turn (`v8-platform-impl.h:5`) | H, but invoked through its vtable from ring 3 |

One structural finding that is not a classification: **not all JS runs in the
guest.** There are exactly two `jsg::runInGuest` call sites, `runJsTurn`
(`io-context.c++:1443`) and top-level execution (`worker.c++:2204`). The
inspector runs user JS outside both: `dispatchProtocolMessage`
(`worker.c++:3171`) and the microtask drain after it execute under the isolate
lock with no guest wrap, so with `--inspector-addr` an isolate's JS runs with
full host privilege. `hibernateWebSockets` (`server.c++:1076`, `:1127`) runs
V8 code under the lock outside the guest as well, and bindings and globals
instantiation is deliberately outside (`worker.c++:2084`, comment at `:2039`).
A plan that relies on every JS turn being confined has to wrap these paths or
refuse the inspector when gk is on.

## Inventory: KJ and Cap'n Proto types that cross

These are not workerd's classes, but a turn's C++ bindings touch them
constantly, and most of them cannot be given a class as they stand.

| Type | Why it resists classification | Minimum change |
|---|---|---|
| `kj::Own<T>` | A disposer pointer plus an object pointer. Across the boundary the disposer is an attacker-controlled indirect call. `IoOwn<T>` is worse: a raw pointer into host memory plus a refcount on a host `AtomicRefcounted` (`io-own.h:278`, `:89`) | `IoOwn::item` becomes an index into a per-isolate list the host bounds- and owner-checks; `deleteQueue` becomes an arena id rather than an `Arc`. I-internal owns should use a disposer baked into the type so no disposer pointer exists in guest memory |
| `kj::Promise<T>` and its nodes | One heap object owned by the host `kj::TaskSet` (`io-context.h:1245`), carrying a vtable and event-loop queue links the guest wrote, and embedding guest state (a `jsg::Promise::Resolver`) the host must not touch | The turn emits a request record into B; the host builds the real promise on its side; on completion it writes `{resolver handle, result or exception record}` into B and schedules a turn. The body of the `run(...)` lambda at `io-context.h:1593` already does only that |
| `kj::Exception` | Owns strings with their own disposers, a context chain and a details array | B after a copy -- it is only data. `GuestTurnOutcome::setException` (`guest-run.c++:78`) already does this for the turn's own outcome; generalize it to carry the detail list, since the ids in `util/exception.h:4` are load-bearing for `decodeTunneledException`, and use it for every rejection that crosses |
| `kj::String`, `kj::Array<T>` | Pointer, size, and a disposer with a virtual `disposeImpl`. Worse, `jsg::asBytes` returns an `Array` aliasing a V8 backing store (`jsg/util.c++:655`), i.e. arena memory, which is then handed to host network code (`server.c++:4393`) | A B descriptor `{offset, length}` over copied bytes, or for large bodies a host-side read-only mapping of the arena range for the duration of the write |
| `kj::HttpHeaders` | Views into guest memory plus a reference to a host header table. Built in-turn (`api/http.c++:1150`) and read asynchronously by the host client (`:1818`) | Serialize to a flat `(name, value)*` block in B and let the host build its own `HttpHeaders` against its own table. `HttpOverCapnp` (`server.c++:249`) already defines such a wire form |
| `kj::AsyncInputStream` / `AsyncOutputStream` | Three faults at once: a vtable call from ring 3 into host code, the host writing into arena memory, and a promise node spanning both sides | A stream handle plus a ring: the turn posts `read(handle, max)`, the host reads into a B chunk and signals `{handle, offset, len, eof}`, the turn copies into its ArrayBuffer |
| `kj::EventLoop`, `UnixEventPort`, the timer set | Per-thread shared state, found through a thread-local, whose queue every in-turn `.then()` touches; `setTimeout` inserts into the host timer set (`io-context.c++:1020`, `server.c++:4600`) | H, unambiguously. `TimeoutManagerImpl` (`io-context.c++:990`) is already handle-shaped: make `setTimeoutImpl`/`clearTimeoutImpl` B messages and keep the `jsg::Function` callback in the arena, keyed by id. The general case disappears once in-turn code stops creating KJ promises |
| `capnp::MessageBuilder` / `MessageReader` | The live objects own segments with disposers and an arena with a vtable -- but the *encoded message* is pointer-free, using segment-relative offsets | The only type on this list that already has the right wire form. Allocate segments in B with a `MessageBuilder` whose `allocateSegment` carves from the shared region; each side builds its own Reader or Builder over those bytes, with a traversal limit, since B is untrusted. Nothing with a vtable crosses |

## What the classification implies for the order of work

Three conclusions follow from the inventory rather than from the rubric.

**The faults a per-isolate heap arena produces are not a sign the scheme is
wrong.** They are the classification being violated, and each one names the
violation. A service binding faults the callee on the caller's request objects
because the caller allocated them in its turn, so they went to the caller's
arena; two workers using `Intl` fault because whichever ran first placed the
ICU data in its arena; two using `setTimeout` fault because whichever grew the
thread's timer set placed a node in its arena. In rubric terms, all three are
H or B data that landed in I memory because placement follows the allocator
rather than the class.

**So placement cannot be driven by the thread's current arena alone.** An
isolate-wide switch for the duration of the lock assigns I to everything a
turn allocates, including the H and B parts. The allocator needs a per-site
decision: an explicit shared-heap scope for allocations the classification
calls H or B, process-wide state initialized eagerly or routed to the shared
heap by its own allocator hook (ICU has `u_setMemoryFunctions` for exactly
this), and the per-isolate arena as the default for everything else.

**The cheapest crossings to build first are the ones whose data is already
copied.** The binary-buffer inventory shows the runtime already copies at
almost every boundary, because of the V8 sandbox and MPK rules; what the
arena changes is only the destination of the copy. Redirecting those copies
into B is mechanical and needs no protocol design. The expensive items -- the
`kj::Promise` channel, the `IoOwn` handle table, the SQLite split -- are where
the design work actually is.

A reasonable order: redirect the already-copying boundaries into B; make
placement per-site so the arena stops capturing H data; convert `IoOwn` to a
handle, which retires the largest single family of raw cross-boundary
pointers; then the promise channel, which retires the `IoContext::run` result
problem and the event-loop touches together.

## What is in H memory

H memory exists: `gk_host_alloc` with `HostOwn<T>` over it, and the host slot
table with `HostSlot<T>` over that (see "Host (H)" above). Its tenants, and
what is waiting on the same facility:

- **In H memory, or in gk's supervisor control block beside the host slot
  table:** `ChannelTokenHandler::tokenKey` (`server/channel-token.h`),
  held through a host slot handle, with the key's two readers deferred to the
  event loop under the guest kernel; the host entropy buffer
  (`util/host-entropy.c++`), one block holding the unconsumed system-generator
  output, its cursor and the lock that serializes draws, held through a host
  slot handle in a function-local static, from which the token key, every
  token nonce and the random half of every unique actor id are drawn; and
  `ActorIdFactoryImpl::key` (`server/actor-id-impl.h`), held through a host
  slot handle in a refcounted holder, with its readers -- synchronous to JS
  -- reaching it through the actor-id hostcall. `host-memory-test.sh` shows a
  ring-3 read of the first two faulting, a handle rewritten from inside a
  turn resolving to nothing, with the host refusing the use, and a turn's
  overwrite of the thread-local entropy buffer reaching the host's next
  `getEntropy()` draw but not a token's nonce; `actor-id-test.sh` shows a
  ring-3 read of the key faulting while the ids turns mint through the
  hostcall are distinct, derive from the configured key and address working
  Durable Objects. The Wasm signal descriptors (`io/tracked-wasm-instance.h`)
  are gk's signal field table, keyed by arena, filled through the
  `WASM_SIGNAL` hostcall and read by the signal handler through gk's record
  of the calling thread (see "The Wasm signal descriptor, closed" above);
  `wasm-signal-test.sh` shows the signal reaching a module at ring 3 and not
  reaching a buffer the turn aimed the heap entry at. This is the first
  host-side table a signal handler reads, and the pattern the thread-local
  cases below need: the handler finds its state through gk's per-thread
  supervisor record -- the thread's record by kernel tid, the arena from the
  record -- and never through a thread-local.
- **In gk's supervisor memory, found by thread:** the watchdog progress
  counter's state (`thread-scopes.c++:27`), in gk's per-thread supervisor
  words (`gk_thread_words_*`), read by the watchdog signal handler through
  gk's record of the thread rather than through a thread-local, as the signal
  field write is. The thread-locals the host reads only after a turn
  (`threadLocalRequest`, `currentApi`, the SQLite and VFS scopes) need no
  supervisor memory: they are saved on the host turn frame and restored when
  the guest returns (`GuestTurnThreadLocals`, violation 3), and so are the
  isolate's jsg data slots, the lock slot among them (violation 1), by the
  frame that enters the guest. What that leaves for the lock slot is the
  write from another thread, which no host-frame save catches and which H
  memory would: a slot in H memory, keyed by isolate and read by an in-turn
  `Lock::from` through a crossing, is the shape of that further step, and it
  is the shared-heap limitation in general rather than this slot's own.
- **Not addressed by H memory alone:** the pointer to an H object is as safe
  as the memory it sits in. A `HostOwn` in a shared-heap object protects the
  object's bytes and lets the guest kernel refuse a swapped pointer at free
  time, but the host still dereferences whatever pointer is there, which is
  why an owner outside H memory holds a `HostSlot` instead: the host resolves
  the handle against gk's table on every use, so a rewritten handle names
  nothing or, at worst, another live tenant, never an address of the guest's
  choosing. That residual, and the fact that the object's owner is still
  reachable for rewriting at all, is what the splits in the inventories above
  are for: the chain from the event loop down to each H object has to be H
  all the way.
