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
can get to run; the one SUPER tenant outside gk's control block is the ring-0
resolve table (`gk_resolve` in `gk.h`), the handle-to-pointer lookup a ring-3
turn makes on every dereference of an `IoOwn` (`io/io-own.h`), which has to
be served inside the guest and so cannot be REFUSE, and which holds pointers
rather than the objects they name. What backs H today is `gk_host_alloc` (`gk.h`): regions mapped by the
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

**Nothing a turn reads can be H.** The sentence above -- ring 3 can neither
read nor write H memory -- is the class's whole value and also the one
constraint that decides most placement arguments before they start. A
`GK_PROT_REFUSE` range is in no guest root at all, so a ring-3 read of it
faults exactly as a read of another isolate's arena does, and the host owning
the memory changes nothing: the fault is the page-table walk, not a check the
host could waive. `GK_PROT_SUPER` relaxes this only for ring 0, which is gk's
own handler text and the resolve lookup, never embedder code. So H is
available for state the host alone touches, and for nothing a turn
dereferences, however indirectly -- and "a turn reads it" has to be answered
about the code the turn runs through, not about the code the runtime wrote.
This is why placing an `IoOwn`'s `T` in host memory is blocked (23 of the 36
classes are read at ring 3 after the turn that created them, below), and it
is the same rule that decides every piece of process-wide third-party state:
V8's wasm type canonicalizer and its JIT-page map are both read at ring 3
inside a turn, so for them the choice is a heap every arena maps or a
per-isolate copy, and host memory is not on the list. When a placement
argument reaches "put it in H", the first question is which ring reads it.

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

A table validates the handle, not what the entry holds, and the resolve
table (`gk_resolve` in `gk.h`) shows how far apart those are. Its entries
are filled from the turn's journal: `registerResolve` takes the pointer and
the type word from there, and `gk_resolve_commit` admits any pointer that is
memory the entry's owner can reach -- outside gk's ranges and outside every
arena but the turn's own -- with no test of the type at all. So a turn can mint a live entry, owned by its own arena and
tagged as whatever class it likes, naming an address of its choosing, and a
host-side `resolveOfType` asking for that class answers with that address. A
handle that passes the owner and class comparisons says that this isolate
registered something and called it that. It does not say that the address is
an object, or an object of that class, or an allocation at all -- so a
pointer does not become H-provenance by being carried as a handle, which is
what the rule above is about.

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

This is what `IoOwn` (`src/workerd/io/io-own.h`) does: it holds a
`hostMemory::ResolveHandle` naming the object the context's list owns, not a
pointer to it, and every dereference, destruction and release resolves the
handle against `gk_resolve`'s table (`gk.h`) -- liveness and generation,
nothing read through the handle's own value -- so a rewritten handle fails
the use rather than redirecting it. A ring-3 turn resolves through a ring 3
to ring 0 trap inside the guest, with no VM exit, so the check costs a
dereference tens of nanoseconds rather than microseconds; the table is
written by the host alone, so the entries a turn's objects need are reserved
before the turn and committed after it from the turn's journal, with the
pointer checked on commit against gk's protected ranges and against every
arena but the one the turn ran under (see
Cost, and `ResolveTurn` in `util/host-memory.h`). The table entry holds the
object's pointer, which is what the turn resolves; the host's own record of
the object -- the owned-list node, with the links its teardown walk and its
delete-queue drain follow and the disposer it calls -- is in H memory,
reached from the entry's companion (`hostMemory::resolveCompanion`, a
host-side record per entry) and built by the host after the turn from the
words the turn journaled with the registration. The context the object
belongs to is named the same way: the `IoOwn`'s other word is the handle of
the context's own entry (`DeleteQueue`), compared with the current context's
on every use, and the context's delete queue -- the handles of holders
dropped off their context, the actions other contexts send it, the signal
that wakes it, and the lock over them -- is in the context's record in H
memory behind that entry's companion, together with the owned list's head,
so no refcount and no mutex of the queue's is in memory a turn can write.
A turn that drops a holder of another context's object, or sends another
context an action, notes it in the turn's journal (`hostMemory::noteInTurn`)
for the host to queue after the turn, having first asked the ring-0 table
whether the context still exists; the host validates the note -- the
context must be live, and the handle must name an object of that context's
own list -- before it queues anything. What the handle and the node do not
protect is the object: the `T` is still in the shared heap, and the
disposer the node records is the one the turn journaled.

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
   What the sites actually are is counted rather than derived, in "Placing the
   shared heap per isolate" below, which also says why this layer's enforcement
   is a run-time counter and not the static check.
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
Two crossings exist. The hostcall is a VM exit: **6.4 us per round trip,
measured** (`hostcall_check` in `gk_test.c`), which is affordable for an
operation a turn makes a few times (an actor id) and not for the bulk of its
crossings. The ring-0 resolve (`gk_resolve`) is a ring 3 to ring 0 transition
inside the guest, not an exit: **27.7 ns per round trip, measured**
(`resolve_check` in `gk_test.c`, which proves from KVM's per-vCPU exit
counter that the resolves ran with no exit), 240 times cheaper than the
hostcall and in line with the 22 ns the bare trap measured in the benchmark
suite (`m10_ringtrap`), where the explicit trap was also 9.5 times cheaper
than servicing a demand fault, which is why the interface is a call into
ring 0 rather than fault-and-service. It is the one in-guest ring-0 helper,
and its table is the one tenant of the SUPER class of H memory.

What the resolve serves is the `IoOwn` dereference, and how many a turn
makes decides whether resolving each one is affordable. Counted with an
atomic counter in each `operator->` and a per-turn log line around
`runJsTurn`, over 1000 requests each, on the Zen 5 test machine: a
hello-world request is one turn with **6 `IoOwn` dereferences** and no
`IoPtr`; the Wasm/WebCrypto/irregexp surface worker (`surface-test.capnp`)
and a request that instantiates 1000 Wasm modules are also 6 each; a SQLite
Durable Object `/get` through a service binding (`disk-sqlite-test.capnp`)
is 3 turns with 15 `IoOwn` and 1 `IoPtr`, and a `/put` 4 turns with 25 and
2. A `ReverseIoOwn` is dereferenced in a turn too, though rarely: the
functor `IoContext::makeReentryCallbackImpl` holds is read inside the
`ctx.run` it wraps, and over the test suite the three `ReverseIoOwn`
classes that appear are read in a later turn three times against 48
registrations. At 27.7 ns each that is
0.1 to 0.5 us per request, against a hello-world request latency of about
620 us under the guest kernel (p50 over one connection on the same machine):
the dereference is not where a turn's time goes. (The ~43 foreign accesses a
hello turn makes, measured by `GK_TRACE_FOREIGN`, are mostly the allocator's
global state, which per-isolate heap arenas remove rather than convert into
crossings, so that count is an upper bound the placement work reduces
first.)

Registration is the other half of the cost, and it is where the design has
to bend: `IoContext::addObject` runs inside the turn, at ring 3, and the
table is written only by the host, so each registration would otherwise be a
hostcall. Counted the same way (a counter on `addObject`, over 2000 requests,
one connection): a hello-world request registers **10** objects, an
in-memory Durable Object `put` 28, and a request that makes one subrequest
through a service binding 44 -- at 6.4 us an exit, 7 to 15 percent of the
request. So the host reserves entries for the thread before the turn and
commits them after it (`ResolveTurn`, `util/host-memory.h`): a pool of 256
reserved entries per thread, topped up at turn entry when below 64, a
journal the turn fills with `{handle, pointer}` by position, and a host-side
commit after the guest returns that validates each pointer (`gk_resolve_commit`,
which admits only memory the entry's owner can reach: outside gk's protected
ranges, and outside every arena but the one the turn ran under) and retires
what the turn released. A turn that outruns its
pool refills it through one hostcall per 256 registrations.

The node behind each entry is the other cost of a registration: a 40-byte
node in a 64-byte block from `gk_host_alloc`, allocated by the host at the
commit and freed
host-side when the holder releases it, the drain reaches it or the list is
torn down, with a 16-byte companion record per entry in chunks mapped as
entries come into use. The holder grows by the disposal record it carries
for an in-turn drop (`IoOwn` and `ReverseIoOwn` are 32 bytes rather than
16). A context torn down inside another context's turn pays one hostcall
per 256 objects in its list, since the turn cannot walk the nodes; that
teardown is the rare case (a request's context ends on the event loop), and
the common paths cost no crossing beyond the resolve. Hello-world p50 and
the SQLite Durable Object `/put` p50 under the guest kernel are unchanged
within noise by the node placement: 649, 611 and 632 us against 610, 613
and 614 us for hello-world, and 2046, 1948 and 1955 us against 1965, 1965
and 1945 us for `/put`, three interleaved rounds of `oha` on one connection
for 10 s each, before and after, on the test machine.

How many entries a process needs at once is the sizing question the table's
capacity answers. Measured with a counter on the context's owned-object list
(registered on `addObject`, released on unlink), reported every 500 ms, under
one kind of load at a time on the test machine: a plain worker holds
**5 objects per in-flight request** parked on a timer (4000 such requests,
20000 entries) and 4 per request streaming a body; a worker awaiting one
subrequest through a service binding holds 10 per request and 20 for three;
an open WebSocket on a plain worker holds **8**; hello-world under `oha` at
1024 connections peaks at 1024. Nothing is held between requests: the count
returns to 0 after each load. So the live count is the in-flight concurrency
times a small constant, and the original 2^16 entries were exhausted by one
worker with 3300 parked requests; the table holds 2^20 (`GK_RESOLVE_SLOTS`),
about 200000 such requests, in 32 MiB of static memory that costs nothing
until written (an entry is 64 bytes: the pointer, the generation, the owner,
the chain word that puts the entry on its arena's list and the type word,
with the shift the trampoline indexes by and one cache line per entry
deciding the rest), with the reserved pools taking 256 per thread of it. The
table stays process-wide: a per-isolate table would move the same sizing
question to each isolate, and the ring-0 lookup needs no per-isolate table
to tell the isolates apart -- each entry records the page-table root it was
registered under, which the lookup compares against CR3. Exhaustion is a
refused registration, which fails the request that made it, never a fault.

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
| R7 | `thread_local`, or a function-local `static` with mutable state | Reachable by the guest, so a value the host trusts after a turn, or that a signal handler or a forwarded syscall consumes mid-turn, must not come from it. Two remedies are in use. A value the host reads only *after* the turn is saved on the host stack frame that runs the turn and put back when the guest returns (the frame is read-only to the guest under the host-frame wall): `GuestTurnThreadLocals` (`io/guest-turn-thread-locals.h`) does this for `threadLocalRequest` (`io-context.c++:29`), `currentApi` (`worker.c++:557`, with `Worker::Lock` also restoring it from the stack rather than its heap `Impl` at release), `tmpDirStorageScope` (`worker-fs.c++:57`), `symbolicLinkGuard` (`worker-fs.c++:53`), `SqliteMemoryScope::threadLocalScope` (`sqlite-metering.c++:36`) and the `AllowV8BackgroundThreads`/`WarnAboutIsolateLock` scope depths (`thread-scopes.c++`); `guestTurnDepth` (`guest-run.c++:22`) was the first. A value consumed *during* the turn with host privilege is kept out of the guest's reach instead: `activeProgressCounter` (`thread-scopes.c++:27`), read by the watchdog's signal handler, lives in gk's per-thread supervisor words (`gk_thread_words_*`), and `currentVfsRoot` (`sqlite.c++:1860`), the dirfd of SQLite's forwarded path syscalls, is bounded by gk's turn directory (`gk_confine_paths`/`gk_turn_dir_set`), which the wrapped-native VFS sets around each native call that may name a file through a hostcall whose handler reads the descriptor from a host slot. `guestTurnArena` (`guest-run.c++:35`) needs neither: a rewrite only weakens a diagnostic the guest draws at ring 3, and the host never dereferences it. `vfsErrorListener` (`sqlite.c++:196`) needs neither: `SqliteCallScope` clears it unconditionally and it is consumed only at ring 3 within the turn. Still open: `inCppgcShimDestructor` (`wrappable.c++:29`) and `AsyncLockQueue::threadCurrentWaiter` (`io/async-lock-scheduler.h:159`). `CompileCache::get()`'s static (`jsg/compile-cache.h:47`) is not reached at all: nothing calls the class (violation 10f) |
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
| `IoOwn`/`IoPtr`/`ReverseIoOwn` (`io-own.h`) | **All three hold handles, and the owned list and the delete queue are H: done.** `IoOwn`'s I side is `{DeleteQueue, hostMemory::ResolveHandle, OwnedDisposal}`, where `DeleteQueue` is the handle of the context's own entry and nothing else: the object's handle names the object through `gk_resolve`'s table, resolved on every dereference, destruction and release, registered by `addObject` through the turn's reserved batch; the context's handle is compared with the current context's on every use and resolved by the host when the holder is dropped off its context; the disposal record is the turn's own way of disposing of the object when it drops the holder inside a turn, which the host never reads. The context's record of the object, `OwnedObjectList`'s node -- links, disposer and the context's handle -- is in H memory, reached from the entry's companion, built by the host at commit and freed by the host on release, teardown or drain; the list's head is in the context's record (below), and the context keeps only its handle. `ReverseIoOwn` has the same shape with the validity token in place of the context handle: that token is still a `kj::Arc` to an `AtomicRefcounted` in the shared heap, whose refcount a turn that drops a `ReverseIoOwn` writes (the one refcount of this family still in guest reach; its holders are KJ I/O objects, dropped in turns only when the context's list disposes of them). `IoPtr` is `{DeleteQueue, ResolveHandle}` with an entry tied to its own lifetime and no node. What is still to split: the `T` behind every handle is in the shared heap, and the disposer the node records is journaled by the turn that created the object, so the host's disposal of a `T` is a call the turn chose, as `T`'s own vtable already is |
| `DeleteQueue` (`io-own.h`) | **H: done.** The queue is the context's record in H memory, behind the context's entry's companion (`DeleteQueueOwner`): the owned list's head, a `kj::MutexGuarded` over the deletion handles and the action boxes, in chunks from `gk_host_alloc`, and the cross-thread fulfiller. Holders name it by the entry's handle; the record lives as long as the context, and a stale handle resolves to nothing, which is the "defunct" answer. In a turn, `~IoOwn` off its context and `tryScheduleAction` write a journal note (`hostMemory::noteInTurn`) after a ring-0 check that the context's entry is live, and the host queues from the notes after the turn, validating each; outside a turn the host queues directly, on any thread, under a process-wide H registry lock (shared) that a teardown takes exclusive, which is what keeps a record alive for a pusher that has found it. The context drains host-side before entering the guest, and its signal task takes the actions out host-side and hands them to the turn that runs them. An action is a `kj::Function` boxed on the heap, which the host moves by pointer and never calls through; the turn that runs it, or the context's teardown, destroys it, and a host-side teardown or a note whose context ended before the turn did destroys it host-side, a call through the closure's own vtable, which is the same exposure as disposing of a `T`. A context torn down inside a turn tears its record down through the `IO_CONTEXT` hostcall, which hands the turn the fulfiller and the actions to destroy and retires the entry; the turn still destroys the fulfiller, whose destructor takes the event loop's executor lock, as it did before the record moved |
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
   deadlocks the next host locker. The `IoOwn` case is closed: the delete
   queue's lock, refcount and state are in the context's record in H memory
   (`DeleteQueueOwner`, `io-own.h`), a holder names the context by its
   entry's handle rather than by an `Arc`, and a holder dropped off its
   context during in-arena GC, or an action sent to another context from a
   turn, writes a journal note (`hostMemory::noteInTurn`) that the host
   validates and queues after the turn, so no in-arena code takes the lock
   or writes the refcount; `ioown-test.sh` shows a turn's write into the
   record faulting at the record, with the host's later lockers unaffected.
   What in-arena code still touches of this family: `ReverseIoOwnValidity`'s
   refcount when a `ReverseIoOwn` is dropped in a turn, and the event loop's
   executor lock when a context torn down inside a turn destroys its
   cross-thread fulfiller there (both as before). The `AbortSignal`
   registration cells are closed; see violation 10 below, which also triages
   the rest of the family -- the actor cache LRU, the module registry,
   `ReverseIoOwnValidity`, `IsolateBase::queue` and `CompileCache` -- by what
   an attacker gets from each.
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
10. **The shared structures a turn reaches that the host also uses.** Five
   were named beside `ReverseIoOwnValidity` as the remainder of violation 4.
   What matters about each is not that it is shared -- the whole C++ heap is,
   until it moves -- but what an attacker with a write primitive gets out of
   it that it does not already have from the heap at large: whether host code,
   running outside any arena with host privilege, later reads a pointer or
   calls through a vtable the turn wrote, and whether it is another tenant's
   state. In that order:

   **a. The `AbortSignal` registration cells (`api/basics.h`): a host
   control-flow hijack, reachable from any worker. Closed.** A native abort
   registration was a `kj::Arc<RegistrationCell>` on the shared heap holding
   `kj::MutexGuarded<kj::Maybe<kj::Function<void(jsg::Lock&, const
   kj::Exception&)>>>`, shared between the signal and one consumer. The
   consumer's RAII handle is held by (or attached to) objects the owning
   request destroys -- `AbortSignal::wrap` attaches the `Cancellation` to the
   wrapped `kj::Promise`, which is how `fetch(url, {signal})` registers -- so
   it is dropped on the event loop, outside the isolate lock and outside the
   guest. That drop did two things in memory a turn can write: it took the
   cell's mutex, and it destroyed the `kj::Function`, which is a call through
   the disposer pointer of that function's `kj::Own`. A turn that rewrites
   that one word therefore makes the host jump to an address of its choosing,
   with the host's privilege rather than ring 3's, at a moment it controls
   (the request completing). The `kj::Arc` refcount, likewise written by that
   thread, is a second primitive: forced to zero early it frees the cell under
   the host, and left high it leaks it. Reachability is as good as it gets for
   an attacker -- one `fetch` with a signal per request, no flags, no
   configuration.

   What closes it is that the drop no longer touches the signal's memory at
   all. The record holding the action is the signal's own, read and written
   only under the isolate lock and destroyed only there; the separate fact of
   whether the consumer still wants the action is an entry in gk's ring-0
   resolve table, which the handle owns and retires when it is dropped. The
   table is beyond a turn's reach, so dropping the handle is one validated
   retirement and nothing else: no lock in guest memory, no refcount, no
   destructor over bytes a turn can write. A turn resolves the entry before it
   takes the action, so a dropped handle disarms the registration as the
   cleared slot used to; a cross-context abort takes the action with it and
   resolves the handle again on arrival, which keeps the "a consumer that went
   away turns the delivery into a no-op" rule. The signal retires whatever
   entries it still holds when it is destroyed, and each side retires only an
   entry that still resolves to its own record, so whichever runs first wins
   and the other finds nothing. `abort-signal-test.sh` drives it: a turn
   rewrites the disposer word of a live action's `kj::Function`, the host
   drops the handle on the event loop, and the chosen disposer is never
   called, the action is still there and the entry no longer resolves; a
   registration whose handle is still held still runs its action on abort, and
   one whose handle was dropped does not. (In the `WORKERD_GK_NEG_ABORT_CELL`
   build the handle clears the action wherever it is dropped, as it did
   before, and the chosen disposer logs that the host called it, which fails
   the script.)

   The `RpcRegistration` cells beside them (`api/basics.h`) have the same
   shape but not the same exposure: there is no consumer-side handle, and
   every one of their slots is taken under the isolate lock -- on abort, on a
   sweep, or when the signal is destroyed -- so no host thread locks or
   destroys anything in them. They stay as they are.

   **b. The actor cache LRU (`io/actor-cache.h:1084`): host-side writes
   through guest-written pointers, across the actors of one isolate. Open.**
   `SharedLru::cleanList` is a `kj::MutexGuarded<kj::List<Entry,
   &Entry::link>>` holding the clean entries of every `ActorCache` in the
   isolate (`Worker::Isolate::Impl::actorCacheLru`, `worker.c++:620`), so one
   actor's turn can write the links of another actor's entries. A turn reaches
   it on ordinary storage calls -- `ActorCache::get` and friends take
   `lru.cleanList.lockExclusive()` directly (`actor-cache.c++:395`, `:439`,
   `:465`, `:614`, ...) -- and so does host code on the event loop, in the RPC
   completions and the flush (`:491`, `:541`, `:802`, `:860`, `:1180`,
   `:1228`). The host walks and splices that list and drops `kj::Own<Entry>`
   from it, so a rewritten link is an arbitrary write performed by the host
   and a rewritten entry is a call through a disposer the turn chose, both
   outside any arena. The bound is the isolate: entries from different actors
   share the list, entries from different isolates do not. In workerd this is
   reachable with a Durable Object namespace that has no `actorStorage`
   directory, where `ActorCache` backs in-memory storage
   (`server.c++:1305`); in production it is the RPC-backed storage path.
   Closing it is the split the inventory already names -- the cache is H only,
   with messages for get/put/delete/list -- which is a larger piece of work
   than a holder conversion.

   **c. The module registry (`jsg/modules-new.h:801`): cross-isolate, but
   within one worker and behind an experimental flag. Open.**
   `ModuleRegistry::lookup` takes `impl.lockExclusive()` on every resolution
   (`modules-new.c++:2330`), which is every static import, dynamic `import()`
   and `require()` a turn makes, and the lock guards the bundle arrays the
   lookup walks and the caching it does. The registry is a `kj::Arc` that in
   workerd belongs to one worker (`server.c++:5765`) but in production may be
   shared by several replicas of that worker, so a rewritten bundle array
   gives another isolate's `import` a `const Module&` the attacker chose -- a
   vtable call at that isolate's ring 3, and the ability to serve it source of
   the attacker's choosing -- and a corrupted mutex word deadlocks the next
   replica to resolve. The bound is the worker: one tenant's replicas, never
   another tenant. It is gated on the new module registry, which requires
   `--experimental` (`server.c++:5756`), so it is not reachable in a default
   configuration today. The split the inventory names -- the bundles mapped
   read-only into the arena, since they are immutable after `finish()`, and
   the builder and the lock H -- is the fix.

   **d. `ReverseIoOwnValidity`'s refcount (`io/io-own.h:23`): a host-side
   use-after-free of its own tenant's object. Open.** The validity token is a
   `kj::Arc<ReverseIoOwnValidity>` -- an `AtomicRefcounted` on the shared heap
   -- held by the `IoContext` (`io-context.h:1213`) and by every
   `ReverseIoOwn`. A turn that drops a `ReverseIoOwn` writes the refcount,
   which is the one refcount of the `IoOwn` family still in guest reach. Set
   to one before such a drop it frees the token while the context still holds
   its `Arc`, so the context's `invalidate()` (`io-context.c++:786`) writes
   into freed memory and `checkValid()` reads it, and the free itself runs the
   `Arc`'s disposer host-side. What that buys is a one-byte write and a stale
   read at an address the attacker can groom, within its own tenant, plus a
   double free. Reachability is narrow: the holders are
   `JsRpcPromise::Handles` (`worker-rpc.c++:2099`) and the trace-stream
   handler (`trace-stream.c++:1033`), dropped in a turn only when the
   context's list disposes of them, and `ReverseIoOwn` is never dereferenced
   in a turn at all (see Cost). The conversion is the same one `IoOwn` had:
   the token becomes a handle whose entry the host retires, with validity
   being the entry's liveness rather than a flag in a refcounted object.

   **e. `IsolateBase::queue` from `Data::destroy` (`jsg/setup.h`): not
   reachable from a turn.** Every path that pushes onto the deferred
   destruction queue is guarded by the isolate *not* being locked on the
   calling thread: `Data::destroy` (`jsg.c++:58`) and `WeakV8Ref::destroy`
   (`jsg.h:1196`) call `deferGlobalDestruction` only in the
   `!v8::Locker::IsLocked(isolate)` branch, `Wrappable::maybeDeferDestruction`
   (`wrappable.c++:322`) the same, and both callers of `destroyUnderLock`
   (`setup.h:1341`, `filesystem.c++:2044`) are in that branch too. Inside a
   turn the isolate is locked by that thread, so in-arena code does not take
   this mutex; it is host-side code dropping handles off the isolate's thread
   that does. What remains is not specific to the queue: its items are
   destroyed host-side at the next lock (`applyDeferredActions`,
   `setup.c++:288`), and an item is a `kj::OneOf` holding a `v8::Global`, a
   `RefToDelete` or a `kj::Own<void>` on the shared heap, so the host calls
   through disposers a turn can write -- which is true of every `kj::Own` on
   that heap and goes away with the heap, not with anything done to this
   queue. The inventory's plan for it (a ring of `{kind, handle}` records)
   still stands as part of that; it is not a violation-4 item.

   **f. `CompileCache` (`jsg/compile-cache.h:54`): not an exposure.** Nothing
   calls it. `CompileCache::get()` has no caller in the tree, and neither
   `add()` nor `find()` is reached from anywhere: the only file that includes
   the header besides its own translation unit is the build-time code-cache
   generator (`src/rust/gen-compile-cache/cxx-bridge.c++`), which does not use
   the class. The compile-cache bytes the runtime actually consumes come from
   elsewhere and are read-only -- `script.getCompileCache()` out of the capnp
   config (`per-isolate-bootstrap.c++:378`) and `module.getCompileCache()` out
   of the embedded module table (`api/node/node.h:93`). The class is dead code
   on this branch, and its function-local static (R7) is never reached. Were
   it ever wired up it would be the worst of the six -- a process-wide map
   whose values are a raw `{data, length}` fed to V8 as a code cache, so a
   write to it would hand every other isolate in the process a deserialized
   code object of the attacker's choosing -- which is the reason to delete it
   rather than leave it for someone to start using.

## Inventory: `api/`

Almost all of it falls out of four rules, on top of R1-R9 above.

| Rule | Test | Class |
|---|---|---|
| A | `JSG_RESOURCE_TYPE` (232 declarations across 70 headers) | **I**, by R1. The compliance sub-test is what produces the escapees: every member must itself be I, a handle, or a read-only pointer |
| B | `JSG_STRUCT` (201 declarations) | **I**. Translation is by value (`jsg/struct.h:7`), so the C++ instance exists only within a turn or as a field of an I object |
| C | Reached through `IoOwn<T>`/`IoPtr<T>`, or created by `IoContext::addObject` | The pointed-to `T` is **H**; the `IoOwn` is a handle and the context's record of the `T` is in H memory (done, see `io/` above), and the `T` itself is still placed in the shared heap. Roughly 60 sites, all consistent -- none needed changing for the handle. The `kj::OneOf<uint, IoOwn<...>>` pattern in `Fetcher` (`api/http.h:575`), `DurableObjectNamespace` (`api/actor.h:321`), `KvNamespace` (`api/kv.h:291`) and `R2Bucket` (`api/r2-bucket.h:594`) is instructive: the `uint` arm is a channel number the host resolves, and the `IoOwn` arm now has the same shape |
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
| `AbortSignal` registration cells (`api/basics.h`) | **Native registrations: done.** The record holding the action is the signal's, written only under the isolate lock, and the consumer's RAII handle owns a resolve entry instead of a share in the record: dropping it on the event loop retires the entry and touches nothing in the signal. A turn resolves the entry before it takes the action, and a cross-context delivery resolves it again on arrival. The RPC registrations beside them keep the `kj::Arc` + `MutexGuarded` shape, which no host thread touches. See violation 10a |
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
| `kj::Own<T>` | A disposer pointer plus an object pointer. Across the boundary the disposer is an attacker-controlled indirect call. `IoOwn<T>` is `{DeleteQueue, hostMemory::ResolveHandle, OwnedDisposal}` (`io-own.h`): both handles are validated against `gk_resolve`'s table on use (liveness, generation; see Cost), the disposer the host calls is in the H node rather than in guest memory, and the context is named by its entry's handle, with no refcount | The disposer in the H node is the one the turn journaled at registration, so it is attacker-chosen once rather than attacker-rewritable; closing that needs the `T` and its disposer placed by the host. I-internal owns should use a disposer baked into the type so no disposer pointer exists in guest memory |
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
pointers (done, see below); then the promise channel, which retires the
`IoContext::run` result problem and the event-loop touches together.

What the `IoOwn` handles give, and what remains. `IoOwn<T>` is
`{DeleteQueue, hostMemory::ResolveHandle, OwnedDisposal}` (`io-own.h`),
where the `DeleteQueue` is the handle of the context's own entry: the
object's handle names the object through `gk_resolve`'s table,
`addObject` registers it through the turn's reserved batch, and
`operator->`, the destructor and the delete-queue drain resolve it for each
use, failing the use when it names nothing (`ioown-test.sh` shows a handle
rewritten from inside a turn to an address the turn chose refused by the
next turn's dereference and by the host's drain, and the isolate still
serving). The context's record of the object is in H memory: the owned-list
node, with the links the host's teardown walk and delete-queue drain follow
and the disposer the host calls, is allocated by the host at the
reserved-batch commit from the words the turn journaled (disposer, context
handle, disposal pointer), reached through the entry's companion, and freed
by the host when the holder releases the entry, when the queue is drained
or when the list is torn down; a turn that drops a holder disposes of the
object itself, through the disposal record the holder carries, and journals
the release; a context torn down inside another context's turn drains its
list through the `IO_CONTEXT` hostcall, which hands the turn each object
with its disposer and frees the nodes host-side (`ioown-test.sh` shows a
turn's write into a node faulting at the node, and the teardown walk
reaching the node's own disposer). The delete queue is in the same record,
behind the context's entry: a holder dropped off its context, or an action
sent to another context, is a journal note from inside a turn and a direct
push under the record's lock from outside one, with the host validating
the note's context and handle before queuing, and the record is torn down
with the context, so a holder that outlives its context resolves the
context's handle to nothing and drops its object to the list that already
disposed of it (`ioown-test.sh` shows a turn's write into the record
faulting at the record and a context handle rewritten to another isolate's
or to a chosen buffer queuing nothing, with the host's later lockers
unaffected; `io-own-test` shows a holder dropped on another thread
disposed of on its context's thread, and one dropped after its context has
ended releasing nothing). What the node does not protect is the
`T`: it is H by rule C and still placed in the shared heap, so a turn with a
read primitive can read it through the pointer the resolve returns, and the
host's disposal of it runs a destructor over memory the turn can write; and
the disposer the node records is the one the turn journaled, admitted by
the commit unvalidated, so a turn that lies about its own object's disposer
sends the host's disposal of that object to a function of its choosing --
the same exposure the `T`'s vtable and members carry until the `T` is
placed by the host, and the same one a queued action's closure carries when
the host destroys it. Placing the `T` is not the step that follows, for the
reasons below. The table tells one
tenant's handle from another's: each entry records the page-table root the
host registered it under and the ring-0 lookup compares that against CR3,
the root the calling turn runs under, so a handle rewritten to another
isolate's live handle resolves to nothing (the cross-isolate form in
`ioown-test.sh`, which also shows the host resolving the same handle to
that isolate's object, so the refusal is about who asked). CR3 is the
caller identity, rather than the `checkFarGet` comparison of the `IoOwn`'s
own context handle, which is as writable as the object's, or a per-vCPU
record gk does not keep: ring 3 can neither read CR3 nor load it, and the
host alone decides which root a turn enters with. The base root counts as a
root rather than a wildcard, so an entry a host thread registers outside a
turn has to name the isolate it belongs to
(`hostMemory::registerResolveFor`); the context's own entry, created on the
event loop, is the one such entry today. The delete queue's side of the
residual is closed too: a dropped handle is queued only for the context
whose list holds its node, whatever context handle the holder carries. The
owner closes the cross-isolate read; the cross-isolate *retire* is a
separate question, because a drop is not a use. A holder's destructor
releases its entry whatever the resolve answered, and inside a turn that
release goes into the turn's journal for the host to act on afterwards,
where the lookup compares no owner. So every handle the host takes out of a
journal -- the retire list, the notes, the context an owned object's
registration names -- is checked against the arena the turn ran under,
which the host records at the turn's entry from gk's record of the thread
and keeps in the pool, in host memory (`hostMemory::ownedByTurn`, over
`gk_resolve_owned_by`); a handle that is another isolate's retires nothing,
unlinks nothing and queues nothing. What that does not reach is a holder
dropped outside a turn: there is no `CR3` to appeal to there, the handle
and the context handle in the holder are both the turn's to rewrite, and a
holder carrying another isolate's context *and* an object of that same
context agrees with itself and so passes the node-to-context check that
catches the mismatched pair. Closing that one wants the holder itself out
of the shared heap, which is the same step the `T` is waiting on. The
tag names a root, and roots are recycled -- a destroyed arena's root is the
next arena's, that being the first page the page-table allocator hands out
-- so an entry that outlived its arena would resolve for whichever isolate
inherits the root. The runtime does not let that happen, since an isolate
outlives every context that registers an entry under it, but gk does not
rest on that: each arena carries the list of its own entries, and
`gk_arena_destroy` retires whatever is left before the root goes, on stderr
and counted in `gk_stats.resolve_orphans`. What the owner does not tell
apart is two entries of the same isolate, and that is what the entry's type
word is for: it carries a constant derived from the C++ class of the object
the pointer names (`hostMemory::resolveTypeTag`), the lookup hands it back
beside the pointer, and a holder resolves only an entry of the class it was
built for -- at the use and at the drop, since releasing another holder's
entry frees an object that holder still names. An `IoPtr`'s drop compares it
too (`DeleteQueue::releaseReference`), although an `IoPtr` owns its entry
rather than the object the entry names: retiring another holder's entry
leaves that holder resolving nothing, and an `IoPtr` is dropped wherever its
JS wrapper's C++ object is destroyed, which is often the event loop, outside
any turn, where there is no arena for the host to compare the handle
against. The comparison is made in the
runtime rather than at ring 0, against a constant the compiler materializes
into text a turn cannot write and cannot have remapped
(`gk_mapping_change_ok`); it therefore binds a turn with a write primitive
and not one that has reached control flow of its own, where the owner's
ring-0 comparison still holds. What is left is a swap between two live
entries of the same isolate *and* the same class, which no tag distinguishes,
and a turn that lies about the class in its own journal, which waits on the
journal leaving the shared heap along with the disposer beside it.

### Placing an `IoOwn`'s `T` in host memory

Host memory is unreachable from inside the guest at either ring: a
`gk_host_alloc` block is registered `GK_PROT_REFUSE` and mapped into no
root, so a guest access to it faults "at ring 3 or at ring 0, inside an
arena or on the base root" (`gk_host_alloc` in `gk.h`). An object placed
there can therefore be constructed, read, written and destroyed only
outside `gk_run_user` -- on the host, where the event loop runs -- and a
turn that touches it faults at the address it touched. That is the
protection; it is also the whole of the constraint. Three facts about the
`IoOwn` population decide what the constraint leaves.

**Every object an `IoOwn`, `IoPtr` or `ReverseIoOwn` names is created
inside a turn.** Counted with a per-class counter on `IoContext::addObject`,
`addObjectReverse` and the `IoPtr` constructor, each recording whether
`hostMemory::inTurn()` held, over the `api/tests` and `io` test suites (768
tests) and four end-to-end guest-kernel workloads (`disk-sqlite-test`,
`surface-test`, `abort-signal-test`, `ioown-test`): of the 36 classes that
appear, every registration was made inside a turn and none outside one, the
only exception being `io-own-test`'s own fixture, which registers from the
test's event loop. The single entry the host registers for itself is the
context's own (`DeleteQueueOwner`, through `registerResolveFor`), and it
names the owner rather than an owned object. So no `IoOwn`'s `T` can simply
be allocated by `hostOwned<T>` at its allocation site: the site runs at ring
3, where neither `gk_host_alloc` nor a constructor writing the block can
run.

**Nearly every one is read inside a turn after the turn that made it.** The
same counters, distinguishing a dereference made in the turn that registered
the object from one made in a later turn, leave 13 of the 36 never read in a
later turn. Three of those are not candidates anyway:
`jsg::JsRef<jsg::JsValue>` is an I class by rule R2,
`Worker::Actor::FacetManager` is named by an `IoPtr`, which owns nothing and
so decides no placement, and the `kj::Promise<api::DeferredProxy<void>>` an
entrypoint's response becomes is read and disposed of inside the turn that
makes it -- `awaitJs` moves out of it with `kj::mv(*result)` in the same
microtask drain -- so placing it would protect nothing. The ten that remain
could live in host memory from the end of their first turn onward:
`TraceContext` and `TraceContextParent` (`io-context.c++`, the spans a
subrequest client is handed and never reads),
`api::JsRpcReplayMemoryTracker`, `rpc::JsRpcTarget::CallResults::Pipeline`,
`api::LegacyWebSocketAdapter::AutoResponse`,
`api::WritableStreamInternalController::Writable` and four continuation
functors. Everything else is read at ring 3 after its first turn and would
fault there: `kj::PromiseFulfiller<void>` (947 registrations in the suite,
every read a late one), the `awaitJs` fulfiller (168), `SpanParent` and
`UserTraceAsyncContext` (84 each), `api::Fetcher::OutgoingFactory`,
`api::ReadableStreamSource`, `ActorCacheInterface` and the rest.

**A resolve entry cannot name host memory.** `gk_resolve_register`,
`gk_resolve_register_for` and `gk_resolve_commit` all run the pointer
through `resolve_ptr_ok` (`gk.c`), which admits only `GK_PROT_KEEP` memory
outside every arena but the one that will own the entry; a registration
naming a `gk_host_alloc` block is
refused and returns handle 0, which the library confirms directly -- an
ordinary heap pointer registers, a `gk_host_alloc` pointer is refused with
"is not memory the entry's owner can reach". The check is right for the commit, whose
pointer comes out of a turn's journal, but it forbids the host-side
registration a placed `T` would need as well, and there is no way to move a
committed entry's pointer afterwards. (The lookup's own description in
`gk.h` supposes an entry whose object is in host memory; no such entry can
exist while the check stands.)

Together these say what the mechanism would have to be. The allocation site
marks the class, which is a compile-time property of `T` so that the
holder's `operator->` and destructor can be compiled differently for it; the
turn still constructs the `T` with `kj::heap` in guest memory and registers
it as now; the host, at the commit that follows the turn, move-constructs
the `T` into a `gk_host_alloc` block, destroys the guest shell, and points
the entry at the block through a gk entry point that admits a host-memory
pointer from a host-side caller and only from one; the node's disposer
becomes a host-memory disposer so that the host's own disposal frees the
block; a holder dropped inside a turn is noted for the host instead of
disposing of the object itself, since the turn cannot run the destructor;
and the `IO_CONTEXT` hostcall's drain keeps such an object host-side rather
than handing it to the turn with its disposer. The cost is one host-memory
allocation and one move per object, no hostcall, and the protection starts
at the end of the turn that created the object rather than at its
construction.

What it buys does not justify it yet. It covers ten classes, none of them
on a request's common path and together about a hundred registrations
across the whole test suite against the roughly 1600 it does not cover, and
it closes none of the three residuals the handles still carry. Those are
the holder's and not the object's: a retire made outside a turn reads the
handle out of a holder in the shared heap, a forged pair of a victim's
context handle and an object handle of that same context agrees with
itself, and a same-class swap is two handles of one isolate. A holder
cannot move to host memory, because the turn reads it on every
dereference, and moving it to a region readable but not writable at ring 3
-- a class gk does not have -- would only move the problem one step, since
the reference to the holder is itself in a JS-heap object the turn must be
able to write. What closes them is the shared heap leaving the arena,
which is the placement work the inventories above describe, and not an
`IoOwn` change.

### The disposer a registration journals

The provenance rule says a pointer the host dereferences comes from H memory,
while an index into an H-owned table may come from anywhere because the host
validates it against the table. The disposer a node records is a pointer the
host *calls*, and it comes from the turn's journal, so it is the rule's
clearest open case. Turning it into an index wants a table the host can fill
with the disposers that may legitimately be called; the measurement says
there is no such table to fill, because the disposer of a `kj::Own<T>` is not
a property of `T`.

Counted with a per-class counter on `DeleteQueue::registerOwned`, classifying
each registration's disposer against `kj::_::HeapDisposer<T>::instance`,
`kj::NullDisposer::instance`, the owned object's own extent and `dladdr`, over
the `api/tests` and `io` suites: of 1771 registrations in 34 classes, 542
carry `kj::_::HeapDisposer<D>::instance`, 34 carry `kj::NullDisposer::
instance`, 33 carry another static the binary holds without a symbol `dladdr`
can name, and **1162 carry a subobject of the owned object itself**.
`kj::Refcounted` inherits `kj::Disposer` privately and `kj::refcounted<T>()`
hands the object to `kj::Own` as its own disposer (`kj/refcount.h`), so every
refcounted object is its own; `kj::_::WeakFulfiller` inherits `kj::Disposer`
through `WeakFulfillerBase` (`kj/async-inl.h`) and so is its own at offset 8,
which is what `kj::newPromiseAndFulfiller` hands `IoContext::awaitJs` -- 934
of the 1162, the commonest registration in the runtime and reachable from any
`await` of a JS promise.

Three things follow.

**The index cannot be minted where the registration is made.** The disposer a
`kj::Own<T>` carries is the one its *dynamic* type was created with: an
`Own<D>` converted to `Own<T>` keeps `HeapDisposer<D>::instance`, which is why
26 of the 33 unnamed statics arrive as `api::ReadableStreamSource`. `D` is not
known at `addObject`, and the only site that knows it is the `kj::heap<D>` or
`kj::refcounted<D>` that made the `Own`, far from the registration and not
reachable from it. A compile-time table keyed on `T`, as the type word is
(`hostMemory::resolveTypeTag`), cannot hold the entry such a registration
needs.

**Validating the pointer does not stand in for an index.** The two shapes a
legitimate disposer takes are a static in the image and a subobject of the
owned object. A static's own vtable pointer is in a relocation-read-only page,
which a turn cannot write and cannot have remapped
(`gk_mapping_change_ok`), so admitting only those would be a real bound. The
object's is not: the owned object is in the shared C++ heap, which gk maps
writable into every arena, so for the 1162 the host's call goes through a
vtable word the turn can write whether the pointer was checked or not.
Bounding the pointer to the object's extent confines the host to the object it
was about to dispose of and nothing further, which is less than it reads as,
since the object pointer is journaled too (below), and it does not reduce what
a turn can make the host do.

**Four host-side calls go through the journaled pair, and the loosest needs
no object at all.** A registration journals a disposer and the address to
hand it, and the host calls the two together from four places: the
delete-queue drain and an off-turn drop (`OwnedObjectList::release`), the
context's teardown walk (`~OwnedObjectList`), and the refusal hook
(`OwnedObjectList::refused`). The first three read the pair out of the owned
node, which is in host memory, but the node's two words were copied there
from the turn's aux words when the registration committed, so the pair is
the turn's either way. `refused` reads the aux words directly, and it is the
one that needs no node and no registration that survives: a registration
naming a context that no longer exists is refused on the hook's own terms,
with nothing linked anywhere, and the pair is called all the same. The two
words need bear no relation to the object the registration carried. In all
four the host loads a vtable word from an address the turn supplied and
calls through it with an argument the turn supplied. The refusal hook is the
narrowest of them in practice rather than the widest: over `api/tests` and
`io`, 556,386 registrations against 16,588 contexts, no registration is
refused at all and it disposes of nothing, which is what giving a context's
entry to its own isolate leaves.

**Bounding the disposal pointer to the object's extent is not the bound it
looks like.** Three things are wrong with it. The object pointer is
journaled too, and all that separates it from the disposal pointer is
`gk_resolve_commit`'s check that it is memory the entry's owner can reach,
so a turn made to keep the two in agreement sets both to the same chosen
address. The host never dereferences the disposal
pointer in any case: it is the argument to a call whose target is the
disposer, which is the word that is followed, and a disposer the turn chose
can ignore its argument. And the relation does not hold for the runtime's
own registrations -- `kj::Own` hands its disposer the most-derived object's
address (`kj::_::castToVoid`), which for a `T` that is not the primary base
of its dynamic type lies below the `T*` the table holds. 978 of the 556,386
registrations are that case, every one of them with the disposal pointer
below the object pointer, so a host that required it inside the object would
orphan them -- and the host has no size to bound it with, since nothing
carries `sizeof(T)` past the registration and the most-derived extent is not
`sizeof(T)` anyway.

**The journaled pointer is not the only route to the same call.** A disposal
through a correct `HeapDisposer<D>` runs `~D()` over members the turn
controls; a context that ends before a turn does destroys a queued action,
which is a `kj::Function` the turn allocated in the shared heap, through that
closure's own vtable. Disposing of an object that sits in the arena's shared
heap is an indirect call through guest-writable memory however the host
reaches it. So closing the journaled disposer would remove one of several
equivalent routes and leave the capability itself intact, which is why the
`T`'s placement, not the disposer, is what the residual waits on -- and why
the previous section's conclusion stands: what closes the class is the object
and the heap it sits in leaving the arena.

The `kj::Function` box an `ACTION` note names (`words[2]` in
`OwnedObjectList::noted`) is the same case. The box is allocated by the turn,
in the shared heap, so a box whose pointer the host had validated would still
be destroyed through a vtable the turn can write. The one thing a check would
add is to keep the host from `operator delete`-ing an address that is not a
block at all -- and a registration's object pointer can already be such an
address, since gk admits any memory the entry's owner can reach -- so it
would narrow nothing that is not already open.

Which is why the host destroys the box whenever no context takes it, whether
because the note names a context of another isolate or because the context it
names has ended. The two are the same call on the same turn-chosen address,
and a turn reaches the second by naming a handle that resolves to nothing,
which the owner comparison admits on purpose. Not destroying it on the first
would buy nothing and cost a leak the turn paces: a box and the closure it
holds, never reclaimed because nothing else names them, one per note, up to
the notes a journal holds in each turn and again in the next -- against a
turn's other allocations, which the request's end reclaims. A note naming
another isolate's live context has no producer in the runtime, since the
`DeleteQueue` a turn reaches an action through is named by a JS object of the
turn's own isolate; destroying the box is the right answer for one anyway,
being what a context that cannot be reached does with an action it will never
run.

### Disposing of an owned object inside a turn

Placing the object out of a turn's reach is the one direction; the other is to
stop the host reaching the object. The host would schedule disposal instead of
performing it, and the destructor would run at guest ring 3 in a turn of the
owning isolate, where a vtable that isolate has corrupted costs it only
itself. Counted with per-site counters on `DeleteQueue::registerOwned`,
`OwnedObjectList::drop`, `release`, `take` and the teardown walk, over the
`api/tests` and `io` suites (768 tests, 16,006 `IoContext`s, 283,648 turns;
one context per stateless request): of 554,798 disposals, 432,736 (78.0%)
already run inside a turn, through the holder's own disposal record, and
115,510 (20.8%) run host-side -- 68,706 the delete-queue drain, 38,732 the
context's teardown walk, 8,064 an immediate drop made under the isolate lock
but outside the guest, and 8 a host-side `take`. Of the remainder, 2,820 are
the teardown path that already runs in a turn and 3,732 are an in-turn `take`,
which hands the object out of the family. That is 7.2 host-side disposals per
context against 27.0 in-turn ones.

**The drain is already under the lock, in the arena, and needs no crossing.**
`DeleteQueueOwner::drainDeletions` runs inside `runInContextScope`, whose one
caller always goes on to enter the guest (`runImpl`), so the objects can be
handed to that turn instead: the host detaches the node, retires the entry
and frees the node before the turn, as `release()` does, and leaves the turn
the `{disposal, disposer}` pairs to dispose of at its start, which is what
the teardown's `DRAIN` hostcall already hands over. Doing it as a hostcall
instead would not be affordable: 67,766 of the 283,648 turns find a non-empty
queue and nearly always find exactly one object in it (68,706 objects in
67,766 drains), so it would be 4.2 hostcalls per context at 6.4 us each
against a hello request's 25.4 us (`measure/findings.md`).

**The teardown walk has neither a turn nor a lock, and cannot be given one
later.** 15,988 of the 16,006 contexts are destroyed outside any turn with no
isolate lock on the thread, and 11,706 of those still own objects. Deferring
their disposal past `~IoContext` is not available: the objects point back at
the context, which is why `runInContextScope` refuses to delay destruction to
a later event-loop turn and why `IoContext` declares `worker` above
`ownedObjects`. So the destructors have to run while the context is still
there, which means entering the guest synchronously from `~IoContext`.
That much is mechanically possible without the isolate lock, since
`gk_arena_enter` is per thread and nests (`gk.h`) and the isolate outlives
the list, at one guest entry per non-empty teardown -- 0.73 per context, 2.75
to 6.77 us each. What it is not is a smaller claim than the host-side walk
makes: the walk runs today with no isolate lock either, so its destructors
already must not touch V8; what it would add is that they must not touch host
memory either (below).

**"In a turn" does not say whose turn, and the paths that exist today do not
ask.** `~OwnedObjectList`, `ReverseIoOwn::drop` and `IoOwn::operator
kj::Own` all branch on `hostMemory::inTurn()`, which is a property of the
thread, so a disposal it admits can be of an object of an isolate other than
the one whose arena the turn runs under: 9,814 of the suite's 17,690
`ReverseIoOwn` drops are inside a turn, and 2,016 of those are of another
isolate. The whole point of in-turn disposal is that a corrupted vtable
reaches only the isolate that corrupted it, and a turn chosen by the thread
rather than by the object gives the opposite. The next section works that
case out: it is reached at seven of the nine sites and refused there by
checks the sites do not make themselves, and the two sites nothing checks are
the two the runtime never reaches. Any widening of in-turn disposal has to
carry the comparison itself.

**A destructor at ring 3 cannot touch host memory.** A `gk_host_alloc` block
is mapped into no root and `gk_host_slot_free` writes gk's supervisor block,
so an owned object whose destructor frees host memory or releases a
`HostSlot` would fault instead of being disposed of. None of the 36 classes
does today, which the 432,736 in-turn drops and the 2,816 objects the
existing teardown hands to a turn are the evidence for, but nothing enforces
it; and the rest of the work moves runtime state *into* host memory, so
in-turn disposal and host placement pull against each other object by object.

**What it would close is a route, not the capability.** The host goes on
destroying objects a turn allocated in the shared heap, through their own
vtables, at a higher rate than the disposals being moved and with nothing to
hang a deferral on: in the same suite the turns made 175,756 `awaitIo` calls
and 204,588 `addTask` calls, 11.0 and 12.8 per context against 7.2 host-side
owned-object disposals, and each builds a `kj::Promise` continuation chain
and a `TaskSet` task in the shared heap inside the turn which the KJ event
loop destroys, through `PromiseNode`'s virtual destructor, when the promise
settles or is cancelled. Those carry no handle, no registration and no
owning context; the event loop destroys a promise node as part of resolving
it, at a moment when the isolate may be locked by another thread, so there
is no turn to defer them to short of changing KJ's event loop. 3,740 owned
objects also leave the family outright through `IoOwn::operator kj::Own`,
3,634 of them `api::ReadableStreamSource`, after which the object is an
ordinary `kj::Own` with no context, no handle and no isolate.

So the conclusion is the one the journaled disposer reached, from the other
side: disposing of an object that sits in the arena's shared heap is an
indirect call through guest-writable memory however the host reaches it, and
taking the host out of one family of those calls leaves the capability
intact. Not destroying the objects at all -- letting the arena's memory go
with the isolate -- is not the alternative either, since the destructors
close descriptors, cancel I/O and release locks, and the arena's lifetime is
the isolate's rather than the request's. What closes the class is still the
shared heap leaving the arena. Until then the cheapest honest step is the
drain, which costs nothing because the lock, the arena and the crossing are
already there, and which is worth taking when the surrounding placement work
makes it the last route rather than one of several.

### Disposing another isolate's object inside a turn

`hostMemory::inTurn()` is a property of the thread. Nine places in
`io/io-own.{h,c++}` branch on it to register, release or dispose of an owned
object inside the turn instead of host-side, and not one of them compares the
object's isolate with the turn's. If any of them ran for an object of isolate
A while a turn of isolate B was on the thread, the disposal -- an indirect
call through a vtable in the shared heap, which A can write -- would run at
guest ring 3 under B's page-table root.

That would not fault. `arena_sync_root` (`gk.c`) copies every top-level entry
outside an arena's own slots from the base root into that arena's root, so the
shared C++/KJ heap, glibc and the runtime's text are mapped at the same
addresses with the same permissions under every root; arenas are placed from
`GK_FIRST_CAGE_SLOT` (slot 64, 32 TiB) upward, clear of all of it. A's object
would be read, A's vtable word followed, and the call would land wherever A
aimed it among the pages executable under B's root -- the runtime image and
B's own code range, since A's JIT pages are in A's arena and unmapped there.
That is a gadget call with a `this` A controls, made inside the arena the
isolation exists to protect: a cross-isolate escape rather than the host-side
one the shared heap already carries. So the question is only whether a
foreign-arena disposal can be reached, and the answer differs between the
sites that resolve a handle and the sites that do not.

**Seven of the nine are entered with a foreign isolate and refused, by checks
that are not their own.** `OwnedObjectList::drop` -- and so `ReverseIoOwn`'s
destructor, which calls it directly with no context of its own to compare --
resolves the handle before disposing, and at ring 3 `gk_resolve` compares the
entry's owner root against CR3, so a foreign entry resolves to nothing and
nothing is disposed; the journal fallback in `resolveOfType` cannot stand in
for it, since the journal holds only handles this turn reserved.
`ReverseIoOwn::operator kj::Own` resolves the same way and throws.
`IoOwn::operator kj::Own` goes through `checkFarGet`, which requires the
holder's context to be the current one. `scheduleDeletion` and
`tryScheduleAction` resolve the context handle before noting anything, and
`noted()` checks `hostMemory::ownedByTurn` host-side as well.
`OwnedObjectList::add` registers from the turn's reserved pool, whose entries
are the turn's own arena's, and its commit hook refuses a registration whose
context is another isolate's. Measured with a two-service reproducer in which
the caller drops, inside its turn, the stubs a callee returned (each export
makes a `TransientJsRpcTarget` holding a `ReverseIoOwn<Handles>` of the
callee's context): 1,102 in-turn drops, 400 of them of the other isolate.
Without the guest kernel all 1,102 resolve and dispose. With it, 702 resolve
and **the 400 foreign ones resolve to nothing** -- exactly the cross-isolate
set -- and the same run logs the commit hook refusing registrations that name
another isolate's context. The suite agrees on the frequency: 2,016 of 5,494
in-turn `ReverseIoOwn` drops across `api/tests` and `io` are of another
isolate.

**The two that nothing checks are the two teardown hostcalls, and nothing
reaches them.** `~OwnedObjectList` and `~DeleteQueueOwner`, when they run
inside a turn, take the owned objects and the queued action boxes from the
`IO_CONTEXT` hostcall rather than from a resolve: the handler finds the
context through the companion directory, which is host-side and makes no
owner comparison by design, and hands the turn raw `{disposal, disposer}`
pairs to call. Nothing there compares arenas, so a context of another isolate
torn down inside a turn would dispose of its objects under the running
isolate's root. No path does that. Of the 16,006 context destructions the
suite makes, 18 are inside a turn, and a run that compared the isolates
found all 18 to be of the isolate whose turn was running; five constructed
abandonment patterns against a service binding -- a
streaming response dropped unread, cancelled, never awaited, aborted through
an `AbortSignal`, and twenty held and dropped together under GC pressure --
produced no in-turn context teardown at all. The reason is deliberate rather
than incidental: a child worker's stub is "typically destroyed while some
other isolate is current, and so we cannot enter the dynamic worker's isolate
to tear it down", so `~WorkerStubImpl` (`server/server.c++`) defers the
unlink and destruction of the `WorkerService` to the next turn of the event
loop, a fix made for a crash of exactly this shape.

**So the invariant is the runtime's, not the guest kernel's: a context is
torn down only outside a turn, or inside a turn of its own isolate.** It
holds today, it is load-bearing for the two unchecked sites, and nothing in
`io-own.{h,c++}` states or enforces it. What would enforce it is the
comparison the other seven sites get for free -- `gk_arena_current()`
(`gk.h`) against the owner the context's resolve entry already records -- with
the disposal done host-side when they differ, trading the host-side indirect
call the whole file already carries for a cross-arena ring-3 one. It is not
written because there is no way to make it fail: a guard for a case nothing
produces is a guard no probe can test.

What the refusals cost, which is the part to watch. A foreign in-turn drop
under the guest kernel disposes of nothing and retires nothing, so the object
stays on its own context's list until that context is torn down, where the
same drop without the guest kernel disposes of it at once. A foreign in-turn
registration used to be worse: `registerOwned` disowns the object before the
commit hook can refuse it, so no node was built and nothing owned it, and the
reproducer lost one object per cross-isolate stub. Two changes closed that. A
context's entry is adopted by the isolate it belongs to rather than left owned
by whichever turn built it, so a callee's own registrations are no longer
foreign at all; and the `refused` turn hook disposes the object of any
registration whose entry does not survive, so a refusal that does happen
drops the object instead of orphaning it. `rpc-context-test.sh` reports no
refusals and no orphans across 120 subrequests. Both refusals are silent at
the call site, which is why neither was visible until it was counted, and both
are the guest kernel's build differing from the ordinary one on paths the
ordinary suite covers.

## Placing the shared heap per isolate

The three sections above each end at the same sentence -- what closes the
class is the shared heap leaving the arena -- and that sentence has two
readings. Read as *out of a turn's reach*, it means H memory, and the
sections that reach it are right that nothing smaller closes what they
name: the host following a disposer, a `T`'s vtable or a queued action's
closure is unaffected by which arena the object sits in, because the host
runs on the base root, from which every arena is mapped. Read as *not one
heap shared by every arena*, it is a different step, and that is the one
the order of work needs.

Which reading the placement work takes is settled by the first of the three
conclusions above rather than chosen. An object a turn legitimately
dereferences after the turn that created it has to stay mapped in that
isolate's arena -- 23 of the 36 `IoOwn` classes are read that way, and
`resolve_ptr_ok` will not admit a host-memory pointer into the table at all
-- so for the shared heap the target is **per-arena, not out-of-arena**: an
allocation the classification calls I goes to the owning isolate's arena,
and one it calls H or B goes to a heap every arena maps. That closes one
half of the exposure: A's objects become unmapped under B's page-table
root, so a ring-3 escape in B can neither read nor write them. Of the other
half it closes nothing. The host-side indirect call stands, the three
residuals the `IoOwn` holders carry stand (each is the owning isolate
forging values in its own holder, which stays writable wherever it is
placed), and same-isolate corruption is out of scope by construction. So
per-arena placement is a precondition for the host-side work and not a
substitute for it, and the two must not be reported as one thing.

### What an allocation census measures, and what it cannot

Which allocations are I and which are H or B has been argued from the rules
above and never counted. `--//src/workerd/util:alloc_census=True`
(`util/alloc-census.{h,c++}`) counts it. The build replaces every form of
`operator new` and `operator delete` and wraps `malloc`, `calloc`,
`realloc` and `free`, and records for each block:

- the allocation site, as the ten return addresses above the interposer,
  walked from the frame pointer with each frame validated so that a caller
  built without one truncates the site instead of faulting
  (`--copt=-fno-omit-frame-pointer`, `--copt=-g1`, symbolized offline);
- the isolate whose lock was held when the block was allocated, taken from
  `jsg::GuestArenaScope`, which is the window in which that isolate's arena
  is the thread's active one, and whether the thread was at ring 3 inside a
  turn;
- the isolate whose lock was held when the block was freed, through a
  16M-entry open-addressed table keyed on the pointer;

and aggregates per site into counters: allocations by context, frees split
into same-isolate, **cross-isolate**, no-lock and owner-less, the set of
isolates that allocated and freed there, and what is still live. It needs
no guest kernel -- the isolate lock is the identity either way -- so it
runs at ordinary speed, and it runs with `use_tcmalloc=False` because
tcmalloc defines `operator new` without going through `malloc`.

What it sees is where a block's *ownership* ends up. A cross-isolate free is
a fault under per-arena placement and not merely a smell, because
`operator delete` is reached through the object's destructor: isolate B
running `~T` over a `T` in A's arena faults at the object. What it does not
see is a cross-isolate *read* that transfers no ownership -- isolate B's
`jsg::Deserializer` reading bytes isolate A allocated for the shared memory
cache is invisible to it -- nor process-wide state that is allocated once
and never freed, for which the phase experiment below is the instrument.
It also misses an allocation libc makes internally and a direct `mmap`:
1,339 frees in the run below were of a pointer it had never recorded.

### The population

One workerd process, six workers -- a plain one, two that use `Intl`, one
that uses `setTimeout`, and a service-binding pair whose caller makes JS
RPC calls, service-binding `fetch`es and RPC calls returning a 4 KiB
`Uint8Array` -- driven 60 rounds through all six endpoints with the guest
kernel on:

- **5,458 distinct allocation sites, 324,877 allocations.** 201,526 of them
  (62.0%) were made under an isolate lock, 182,351 of those at ring 3
  inside a turn and 19,175 under the lock but outside the guest. The
  remaining 123,351 were made with no lock held at all. So the arena switch
  has to span the whole isolate lock and not the turn: a tenth of what an
  isolate allocates it allocates host-side under its own lock.
- **Frees: 82,703 under the allocating isolate's own lock, 5,700 under
  another isolate's, 174,116 with no lock held, 4,798 of an owner-less
  block under some lock.** Two thirds of all frees happen on the base root,
  which is why the allocator's free path must not touch the freed block --
  it is the property the parked branch gets from jemalloc's
  `metadata_use_hooks = false`, and the measurement is what makes it a
  requirement rather than a convenience.
- **The 5,700 cross-isolate frees fall in 25 sites, and every one of them
  has a Cap'n Proto frame in its stack.** 5,400 also carry a workerd JS-RPC
  frame. The named allocations are `capnp::MallocMessageBuilder::
  allocateSegment`, `BuilderArena::MultiSegmentState`, the
  `SegmentBuilder`s and the three `kj::Vector`s `BuilderArena` grows,
  `LocalCallContext`'s `LocalResponse`, `Capability::Client::
  makeLocalClient`'s `LocalClient`, `MembranePolicy::exportInternal`'s
  `MembraneHook` with the membrane's `HashMap<ClientHook*, ClientHook*>`
  and its rehash buckets, and on workerd's side
  `api::TransientJsRpcTarget`, its `Handles`, the `kj::Function` box
  `JsRpcTargetBase` holds, and `IoContext::makeReentryCallback`'s lambda
  with its `kj::Deferred`. 300 are `IoContext::PendingEvent`, which
  `registerPendingEvent` makes on every turn and which a callee's context
  releases under the caller's lock.
- **The `jsg::Object` family is clean.** Over the same run, the 58 sites
  that reach `jsg::Lock::alloc` or `jsg::wrapOpaque` account for 21,871
  allocations -- 10.9% of everything allocated under a lock -- with 3,533
  frees, **every one under the allocating isolate's own lock**: no
  cross-isolate free and not one free with no lock held.
- **The owned-list nodes are already out of the heap.** The census records
  no `hostMemory::allocate` at all under the guest kernel, against 14,011
  allocations at 10 sites without it, 2,000 of them freed cross-isolate.
  That is `gk_host_alloc` doing what it is for, and it is the shape the
  rest of the H population wants.

The same workload with the guest kernel off and 200 rounds gives the same
picture at scale: 977,777 allocations, 61.5% under a lock, and 24,254
cross-isolate frees over 31 sites, again all on the Cap'n Proto path, plus
two the guest-kernel configuration does not produce (below).

The site count is a lower bound on the work, not an estimate of it. This is
one workload; the cross-isolate list above names a dozen mechanisms it does
not exercise -- streams over RPC, hibernatable WebSockets, the shared
memory cache, Pyodide bundles, `WorkerLoader`, replica storage, `AbortSignal`
over RPC -- and the census is blind to the reads among them.

### What breaks first

Four classes, in the order the measurements put them.

**1. The in-process Cap'n Proto call, which is every service binding and
every JS RPC.** It is the whole of the measured cross-isolate set. The
caller builds a `MallocMessageBuilder` under its own lock, the callee's
turn reads and extends the same arena, and the stubs and membrane hooks
that carry the capability are destroyed under whichever isolate drops them
last. No scope around workerd code reaches the segments: they are allocated
inside `capnp::_::BuilderArena::allocate`, called from the callee's turn.
Either the message arena is allocated from a heap both isolates map --
which is a shared-heap scope at `LocalClient::call` and at the
`JsRpcTargetBase`/`TransientJsRpcTarget` constructors, spanning the callee's
turn -- or the message is copied at the boundary, which is the B form the
inventory above asks for and which this measurement prices: 3,816 segment
allocations and 1,200 `LocalResponse`s per 12,000 RPC calls.

**2. Process-wide state established inside the first turn that needs it,
which no free reveals.** Measured by phases: 100 requests to the first
`Intl` worker, a dump, 100 to the second, another dump, and then the sites
that did not grow for the second isolate although it did the same work.
**46 sites and 7,386 allocations**, all allocated under the first isolate's
lock, all still live, none re-allocated by the second. 35 of the 46 are
inside ICU, at the shapes that name the cause: `umtx_initOnce`,
`entryOpen`/`init_entry` and `uprv_strdup` for the resource-bundle cache,
`UDataMemory_createNewInstance`, `UnifiedCache::_get`,
`LikelySubtags::initLikelySubtags`, `getStaticMacroregions`. ICU has
`u_setMemoryFunctions`, so one hook covers all 35.

The other nine are not ICU and not reachable by a hook: 1,290
`std::set<std::string>::insert` nodes, reached from
`v8::internal::JSDateTimeFormat::New` (`js-date-time-format.cc:2585`)
through `builtins-intl.cc:281`, allocated with the plain global
`operator new` inside V8 and never freed. They are V8's own process-wide
locale sets, built by whichever isolate first constructs an
`Intl.DateTimeFormat`. Nothing in workerd can place them; it takes a V8
change, which is why the parked branch needed a patch running the `Intl`
builtins under an embedder shared-allocation scope, and this is the
measurement that says such a patch is not avoidable.

**3. The event loop's timer set, grown at ring 3.** `kj::TimerImpl`'s
`std::multimap` node appears as `__emplace_multi<TimerImpl::
TimerPromiseAdapter*>`, 60 allocations in the run, every one inside a turn
and every one freed with no lock held. The container is the thread's, so
one isolate's `setTimeout` puts a node of a process-wide tree in that
isolate's arena, and the next isolate's insert rebalances across it. The
workload has one worker using timers, so it produces the allocation and not
the fault; the parked branch produced the fault with two.

**4. V8's page-allocator bookkeeping -- not in this configuration.**
Without the guest kernel, `v8::base::RegionAllocator`'s region map
contributes 1,252 cross-isolate frees over 117 sites, and
`v8::internal::PooledPage` two more: process-wide allocator state grown and
shrunk under whichever isolate's lock is held. Under the guest kernel both
go to zero, because each isolate already gets its own `v8::IsolateGroup`
with an `ArenaPageAllocator` over its arena's tail (`jsg/setup.h`), so the
region map is per isolate. It is listed because it is the shape of hazard
to expect from V8 and because the zero depends on that configuration, not
because it is open.

What the parked branch hit is therefore 1, 2 and 3, and the census locates
each one at the allocation site that produces it. What it adds is that the
second class has a V8 half the ICU hook does not reach, and that the first
class is not a handful of workerd objects but Cap'n Proto's message arena.

### The enumeration

The four classes above came out of one workload each time something faulted,
which is a linear search over a list of unknown length, and the length is
what decides whether per-isolate C++ heaps are worth having at all. So the
phase experiment was run again against a workload chosen to reach what the
faults came from rather than what was convenient: two workers running the
same module, each request instantiating a wasm module, running a function hot
enough to be optimized and then calling it with another type so it deopts,
driving a loop long enough to be replaced on the stack, constructing
`Intl.DateTimeFormat`, `NumberFormat`, `Collator`, `PluralRules` and
`RelativeTimeFormat` over four locales, and making a service-binding `fetch`
and eight JS RPC calls against a third worker
(`measure/census-workload.capnp` and `measure/census-run.sh`). 30 rounds per
phase, with the guest kernel on and again off: 413,505 allocations over 7,043
sites, 331,580 of them under an isolate lock and 313,902 of those at ring 3.

Three dumps, not two: one after startup with no request served, one after the
first worker's rounds, one after the second's. Neither isolate exists at the
first dump, because a worker's isolate is created by its first request, so
creating it, running its module and compiling its wasm all fall inside that
worker's own phase -- which matters, because the wasm entry below is
established there and not in a turn.

**The list.** Six process-wide registries end up in whichever isolate was
locked when they were first needed. For each one, where it is grown, what
reads it, and whether an embedder choke point reaches it:

1. **ICU's caches** -- the resource-bundle cache (`ures_openWithType`,
   `init_entry`, `UDataMemory_createNewInstance`), the locale and legacy-key
   pools under `icu::Locale::init`, `UnifiedCache` entries for
   `DateFormatSymbols`, `PluralRules` and `RelativeDateTimeCacheData`, the
   `DateTimePatternGenerator` pattern maps, `LikelySubtags`. 166 of the 257
   aggregated sites and the bulk of the allocations. Read by the `Intl`
   builtins at ring 3. **Embedder**: `u_setMemoryFunctions()`, one hook for
   all of it.
2. **V8's `Intl` available-locale sets** -- `std::set<std::string>` trees
   built by `Intl::BuildLocaleSet` behind
   `v8::base::LazyInstanceImpl<...>` and `CallOnceImpl`, reached from
   `JSDateTimeFormat::CreateDateTimeFormat`, `JSNumberFormat::New`,
   `JSPluralRules::GetAvailableLocales` and `JSCollator::GetAvailableLocales`.
   19 aggregates. Read by the same builtins at ring 3. **V8 patch**, which
   `patches/v8/0049-...` is.
3. **V8's wasm type canonicalizer** -- the recursion-group sets and
   `canonical_supertypes_`, grown at `AddRecursiveSingletonGroup` from
   `ModuleDecoderImpl::DecodeTypeSection`, under the isolate's lock and
   **outside any turn**, when the worker's module is compiled at script load.
   Read by `WasmJs::Install` on every `v8::Context::New`, at ring 3.
   **V8 patch**, which `patches/v8/0050-...` is.
4. **V8's `ThreadIsolation` JIT-page map** --
   `JitPageReference::RegisterAllocation` from
   `NativeModule::AddCompiledCode` under `Runtime_WasmCompileLazy`, and the
   map inserts in `JitPageReference::Shrink` and `SplitJitPageLocked`. Read by
   `StartOfJitAllocationAt` from GC-safe code lookup, the deoptimizer and
   stack iteration, at ring 3. **Embedder**: the map's allocations all go
   through the platform's `ThreadIsolatedAllocator`, so a workerd one
   overriding `Allocate` reaches every one of them (see "What is in H memory"
   and the branch README).
5. **V8's wasm engine registries** -- `WasmEngine`'s `NativeModuleInfo` map
   and the per-isolate sets `UseNativeModuleInIsolate` fills, the
   `NativeModuleCache` keyed on wire bytes, `WasmCodeManager`'s allocation
   pool, and each `NativeModule`'s own code vectors and jump tables. 69
   aggregates between them, grown at module compile under the compiling
   isolate's lock and at lazy compilation inside a turn; `WasmCodeManager` and
   the caches are read at ring 3 through `Runtime_WasmCompileLazy`. **V8
   patch**: these are members of the process-wide `GlobalWasmState` with no
   embedder allocator between them and `operator new`. Not previously known.
6. **The thread's timer multimap** in `kj::TimerImpl`, which this workload
   does not exercise and the earlier one did. **Embedder**: a shared scope at
   `TimerChannel`.

Everything else the diff reports is per-isolate state that the instrument
cannot tell from process-wide state, and the exclusions are a judgement rather
than a measurement: `GlobalHandles` and `TracedHandles` node blocks, the
parser's and preparse data's zones, workerd's own module registry and the
`kj::Path` strings in it, the microtask queue. Each belongs to one isolate, so
each is correctly placed in that isolate's arena; they appear only because the
diff asks "did the second isolate allocate here" and the answer for a
per-isolate site reached through different code addresses is no.

**What the instrument gets wrong, and what to do about it.** Three things,
each of which hid a member of the list above:

- **A site is keyed on ten return addresses, and an allocation made inside a
  turn has JIT code addresses among them.** Two isolates running the same
  script therefore allocate at *different* sites, each with one isolate and no
  growth in the other's phase, and the diff reports both. Keying on the raw
  stack turned 92 aggregated sites into 571. The `phases` mode of
  `alloc-census-report.py` aggregates by symbol name before diffing, which is
  what makes the question mean what it says.
- **Size is not a proxy for hazard.** The JIT-page map's dangerous part is
  four allocations in a whole run, and the canonicalizer's is seven. The
  earlier `shared` mode required ten allocations under a lock before it would
  report a site, which is exactly why neither appeared, and the figure that
  matters is not how much a registry allocates but how many isolates read it.
  Both modes now take the threshold as an argument and the useful value is 1.
- **Ten frames do not reach through the STL.** An allocation inside a
  templated container spends five or six frames on `operator new`,
  `__libcpp_allocate`, `__construct_node` and the key-extraction helpers
  before the first frame that says anything, so the caller that matters is
  sometimes past the tenth. `WasmJs::Install` does not appear in any site of
  this run although `guest-arena-test` faults inside it; what the census sees
  there is the canonicalizer's container frames and nothing above them.

**And what it cannot see at all** is a read, which is the thing that actually
faults. The list above pairs each site with a reader found by following the
code, not by measuring. Two consequences: a registry that is established and
read by the same isolate in every real workload would be on the list
wrongly, and one established outside any lock -- so already in the shared
heap, which `nolock` counts -- is on the list not at all, correctly.

**Is the list long enough to change the judgement?** No. Four of the six have
a fix already written, the fifth is an embedder allocator and the sixth is a
V8 patch of the same shape as the two that exist, and it is only reached by a
worker that uses wasm. The open-ended series is closed at six for this
workload. What is not settled is how many more workloads add to it: this one
does not exercise streams over RPC, hibernatable WebSockets, the shared memory
cache, Pyodide, `WorkerLoader`, replica storage, `AbortSignal` over RPC or
`setTimeout`, and the earlier run shows that a workload's reach is what the
enumeration is bounded by.

**The Cap'n Proto question, settled.** The cross-isolate free set is 37 sites
and 20,720 frees under the guest kernel, every one on the in-process call
path, and each RPC call accounts for about one free at every site. Of those 37
sites, **14 have `BuilderArena::allocate` or `MallocMessageBuilder::
allocateSegment` in their stack and 23 do not**: the others are
`TransientJsRpcTarget` and its `Handles`, the `JsRpcTargetBase` function box,
`IoContext::makeReentryCallback`'s lambda and its `kj::Deferred`,
`Capability::Client::makeLocalClient`'s `LocalClient`,
`MembraneHook::wrap` and `MembranePolicy::exportInternal` with the
membrane's `HashMap<ClientHook*, ClientHook*>` and its rehash buckets,
`LocalCallContext::getResults`'s `LocalResponse`,
`BuilderArena::LocalCapTable::injectCap`'s vector, and
`IoContext::PendingEvent`. So `BuilderArena::allocate` is **not** the right
choke point: a scope there would leave the majority of the set, including
every capability stub and membrane hook, in the caller's arena. The scope has
to span the call -- `LocalClient::call` and the `JsRpcTargetBase` and
`TransientJsRpcTarget` constructors -- which is what the earlier section
proposed and what this measurement confirms by counting what a narrower scope
would miss.

Without the guest kernel the same run has 47 cross-isolate-free sites rather
than 37, and the six extra are the owned-list nodes
(`hostMemory::allocate` from `OwnedObjectList::committed`, 2,240 frees, which
under gk come from `gk_host_alloc` and are not in the heap at all) and one
`v8::base::RegionAllocator` region-map node. The `RegionAllocator` and
`PooledPage` cross-isolate frees the earlier run found are **zero** under the
guest kernel here too, which is the per-`IsolateGroup` page allocator doing
its job.

### A turn can choose what the host casts to `InstructionStream`

One of the registries above is also a hole in the shipping build, and the
sharper of the two things being in the shared heap costs. V8's
`ThreadIsolation` JIT-page map answers "which code allocation is this PC in?"
for `Heap::GcSafeTryFindInstructionStreamForInnerPointer`, which is three
lines long:

    std::optional<Address> start = ThreadIsolation::StartOfJitAllocationAt(pc);
    if (start.has_value()) {
      return UncheckedCast<InstructionStream>(HeapObject::FromAddress(*start));
    }

The map is an ordinary `std::map` in the C++ heap -- the allocator V8's
`StlAllocator` reaches is `DefaultThreadIsolatedAllocator::Allocate`, which
is `return malloc(size)`, a stub V8 marks as one -- and in the default build
the C++ heap is one heap in the base root, mapped writably into every arena
root. So a turn with a write primitive can rewrite an entry, and the host's
next stack walk or deopt takes the address the turn chose and casts it to an
`InstructionStream` without a check. The callers are the deoptimizer
(`deoptimizer.cc`), `StackFrame::GcSafeLookupCode` and the profiler's
iterator (`frames.cc`), reloc-info and the diagnostics: every deopt and every
stack walk over JIT code.

This is reachable today, with no per-isolate heaps and no guest kernel,
because the map is already shared and already writable at ring 3. Per-isolate
heaps do not open it and placing the map in the shared heap does not open it
either; what they change is only that the map stays as exposed as it is now
while everything around it becomes private. The confidentiality half --
another isolate's code ranges and allocation sizes being readable -- is the
same, and under the guest kernel it discloses layout and not contents,
because a read into another arena still faults.

What would close the integrity half is a placement gk can already express but
nothing uses yet: a page mapped into every arena root readable and **not**
writable at ring 3. `GK_MAP_WALLED` is the mechanism -- `demand_map` already
drops the write bit per PTE for the host-frame wall and tags the page so that
`wall_lower` can give it back -- so a "shared, host-writable only" class is a
flag on the mapping rather than a new mechanism. It would cost a hostcall or
a host-side fixup for every write the host makes to such a page, which for
the JIT-page map is every code allocation, so it is not free. Recorded here
rather than built: the hole predates this work and closing it is a decision
about V8's bookkeeping, not about arenas.

### What a check can enforce, and what it cannot

The `workerd-memory-class` check cannot carry placement, and the reason is
not that it is unfinished. It decides by type, over the data members and
base classes of an annotated class; placement is a property of the code
path that allocates, and what the measurement says must be placed in the
shared heap is almost never an allocation of a class that could carry an
annotation. Of the 5,700 cross-isolate frees, about 1,900 are of a type
declared in workerd (`TransientJsRpcTarget` and its `Handles`, the
`JsRpcTargetBase` function box, the reentry callback and its `kj::Deferred`,
`PendingEvent`); the rest are `capnp::_::SegmentBuilder`, `MultiSegmentState`,
`LocalResponse`, `LocalClient`, `MembraneHook`, `kj::Vector` growth and
`kj::_::rehash` buckets, and the three classes of process-wide state are
inside ICU, inside V8 and inside KJ. None of those can be annotated here,
and even for the ones that can, an annotation that reached `operator new`
would place the object and not the transitive allocations its constructor
makes, which is what a scope does.

So the three layers under "Enforcing it" above divide differently than the
list there suggests. The static check keeps the type-level rules honest and
should not be extended to placement. Placement is enforced at run time, by
two things that cost almost nothing:

- **the hardware, for reads**: a cross-arena read is already a fault, and
  the fault reports the address and the faulting RIP;
- **a cross-arena-free counter, for ownership**: the allocator knows the
  freed block's arena and the thread's, so the comparison is free, and the
  census is the evidence that the counter is informative -- it is what
  found all four classes above. A per-arena build whose counter stays at
  zero over the suite is a placement claim that can be checked; one that
  does not is a list of sites to fix.

Measurement, the third layer, is what the census is: it has to come before
the placement rather than after it, because the sites that need a scope are
not derivable from the class rules.

### The smallest honest slice, and why it is not small

The `jsg::Object` family is the best candidate the census found: 232
`JSG_RESOURCE_TYPE` classes, one allocation choke point (`jsg::Lock::alloc`
and `jsg::wrapOpaque`, both reaching `kj::refcounted<T>`), 10.9% of what a
turn allocates, and in the measured run every free under the allocating
isolate's own lock. Placing it would need: a size-class allocator over a
region of the isolate's arena whose metadata is out of the arena, since a
free must not touch the block and the owner has to be derivable from the
address -- which it is, because gk places arenas in whole 512 GiB
page-table slots from `GK_FIRST_CAGE_SLOT` up, so the slot index names the
owner; `operator new`/`operator delete` on `jsg::Object`, so that every
subclass is placed by inheritance; and a negative build in which another
isolate reads a placed object and faults.

That is not a small change. It is a new allocator on the path of every JSG
wrapper, and the parked branch's two unresolved defects were both in an
allocator of exactly that kind (an extent jemalloc believed committed and
was not; ICU data outliving the arena it was placed in). It also does not
stand on its own: a placed `jsg::Object` whose members are `kj::Vector`s,
`kj::String`s and `kj::HashMap`s leaves those sub-allocations in the shared
heap, so the family is placed and the memory it owns is not, and the first
thing the negative build would measure is the reads it still allows rather
than the ones it stops.

The order the measurements imply is therefore the allocator first and the
families second, and in the allocator the three things the parked branch
did not have: the free path proven not to touch the block (66% of frees are
off any lock), the shared scope placed at the Cap'n Proto call rather than
at workerd's own objects, and the ICU and V8 `Intl` hooks in place before
any second `Intl` worker runs. Until those exist there is no slice that
demonstrates the property without carrying the whole mechanism, and the
honest step is the census itself, which turns each of the four classes from
an argument into a site list.

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
- **In gk's supervisor memory, mapped SUPER rather than REFUSE:** the ring-0
  resolve table (`gk_resolve_table` in `gk.c`), the one table a ring-3 turn
  reads through a ring-0 trap inside the guest rather than through the host;
  it holds the pointers every `IoOwn`'s handle names (`io/io-own.h`), with the
  entries a turn will need reserved ahead of it and committed after it from
  the turn's journal, and the host's record of what it reserved for each
  thread (`ThreadPool` in `util/host-memory.c++`) in H memory, found through
  gk's per-thread words. Each entry also records the page-table root it was
  registered under, which the ring-0 lookup compares against CR3, so an
  entry resolves at ring 3 only for the isolate it belongs to.
  `ioown-test.sh` shows a handle rewritten from inside
  a turn resolving to nothing, with the next turn's dereference and the
  host's delete-queue drain both refusing it, and a handle rewritten to
  another isolate's live handle refused while the host still resolves it.
- **In H memory, beside the resolve table:** the companions
  (`hostMemory::resolveCompanion`), one host-only record per entry in chunks
  mapped as entries come into use, reached through a host slot handle, and
  what they point at for `IoOwn`'s entries: the owned-list nodes
  (`OwnedObjectList` in `io/io-own.h`), each with its links, its disposer
  and its entry's and context's handles, and each context's record
  (`DeleteQueueOwner`), behind the context's own entry, holding the list's
  head and the delete queue -- its lock, the deletion handles and action
  boxes in chunks, and the cross-thread fulfiller -- with the registry lock
  that serializes a pusher's lookup against a teardown reached through a
  host slot handle. `ioown-test.sh` shows a turn's write into a node
  faulting at the node and the context's teardown walk disposing of the
  object through the node's own disposer, and a turn's write into a record
  faulting at the record with the host's teardown and later lockers
  unaffected.
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

## Why the neuterable stream wrappers are safe

Three sites in `api/global-scope.c++` wrap a stream whose lifetime belongs to
the HTTP or datagram layer rather than to workerd, so that workerd can
disconnect from it before it becomes invalid: `request()`'s request body, and
`connect()`'s connection and channel. Each neuters its wrapper from a
`kj::defer` and a `KJ_ON_SCOPE_FAILURE` in the same frame that calls the
worker's handler, and that frame runs in the guest. A turn that faults inside
the handler -- where a sandbox escape faults, as against the test hook, which
fires at the top of the function before the wrapper exists -- abandons both,
and the wrapper is left holding a raw pointer to a stream the owning layer
then frees.

Nothing dereferences it, and the reasons are worth stating because they are
invariants that other work can break, not properties of these three sites:

- The fault condemns the isolate, so no JS runs on it again. The readers the
  neuter exists to defend against are reachable only from the `Request` or the
  `Socket` on that isolate's heap, so they are unreachable rather than
  guarded. The leaked wrapper references, including those on the abandoned
  guest stack, are never dropped either, since nothing evicts a condemned
  isolate -- the wrapper leaks with a dangling pointer instead of being read.
- A turn holds the isolate lock from entry to fault without yielding, so no
  parked host-side read of the same stream resumes in between. The ones the
  `IoContext` owns are cancelled by its teardown, which runs as
  `WorkerEntrypoint`'s `co_await context.run()` unwinds -- before the fault's
  exception reaches the layer that frees the stream. Cancellation destroys the
  read rather than issuing a fresh one.
- The continuations that would read the body past the handler's own frame, the
  bidirectional-streaming `.then()` and the default handler's
  `adapter->request()`, are only constructed once `request()` returns a value.
  A faulted turn never returns one, so they never exist.

So these are latent hazards, not live bugs, and they need no fix today. What
they do need is for a change in any of the three areas above to be recognised
as reaching them: evicting condemned isolates, letting a turn yield the lock
before its fault, or arming a reader earlier than the handler's return would
each turn all three sites into host-side use-after-frees. The fix at that
point is an owner that outlives the turn -- `IoContext::addObject` on the
wrapper before the handler runs, as the memory cache's fallback callback does
-- and not neutering from the fault path, which sits in `io/` and has no
handle to per-request state in `api/`.
