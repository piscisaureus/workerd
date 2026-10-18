# gk: a guest-kernel isolation library (prototype)

`gk` runs the calling process inside a virtual machine as its own guest
ring 0 (the Dune model), forwards its syscalls to the host, and gives each
isolate a private memory arena backed by a separate guest page-table root. The
goal is per-isolate, hardware-enforced memory isolation with no fixed cap on
the number of isolates, using the host's hypervisor (KVM, or
Hypervisor.framework on macOS) rather than a bespoke one.

This is a research prototype. It runs on Linux on x86-64 or arm64 (see
[arm64](#arm64)), where it needs read/write access to `/dev/kvm` (no root, no
kernel module), and on macOS on Apple Silicon (see [macOS](#macos)). The
library builds and runs standalone with the included Makefile (`make run`); on
Linux it is also built as a Bazel `cc_library` and wired into the workerd
runtime behind the `WORKERD_EXPERIMENTAL_GUEST_KERNEL` environment variable,
which runs each isolate's JS turn at guest ring 3.

## Files

- `gk.h` — the library's interface.
- `gk.c` — the library, compiled as one translation unit that includes the
  parts below; each part compiles to nothing on a platform it is not for, so the
  backend, architecture and host OS are fixed at compile time and the parts call
  each other directly. `gk_internal.h` holds the state and declarations they
  share.
  - Shared by every platform: `gk_core.c` (`gk_init`, the run entry points,
    the host-frame wall, diagnostics), `gk_pt.c` (page tables, the
    supervisor/refuse registry, demand paging), `gk_region.c` (the regions of
    guest-physical backing), `gk_arena.c`, `gk_thread.c` (thread records, the vCPU pool),
    `gk_pkey.c` and `gk_util.c`.
  - Per architecture: `gk_x86_64.h`/`gk_x86_64.c` (x86-64, which is KVM only)
    and `gk_arm64.h`/`gk_arm64.c` (arm64 under either hypervisor).
  - Per host and hypervisor: `gk_linux.c` (Linux syscalls, `/proc/self/maps`,
    guest thread creation), `gk_kvm.c` (KVM memslots), `gk_kvm_arm64.c` (arm64
    under KVM) and `gk_hvf.c` (macOS and Hypervisor.framework).
- `gk_asm.S` — the in-guest trampolines and exception handlers, which includes
  `gk_asm_x86_64.S` or `gk_asm_arm64.S`.
- `gk_test.c` — the test/example.
- `Makefile` — `make run` builds and runs the test.
- `measure/` — the density measurements against a process per isolate:
  marginal memory per isolate, cross-isolate dispatch cost and cold start,
  each with the guest kernel off, on, and with one process per isolate; and
  the attribution of the guest kernel's per-request CPU cost (`stall.py`
  counts cycles, mispredicts, instruction fills and page walks per request
  on the event-loop thread, split into guest and host mode; `walk-cost.c`
  prices a nested page walk against a native one; `after-exit.c` prices what
  one `KVM_RUN` round trip does to the host code that follows it).
  `measure/findings.md` reports them and `measure/run-all.sh` reproduces
  them.
- `gk.entitlements` — the hypervisor entitlement the macOS binary is signed with.
- `fault-test.sh` / `fault-test.capnp` — end-to-end check against a built
  workerd that a fault inside the guest during a JS turn fails only that
  request and condemns only that isolate, and the process keeps serving.
- `fault-timeout-test.sh` / `fault-timeout-test.capnp` /
  `fault-timeout-test.js` — the same fault taken inside a `setTimeout`
  callback, the turn whose abandoned frames hold host state the host asserts
  on afterwards (the timeout's running mark, checked when the request's
  IoContext is destroyed). The faulted request fails, the isolate is
  condemned, the IoContext is torn down, and the process keeps serving.
  Negative build: `--copt=-DWORKERD_GK_NEG_TIMEOUT_RUNNING`, where the fault
  path leaves the mark standing and the process dies on the assert.
- `memory-cache-fault-test.sh` / `memory-cache-fault-test.capnp` /
  `memory-cache-fault-test.js` — the fault taken inside a `MemoryCache`
  fallback, whose frame holds the callback that releases the cache's
  in-progress entry for the key: a process-wide entry, since workers
  sharing a cache id share the cache, on which every later read of that key
  from any of them waits. The callback is the request's IoContext's before
  the fallback runs, so the teardown of the faulted request releases the
  entry, and a co-tenant worker's read of the same key completes with its
  own fallback. Negative build: `--copt=-DWORKERD_GK_NEG_CACHE_FALLBACK`,
  where the entry is stranded and the co-tenant's read hangs, which the
  script bounds and reports.
  On a build with per-isolate heap arenas the script fails at its second step,
  before the fault is injected: the cache instance is in the arena of the first
  worker whose bindings were compiled, and the peer's first read faults on it.
  `memory-classes.md`, "The shared memory cache", says why the shared heap is
  not where the cache goes and what placement is.
- `surface-test.sh` / `surface-test.capnp` / `surface-test.js` / `add.wasm` /
  `oob.wasm` — end-to-end check against a built workerd that a broad V8 surface
  (Wasm including an out-of-bounds trap, WebCrypto, irregexp and a JIT-heavy
  loop) runs correctly with the worker's JS turn at guest ring 3, and that a
  Wasm bounds violation surfaces as a `WebAssembly.RuntimeError` rather than a
  guest fault.
- `host-memory-test.sh` / `host-memory-test.capnp` — end-to-end check against
  a built workerd that the channel token key and the host entropy buffer live
  in host memory, that a JS turn overwriting the handle the host reaches the
  key through leaves the host refusing the use rather than reading a key of
  the turn's choosing, that a JS turn overwriting the thread-local entropy
  buffer decides what the host's next `getEntropy()` draw returns but not the
  nonce of the token the host mints, and that a JS turn at guest ring 3
  reading the key or the buffer faults at the address read, failing only that
  request and condemning only that isolate.
- `actor-id-test.sh` / `actor-id-test.capnp` / `actor-id-test.js` — end-to-end
  check against a built workerd that a Durable Object namespace's HMAC key
  lives in host memory while the ids a JS turn mints with it, through a
  hostcall, are distinct, derive from the configured key (for a short name and
  for one longer than a hostcall frame), address working Durable Objects, and
  that a forged id is refused and a ring-3 read of the key faults at the
  address read.
- `wasm-signal-test.sh` / `wasm-signal-test.capnp` / `wasm-signal-test.js` /
  `signal.wasm` — end-to-end check against a built workerd that the Wasm
  shutdown signal, written from a CPU-time timer's signal handler, reaches a
  module whose JS turn runs at guest ring 3, and that a turn overwriting the
  module's heap descriptor with a buffer of its choosing leaves the signal
  landing in the module's real memory and the chosen buffer untouched.
- `tls-test.sh` / `tls-test.capnp` — end-to-end check against a built workerd
  that the guest-writable thread-locals the host reads after a JS turn do not
  carry a turn's value past it: a turn overwrites, from inside itself, the heap
  slot the isolate lock restores `Worker::Api`'s thread-local from, and the
  host still leaves the thread-local holding the value it had before the lock,
  taken from a host stack frame and not the overwritten slot.
- `lock-slot-test.sh` / `lock-slot-test.capnp` — end-to-end check against a
  built workerd that the isolate's lock slot, the V8 data slot `jsg::Lock::from`
  reads, which is in the C++ isolate object in the shared heap and so writable
  from any isolate's turn, does not hand the host a lock of a turn's choosing:
  a turn overwrites its own isolate's slot from inside itself, and another
  isolate's turn overwrites it from inside its own, and in both cases host code
  holding the lock after the turn gets its own lock from `jsg::Lock::from`, the
  slot is clear once the lock is released, and no lock is refused as recursive.
- `disk-sqlite-test.sh` / `disk-sqlite-test.capnp` / `disk-sqlite-test.js` —
  end-to-end check against a built workerd that a SQLite-backed Durable Object
  stored on local disk works with its JS turns at guest ring 3, where SQLite's
  native VFS runs inside the turn and its file syscalls pass the syscall
  filter: rows are written and read back across requests and a restart, and
  an error raised inside a SQLite turn -- caught in JS, uncaught, or one the
  runtime logs inside the turn with its stack trace, which would run the
  symbolizer -- fails only its request rather than faulting the guest.
- `vfs-root-test.sh` — end-to-end check against a built workerd that a JS turn
  rewriting the directory descriptor SQLite's native VFS uses for its path
  syscalls (a thread-local the turn can write) does not redirect where a
  Durable Object's database lands: the open is refused under the turn
  directory and nothing appears in the directory of the turn's choosing,
  while with the confinement switched off (a test-only switch) the same
  rewrite puts the database there.
- `ioown-test.sh` / `ioown-test.capnp` — end-to-end check against a built
  workerd that an `IoOwn` (`io/io-own.h`) names its object by a handle the
  host validates, and that the record behind the handle is out of a turn's
  reach: a JS turn that rewrites the handle inside its own object, to the
  address of a buffer the turn chose, leaves the next turn's dereference and
  the host's drain of the delete queue refusing the handle rather than
  reaching the buffer, with the isolate still serving; a handle rewritten to
  another isolate's live handle names that isolate's object, the ownership
  residual the table cannot close, stated as such; the same stolen handle
  dropped -- the holder destroyed inside a turn, which asks the host to
  retire that isolate's entry and unlink the node behind it -- refused by
  the journal flush, leaving the entry naming its object, the node linked
  and the other isolate dereferencing it afterwards; a handle written over
  another of the *same* isolate's, naming an object of an unrelated class
  with a virtual of its own, refused at the use -- so the call never reaches
  the other class's vtable -- and at the drop, leaving that object with its
  own holder; a turn's write into
  the owned-list node behind a handle -- the links the host's walk follows
  and the disposer it calls, in host memory -- faults at the node, after
  which the context's teardown, host-side, disposes of the object through
  the disposer the node recorded and never the one the turn chose; a turn's
  write into the context record behind an `IoOwn`'s other handle -- the
  delete queue, with its lock and the fulfiller the host calls through --
  faults at the record, after which the teardown locks the record and the
  host's later lockers are unaffected; and an `IoOwn`'s context handle
  rewritten to another isolate's live context, or to a buffer the turn
  chose, queues nothing when the holder is dropped, with the object left to
  its own context's teardown.
- `rpc-context-test.sh` / `rpc-context-test.capnp` /
  `rpc-context-caller.js` / `rpc-context-callee.js` — end-to-end check
  against a built workerd that a worker whose `IoContext` was built inside
  another worker's JS turn owns the objects it registers against that
  context. A subrequest builds the callee's context from the caller's turn
  (`IoContext::getSubrequestChannel`), so the context's entry comes out of
  the caller's journal; registered under the caller's arena it would make
  every owned object the callee registers against its own context name a
  context of another isolate, which the commit hook refuses with the object
  already handed over, and would make the callee's own turns resolve their
  context's handle to nothing. The entry is therefore owned by no arena
  until the isolate it belongs to adopts it host-side (`gk_resolve_adopt`).
  100 cross-worker RPC calls and 20 service-binding fetches must refuse no
  registration and leave no owned node or context record behind; the stubs
  the RPC calls dispose of inside the caller's turn must still reach the
  cross-arena case, counted so that the check cannot pass by not happening;
  and an owned object whose registration is refused must be disposed of by
  the refusal rather than left owned by nothing.
- `abort-signal-test.sh` / `abort-signal-test.capnp` — end-to-end check
  against a built workerd that an `AbortSignal`'s native abort registration
  (`api/basics.h`) keeps the host out of the signal's own memory: the record
  holding the abort action is the signal's and is read, written and destroyed
  only under the isolate lock, while the consumer's RAII handle owns nothing
  but an entry in the resolve table, which it retires when it is dropped. A
  JS turn rewrites the disposer word of a live action's `kj::Function`, as a
  write primitive would, and the handle is then dropped on the event loop,
  outside the guest, where a `fetch` with a signal drops it: the chosen
  disposer is never called, the action is still the signal's and the entry no
  longer resolves. A registration whose handle is still held runs its action
  on abort; one whose handle was dropped does not.
- `log-flood-test.sh` / `log-flood-test.capnp` — end-to-end check against a
  built workerd that a worker cannot drive the process's log output by
  forcing the refusal paths. One request journals 20000 notes of a kind the
  host does not know, which the host reads and refuses after the turn;
  another releases 5000 holders at ring 3 whose handle names an entry of
  another class, refused in the turn's own code. The reports are budgeted
  per site and per second out of gk's control block, so the 45000 refusals
  draw two dozen lines rather than 45000; the first report of each kind is
  still made, and one in the next window carries the number held back.
  Negative build: `--copt=-DWORKERD_GK_NEG_LOG_CAP`, where every report is
  made and the script fails on the volume. Worth running against the
  handler-allocation guard build too, where the reports the refill hostcall
  finds would otherwise format inside a handler;
  `--copt=-DWORKERD_GK_NEG_REPORT_DEFER` puts that back and aborts it.
- `body-arena-test.sh` / `body-arena-test.capnp` / `body-arena-front.js` /
  `body-arena-back.js` — end-to-end check against a workerd built with
  per-isolate heap arenas that request and response bodies cross an
  in-process service binding intact, with neither isolate's turn reaching the
  other's arena: a string body and a JS-implemented stream body read whole by
  the callee, a response body read whole by the caller, and a stream body the
  callee returns unread as its response, each at 1000 bytes and at 300000;
  and a streaming upload aborted while the callee is blocked in a read of it,
  which tears the body pump down inside the caller's turn. Negative build:
  `--copt=-DWORKERD_GK_NEG_BODY_RELAY`, where the bodies cross through the
  bare pipe and the first route faults the reading isolate's turn.
- `binding-crossing-test.sh` / `binding-crossing-test.capnp` /
  `binding-crossing-front.js` / `binding-crossing-back.js` — end-to-end check
  against a workerd built with per-isolate heap arenas of the crossings of an
  in-process service binding that carry no body: the `cf` blob a request
  brings beside the HTTP interface, on `bench/bench.capnp`'s cdn pair (served
  as is: the incoming `Request` forwarded to an origin and its response passed
  through as the init of a new one) and on a fetch init; and the error a
  callee that throws or rejects hands back, over fetch and over JS RPC, each
  followed by another request of the caller's. Negative builds:
  `--copt=-DWORKERD_GK_NEG_CF_BLOB_HEAP`, where the blob stays in the
  caller's arena and the origin's turn faults reading it, and
  `--copt=-DWORKERD_GK_NEG_EXCEPTION_HEAP`, where a callee's exception stays
  in its arena and the caller's turn faults converting it.
- `heap-region-test.sh` / `heap-region-test.capnp` / `heap-region-front.js` /
  `heap-region-back.js` — check against a workerd built with per-isolate heap
  arenas that an isolate serving large response bodies does not spend its heap
  arena region, which is a fixed 16 GiB reservation an isolate is condemned
  for outgrowing. One worker serves 4 MiB bodies from a buffer it keeps and
  another reads them whole across a service binding, three thousand times,
  and the script asserts that both isolates' regions plateau (the offsets come
  from the `__gk_heap_region` probe route in `api/global-scope.c++`) besides
  that nothing faults. What it guards is the report of a buffer-built body's
  copy to V8 as external memory and the collection jsg runs at a soft limit's
  worth of such growth (`ExternalMemoryTarget` in `jsg/jsg.c++`): without
  them the copies, held until their Response is collected, take about 4 MB of
  region per response until a collection V8 finds some other reason for.
  Negative build: `--copt=-DWORKERD_GK_NEG_BODY_BUFFER_UNACCOUNTED`, where
  the copies go unreported and the serving isolate's region grows by
  gigabytes over the run.

## API

```c
int   gk_init(void);                         // become a guest-capable VM
long  gk_run(long (*fn)(void*), void *arg);  // run fn in the guest, forward syscalls

gk_arena *gk_arena_create(size_t size);      // a private per-isolate cage
void     *gk_arena_base(const gk_arena*);
gk_arena *gk_arena_enter(gk_arena*);         // select this thread's active cage (NULL = base)
void      gk_arena_destroy(gk_arena*);

void gk_set_syscall_filter(gk_syscall_filter); // bound what forwarded code may do
int  gk_mapping_syscall_ok(long nr, long a1, ..., long a6); // a filter's range check
                                                            // for the memory syscalls

void *gk_host_alloc(size_t);                 // host memory: mapped into no guest root
void  gk_host_free(void*, size_t);
gk_host_handle gk_host_slot_alloc(size_t);   // a host-memory block named by a handle
void *gk_host_slot_get(gk_host_handle);      // validated against gk's table; NULL otherwise
void  gk_host_slot_free(gk_host_handle);

int   gk_hostcall_register(unsigned idx, gk_hostcall_fn, void *ctx); // a host handler the guest may call
gk_hostcall_frame *gk_hostcall_region(void); // the thread's exchange frame: args in, result out
long  gk_hostcall(unsigned idx);             // call handler idx synchronously (one VM exit)

gk_resolve_handle gk_resolve_register(void *ptr); // a pointer named by a handle, in gk's supervisor table
int   gk_resolve_unregister(gk_resolve_handle);
void *gk_resolve(gk_resolve_handle);         // handle -> pointer or NULL; at ring 3 a ring-0 trap
                                             // inside the guest, no VM exit (about 27 ns), and
                                             // only for the arena the entry was registered under

int   gk_signal_field_register(uintptr_t off, size_t len, uint32_t field, unsigned kind,
                               gk_signal_handle *h); // a word in the thread's arena a signal handler may write
int   gk_signal_field_unregister(gk_signal_handle);
long  gk_signal_field_write(unsigned kind, uint32_t value); // from a signal handler: no lock, no allocation

int   gk_thread_words_set(unsigned first, unsigned n, const uint64_t *v); // per-thread words in supervisor memory
int   gk_thread_words_get(unsigned first, unsigned n, uint64_t *v);       // for state a signal handler reads
long  gk_log_budget(unsigned site);          // whether a refusal path may report, counted in the control block
int   gk_in_guest_hint(void);                // gk's own in-guest flag; a hint, never trusted

void  gk_confine_paths(int on);              // bound a confined turn's path syscalls to its turn directory
int   gk_turn_dir_set(int dirfd);            // set this thread's turn directory (host-side)
```

`gk_run` returns `fn`'s return value, or `GK_EFAULT` if the guest took an
unhandled fault (for example an access to an inactive arena); `gk_fault_addr()`
then gives the faulting address.

## What works

`make run` builds and runs `gk_test`, which checks the following on real
hardware:

- **Enter and leave the guest.** A pure computation runs in guest ring 0 and
  returns its result.
- **Syscall forwarding.** A raw `syscall` in the guest is trapped by an
  in-guest trampoline, hypercalled to the host, executed there, and resumed.
- **Real libc in the guest.** A function that calls `malloc`, `snprintf` and
  `write` runs unmodified inside the guest. `malloc`'s `mmap`/`brk` are
  forwarded and the new memory is mapped into the guest on the fly; TLS works
  because the guest FS base is set to the host's.
- **Arena isolation.** Two arenas act as two isolates. An arena is a large
  `PROT_NONE` reservation spanning one or more aligned 512 GiB page-table slots,
  with its own root that shares the runtime's subtrees but owns those slots
  privately. Its memory is demand-paged into the arena's root only while the
  arena is active, with the protection the owner has committed (via forwarded
  `mprotect`) at that moment; uncommitted pages fault. Under arena A's root, B's
  memory is unmapped, and from the base root all arena memory is: a guest access
  to another arena's range faults at that address before the host mapping is even
  consulted. A decommit is reflected into every arena root, so a stale writable
  entry never survives. A decommit the guest never sees (issued from the host
  side, by a thread outside the guest) leaves the entry in place, and the
  guest's next access fails in `KVM_RUN` instead; gk drops the arena's entries
  and restarts the access, so it faults at the page like any other genuine
  fault, and the thread's next turn runs normally. This is the isolate-cage
  primitive; one arena per V8 isolate holds that isolate's sandbox/cage.
- **Ring-3 execution and supervisor control data.** `gk_run_user` /
  `gk_run_here_user` run untrusted code at guest ring 3, where it cannot execute
  privileged instructions or reload `CR3`. Everything ring 0 relies on is kept
  out of its reach by a page-class registry consulted whenever a page is mapped:
  supervisor pages (handler text, GDT/IDT, exception stacks) are mapped with the
  user bit clear, and refused pages (the page tables, `kvm_run`, gk's host-side
  stacks) are never mapped at all. gk's own bookkeeping -- the arena registry and
  structs, the page-table allocator, the memslot tree and its node pool, the
  registry itself, the vCPU pool and the per-thread records -- lives in one
  page-padded supervisor control block, and a thread's record is found through a
  thread-local index that is validated against the thread's kernel tid, so a
  ring-3 escape with an arbitrary write can neither rewrite the walls nor
  redirect a thread to another root. The test writes to each structure from
  ring 3 and checks the fault, then reads and writes from ring 0.

  This is the extent of the guarantee: cross-arena isolation, gk's own
  control state and host memory (next) hold against arbitrary code execution
  inside an isolate. The rest of the shared, non-arena runtime memory does
  not: the C++/KJ heap, glibc, and every vCPU's guest stack (including the
  caller's stack that `gk_run_here_user` runs the turn on) are user-mapped and
  readable and writable from ring 3. A sandbox escape that reaches arbitrary
  code execution can therefore corrupt shared runtime state and, through a
  host return address on that shared stack, may reach host execution. Moving
  the runtime's own mutable state into host memory is remaining work (see
  Limitations).
- **Host memory.** `gk_host_alloc` hands the host side of the runtime memory
  that no guest page-table root maps: the host reads and writes it as
  ordinary memory, and a guest access at any ring -- ring 3 or ring 0, inside
  an arena or on the base root -- faults at the accessed address, since the
  pages are registered in the refuse class that also keeps gk's page tables
  and `kvm_run` from the guest. Regions of a few MiB are mapped and registered
  once each (the registry is fixed-size and fatal when full, so it is never
  spent per block) and suballocated by power-of-two size class through free
  lists kept inside the refused pages; blocks come back zero-filled and are
  scrubbed when freed. The test writes and reads a block from the host, shows
  every kind of guest access faulting at the exact address with the host's
  bytes intact, and runs tens of thousands of allocate/free cycles and
  multi-region bursts without the registry growing past the regions. In
  workerd, `util/host-memory.h` wraps this (`HostOwn<T>`, ordinary heap memory
  when the guest kernel is off).
- **Host slots.** A block in host memory is out of the guest's reach, but a
  pointer to it is only as safe as the memory holding it, and an owner in the
  ordinary heap is memory a write primitive can rewrite, after which the host
  reads and writes through whatever address it finds. `gk_host_slot_alloc`
  therefore names a host-memory block by a handle instead: a fixed-capacity
  table (`GK_HOST_SLOTS`, 4096 entries) in the supervisor control block maps a
  small integer to the block, and `gk_host_slot_get` validates a handle
  completely against that table -- index bounds, slot liveness and a
  generation that every allocation takes fresh from a counter -- before
  returning the block, reading nothing through the handle's value. A handle
  gk did not issue, a handle to a freed slot, and the old handle of a slot
  since recycled all resolve to nothing, reported on stderr; a full table is
  reported as a failed allocation, never fatal. The test resolves a handle
  and reads and writes the block from the host, shows ring 3 faulting on
  both the block and the table, feeds the resolver thirteen handles it did
  not issue -- including the address of a `PROT_NONE` page, which any read
  through the value would turn into a crash -- and gets nothing back with the
  caller's memory untouched, frees and recycles a slot under a new handle
  while the old one stays dead, fills the table to its bound, has the next
  allocation refused, and sees the protection registry grow by at most one
  region rather than per slot. What the table cannot tell apart is a handle
  rewritten to another live tenant's: that names that tenant's block, the
  same ownership bound `gk_arena_enter` and `gk_arena_destroy` carry (see
  Limitations). In workerd, `HostSlot<T>` (`util/host-memory.h`) owns a
  host-memory object through such a handle, which is its only member, with a
  table of the same shape on the ordinary heap when the guest kernel is off;
  the channel token key and the host entropy buffer are held this way.
  `host-memory-test.sh` shows a handle rewritten from inside a turn resolving
  to nothing, the host refusing the use instead of reading the turn's buffer,
  and a ring-3 read of the key, and of the buffer, faulting.
- **Host entropy.** A secret in host memory is only as good as the generator
  that produced it, and workerd's `getEntropy()` buffers 4 KiB of unconsumed
  CSPRNG output in thread-local storage, which is shared runtime memory a
  ring-3 turn can read and write: the bytes the thread's next draw returns
  are the turn's to read or to choose. That is harmless for the draws a turn
  makes for itself, whose bytes go to the isolate anyway, and fatal for a
  nonce: AES-GCM under a fixed key with a repeated nonce gives up its
  integrity guarantee, so a turn that could choose the nonce of the next
  channel token could forge tokens without ever reading the key. Host-side
  secrets -- the channel token key and the nonce of every token minted with
  it -- are therefore drawn with `getHostEntropy()` (`util/host-entropy.h`),
  whose buffer, cursor and lock are one host-memory block reached through a
  host slot handle, and which refills from the operating system's generator
  (`getrandom(2)`) rather than from BoringSSL's in-process DRBG, whose
  per-thread state is in the shared heap too and, on a CPU without a fast
  RDRAND, determines the DRBG's output outright. The in-turn draws keep
  `getEntropy()` and its thread-local buffer, since routing bytes the isolate
  receives anyway through host memory would cost a crossing for nothing.
  `host-memory-test.sh` overwrites the thread-local buffer from inside a turn
  with a pattern and shows the host's next `getEntropy()` draw on that thread
  returning the pattern and the token the host mints carrying a nonce that is
  not.
- **Hostcalls.** A secret in host memory is of no use to a runtime operation
  that needs it inside a turn, and some of those are synchronous to JS -- a
  Durable Object namespace's `newUniqueId()` and `idFromName()` return the id
  directly -- so they cannot be deferred to the event loop as the token key's
  readers are. `gk_hostcall` is the synchronous crossing: the guest issues a
  reserved syscall number with a handler index in `RDI`, which gk's syscall
  exit answers itself, before the filter and before any forwarding, by
  running the handler the host registered under that index and resuming the
  guest with its result. The index is checked against a fixed table of
  `GK_HOSTCALLS` entries in the supervisor control block, so the guest can
  name nothing but a registered small integer: an index at or past the
  table, an empty entry, and any index while nothing is registered all
  return `-ENOSYS` with nothing called. Arguments and results cross through
  a per-thread exchange frame in a reservation gk owns, which the mapping
  policy refuses to let the guest unmap or reprotect; before the handler
  runs, gk reads the frame's length once, refuses one past the frame unread
  (`-E2BIG`), and copies the arguments into a second frame in host memory,
  which is all the handler sees, so a handler is never handed a pointer the
  guest chose and no byte it reads can change under it. The same function
  on the host side runs the handler directly through the same table, so an
  embedder calls one function from either side. The test registers a
  handler, calls it from ring 3 on both kinds of stack, from ring 0 and from
  the host, checks that the handler saw exactly the arguments the guest
  passed and the guest got exactly the handler's bytes back, feeds it the
  unregistered, at-table, past-table, all-ones and pointer-shaped indices
  and a length past the frame -- with every argument and the whole buffer
  set to the address of a `PROT_NONE` page -- and gets the refusal each
  time with the handler's call count unchanged, shows a ring-3 write to the
  table faulting and the region unmappable by the guest, unregisters and
  sees the index refused again, and times the round trip: **6.4 us** per
  call on the test machine, one VM exit plus a trivial handler. The guard
  build runs the whole path with allocation forbidden. In workerd,
  `util/hostcall.h` wraps this (a direct call when the guest kernel is off),
  and `ActorIdFactoryImpl` (`server/actor-id-impl.h`) is its first user:
  the namespace key is in host memory behind a host slot handle, every
  keyed operation -- the HMAC of a name, the HMAC of an id's first half, the
  draw of a unique id's random half from `getHostEntropy()` -- runs in the
  handler host-side, and a name longer than one frame crosses in chunks with
  the running HMAC kept in the frame's host-side state. `actor-id-test.sh`
  shows the ids a turn mints being right and a ring-3 read of the key
  faulting. What the primitive does not give: a handler runs with host
  privilege whenever ring 3 asks, with arguments of ring 3's choosing, so
  every handler is an attack surface that validates its own inputs, and a
  handle swapped for another live tenant's is one it cannot tell from the
  owner's own.
- **Ring-0 resolve.** The hostcall's exit prices it out of anything a turn
  does often, and the next crossing the runtime needs is exactly that: an
  isolate-side object that holds a host object by handle, and resolves the
  handle on every dereference, as `IoOwn` would once it stops holding a raw
  pointer. The answer cannot come from memory the guest can write, since a
  table ring 3 can read is a table ring 3 can rewrite, so it comes from a
  table in supervisor memory, read at ring 0 inside the guest. `gk_resolve`
  is that crossing: at guest ring 3 it issues a reserved syscall number with
  the handle in `RDI`, and gk's SYSCALL trampoline, in the guest at ring 0,
  recognizes the number before the hypercall that would leave the VM, looks
  the handle up in `gk_resolve_table` and SYSRETs back with the pointer, or
  0, in `RAX`: a few dozen instructions of gk's own handler text, touching
  no stack and nothing but the table, with the VM never exited. Which path a
  caller takes comes from its CS selector -- only the ring-3 code selector
  gk's GDT hands out traps; guest ring 0 and the host read the table
  directly, with the same validation -- so nothing in memory decides it. The
  operation is fixed rather than a registered handler, because embedder code
  at guest ring 0 would need its data mapped supervisor, where any ring-0
  code the guest reaches could read it: the only thing that runs at ring 0 is
  gk's lookup. The table is `GK_RESOLVE_SLOTS` (2^20) entries of `{pointer,
  generation, owner, chain, type}`, registered `GK_PROT_SUPER` -- a weaker class than the
  `GK_PROT_REFUSE` of host memory and the page tables, since a SUPER page is
  in every root with `U` clear while a REFUSE page is in none, and the right
  one here because the lookup has to run inside the guest; host memory stays
  on REFUSE -- and a handle is validated as a host slot handle is: the index
  is the handle's low 20 bits, which the capacity of exactly 2^20 bounds by
  masking, the entry must be live and its generation the handle's, so a
  handle to an unregistered or recycled entry resolves to nothing and
  nothing is ever read through the handle's own value. The generation's
  counter has 44 bits and wraps back to 1, so generations do eventually
  repeat: a handle retired before a wrap resolves again if the wrap lands
  the repeated generation on the slot it named. 2^44 registrations is about
  two hundred days at a million a second and years at the rates a worker
  registers at, and the revived handle must still be held and must still
  pass the owner check, so the window is narrow -- but the counter is not
  endless, and the comment that once said a generation is never reused was
  wrong. The owner says whose
  entry it is: the page-table root the host registered it under, compared at
  ring 0 against `CR3`, the root the calling turn runs under, so an entry
  resolves at ring 3 only for the isolate it belongs to. `CR3` is the one
  caller identity at ring 0 that is not memory -- ring 3 can neither read it
  nor load it, and the host alone decides which root a turn enters with --
  so a turn that rewrites its handle word to another isolate's live handle
  gets 0, the answer a stale handle gets, and not that isolate's object. No
  entry outlives its arena, and gk enforces that rather than assuming it: the
  tag is a root, a destroyed arena's root goes back to the page-table
  allocator, and the next `gk_arena_create` is handed exactly that page (the
  root is its first allocation and the allocator's free list is LIFO), so an
  entry left behind would carry the new isolate's `CR3` as its tag and
  resolve at that isolate's ring 3 -- the cross-isolate read the tag exists
  to refuse, with no race in it. Each arena carries the list of its entries
  and a count of them in gk's control block, and `gk_arena_destroy` retires
  whatever is still on the list in the same critical section that frees the
  root, reports the count on stderr and adds it to
  `gk_stats.resolve_orphans`. A nonzero count is an embedder bug rather than
  a gk one: an object registered under an isolate has outlived that isolate.
  The runtime's entries go with the objects that hold them, and the isolate
  outlives every context that registers one, so the count should stay 0. The
  The owner says whose entry it is; it does not say what the entry is, and
  a handle rewritten to another live entry of the *same* isolate resolves,
  after which the caller casts the pointer to the class it expected. So the
  entry carries a type word as well: a constant the registrant derives from
  the C++ class (`hostMemory::resolveTypeTag`, an FNV-1a hash of the class's
  spelling, folded at compile time), which the lookup hands back beside the
  pointer -- at ring 3 in `RDX`, which costs the instruction that otherwise
  zeroes it, leaving the trampoline at the 26.8 ns it cost before -- for the
  caller to
  compare against the class it was built for. `IoOwn<T>`, `IoPtr<T>` and
  `ReverseIoOwn<T>` register and resolve under their own `T`, and refuse a
  handle naming another class at the use and at the drop, where releasing
  would free an object its real holder still names. The comparison is the
  caller's rather than the trampoline's because the runtime's text is mapped
  without `W` and `gk_mapping_change_ok` will not let a forwarded `mprotect`
  make it writable, so a comparison against a constant the compiler
  materializes is one a turn with a write primitive can neither rewrite nor
  edit the expectation of. What it is not proof against is a turn that
  reaches control flow of its own inside the runtime, which skips it as it
  would skip any check the runtime makes -- the owner, compared at ring 0,
  still holds there -- nor against a swap between two live entries of the
  same class, which no tag can tell apart. The tag is also the turn's own
  word for a registration it journals, so it binds a turn that swaps handles
  and not one that lies about its own registration; that one waits on the
  class word being recorded where a turn cannot write it, as the disposer
  beside it does. Giving the journal pages of its own does not do that: ring 3
  writes the journal on every turn, which is what it is for.
  Tagging is a hash, so two classes whose spellings collide share a tag,
  which costs a catch and never a false refusal.

  The retire side needs the same question answered and cannot ask it the
  same way. A drop is not a use: a holder's destructor releases its entry
  whatever the resolve answered, and inside a turn that release goes into
  the turn's journal, which is the turn's own memory, for the host to act on
  once the turn has returned -- host-side, where the lookup compares no
  owner and must not, since the host resolves for whichever context it is
  serving. A successful lookup is therefore not evidence that the handle was
  the caller's, and a host path that retires an entry, or follows it, on one
  isolate's behalf has to name that isolate: `gk_resolve_owned_by` answers
  whether an entry is a given arena's and `gk_resolve_unregister_for`
  retires one only if it is, while `gk_resolve_unregister` stays the
  unchecked retire for teardown the host does on its own account. Every
  handle the host takes out of a turn's journal -- the retire list, the
  notes, the context an owned object's registration names -- is checked
  against the arena the turn ran under, which the host records at the turn's
  entry from gk's record of the thread
  (`hostMemory::ownedByTurn`). The comparison is against `CR3` as the
  register reads, which is the root's address and nothing else only while
  `CR4.PCIDE` is clear, as gk leaves it; whoever enables PCID must mask the
  trampoline's comparison down to the root bits first, since `CR3` would
  then carry the PCID in bits 11:0 and the no-flush bit in 63. The
  base root is a root like any other rather than a wildcard, so an entry
  registered with no arena active resolves at ring 3 only on the base root;
  a host thread that registers an entry for an isolate it is not locked into
  names the isolate instead (`gk_resolve_register_for`,
  `hostMemory::registerResolveFor`, used for a context's own entry, which is
  usually created on the event loop).

  A turn cannot name an isolate that way, and sometimes has to: a subrequest
  builds the callee worker's `IoContext` inside the calling worker's turn
  (`IoContext::getSubrequestChannel`), so that context's entry comes out of
  the caller's journal and would be owned by the caller's arena -- leaving
  the callee's own turns unable to resolve their own context's handle, and
  every owned object they register against it refused by the commit hook
  with the object already handed to the table and so lost. Nothing in a
  turn's reach can say which isolate the context belongs to: the arena, the
  `Worker::Isolate` it would be read from and the handle itself are all in
  the shared heap a turn can write, and gk validating an arena pointer
  against its pool only narrows a forged value to *some* live arena. So the
  entry names no arena at all. The commit hook that builds the context's
  record leaves it owned by the base root (`gk_resolve_disown`), where it
  resolves for no ring 3 -- not the turn that made it either -- and the
  isolate it belongs to claims it host-side, on its own thread, under its
  own lock, before its first turn (`DeleteQueueOwner::adoptEntry`,
  `gk_resolve_adopt`); the arena comes from gk's record of that thread, as a
  turn's own does, and is never named. What keeps one caller from claiming
  another's entry is the pointer: the entry must hold exactly the pointer
  the caller passes, and a `DeleteQueueOwner` passes its own address, so a
  handle rewritten to another context's entry finds that context's owner
  there and re-owns nothing. Guest ring 0 and the host are not compared:
  host code resolves on behalf of whichever context it is serving, from
  threads that are usually on no arena at all. The lookup takes no
  lock and writes nothing: it reads the generation on both sides of the
  pointer and answers only if both match, and the host publishes an entry
  pointer-first and retires it generation-first, so a pointer seen between
  two matching reads was the entry's under that generation. Exhaustion is a
  registration refused with 0, never fatal. The tenant is `IoOwn`
  (`io/io-own.h`), with `ReverseIoOwn` and `IoPtr` beside it: the handle a
  holder keeps names the object, and every dereference, destruction and
  release resolves it. The entry holds the object's pointer, which the turn
  resolves; what the host keeps about the object -- its node in the
  context's owned list, with the links the host's walk follows and the
  disposer the host calls -- is in host memory, found through the entry's
  companion (`hostMemory::resolveCompanion`), a host-side record per entry
  that the host alone writes. The context that owns the object has an entry
  of its own, whose handle is the holder's other word (`DeleteQueue`) and
  whose companion is the context's record in host memory: the list's head
  and the delete queue -- the handles of holders dropped off their context,
  the actions other contexts send it, the signal that wakes it, and the
  lock over them -- so that no refcount or mutex of the queue's is in
  memory a turn can write; a turn that drops a holder off its context, or
  sends another context an action, leaves a note in its journal
  (`hostMemory::noteInTurn`) that the host validates and queues after the
  turn. The objects are created inside turns, where
  neither the table nor host memory can be written, so the host reserves
  entries for the thread before each turn and commits them after it from
  the turn's journal (`gk_resolve_reserve`, `gk_resolve_commit`;
  `ResolveTurn` in `util/host-memory.h`), building each node from the
  journal's words as it commits: a commit takes once, only into a reserved
  entry, and only a pointer to memory the entry's owner can reach -- outside
  gk's supervisor and refused ranges, and outside every arena but the one the
  turn ran under, which is where the objects that isolate registers live -- so
  a turn can launder nothing into the table that it could not already write
  directly, and cannot aim the host at gk's state, host memory or another
  arena. The
  capacity is set from the measured live count: five entries per in-flight
  request parked on a timer, eight per open WebSocket (memory-classes.md,
  Cost), so 2^20 holds about two hundred thousand such requests, in 32 MiB
  that cost nothing until written (an entry is 64 bytes: the pointer, the
  generation, the owner, the chain word that puts it on its arena's list and
  the type word, with the shift the trampoline indexes by and one cache line
  per entry deciding the rest). The test registers an entry and resolves it
  from ring 3 on both kinds of stack, from ring 0 and from the host; feeds
  the lookup a zero handle, an index at and past capacity, generations never
  issued, a slot never taken, the pointer itself, a heap address, a
  `PROT_NONE` page's address and all-ones, and gets nothing each time with
  the caller's memory untouched; shows a ring-3 read and write of the table
  faulting at the table; unregisters and recycles the slot and shows the old
  handle still resolving to nothing; fills the table to capacity, sees the
  next registration refused and every entry resolve to its own pointer from
  ring 3, and empties it; and times the round trip in sub-runs of 8000
  ring-3 resolves, each bracketed by a read of KVM's own per-vCPU `exits`
  counter (`KVM_GET_STATS_FD`, which counts the exits the kernel services
  without returning to gk): a kept sub-run is one whose counter moved by
  exactly the one exit that ends the turn, so its 8000 resolves provably
  stayed inside the guest, and the sub-runs that moved by more were host
  timer interrupts (`irq_exits`) and are discarded. **27.7 ns per resolve**
  on the test machine, the median over kept sub-runs with the empty turn's
  cost subtracted, against 6.4 us for a hostcall: 230 times cheaper. What is
  timed is the resolve as the runtime makes it, with the entry's type word
  taken back and compared as well; the trampoline alone, timed with that
  comparison removed, is 26.8 ns, so the type costs 0.9 ns, which is the
  caller's own store and compare and not the trampoline, whose instruction
  count is unchanged. The
  owner comparison is 8.1 ns of that, and most of it is not the comparison
  itself: the same build without the `MOV` from `CR3` measures 25.3 ns and
  without the owner word 20.4 ns, while reorderings of the same semantics
  span 26.8 to 33.3 ns, so the path is dominated by instruction placement
  around the SYSCALL/SYSRET pair rather than by the work added. It also
  registers an entry under each of two arenas and one on the base root and
  resolves all three from ring 3 under each root in turn: each answers in
  its own and nothing in the others, while the host resolves all three. It
  then registers one entry under an arena, destroys that arena with the entry
  left registered, creates another, and checks that the new arena's root is
  the destroyed one's recycled page -- so the case is the collision and not
  two arenas that happened to differ -- before resolving the stale handle
  from the new arena's ring 3 and from the host: both answer nothing, the
  destroy having retired the entry and counted it in
  `gk_stats.resolve_orphans`.
  Negative builds (`gk_test_neg`) remove the generation check
  (`GK_NEG_RESOLVE`), so the case fails on the never-issued generations and
  the recycled slot, the owner check (`GK_NEG_RESOLVE_OWNER`), so it
  fails on all six off-diagonal resolves, and the retirement at destroy
  (`GK_NEG_RESOLVE_ORPHAN`), so the stale handle resolves to the dead
  isolate's object from the next arena's ring 3 and host-side. It also
  registers two entries under types of its own and sees each answer with its
  own word, host-side and from ring 3, with a reserved entry carrying none
  until it is committed and a retired one none again. It also reserves three entries, sees them resolve to nothing from
  ring 3 until committed, commits one and sees it take once, and sees a
  commit refused for a committed, retired or never-issued handle and for a
  NULL, supervisor, refused or arena pointer, from commit and from register
  alike. The guard build runs the host-side path with allocation forbidden;
  the ring-0 path is asm with no call in it. What it does not give: ring 3
  can resolve as often as it likes with any handle, and learns only the
  pointer behind a live handle it holds, which the lookup makes safe to keep
  in guest memory but does not make safe to reach from there -- the object's
  own placement does that -- and a handle rewritten to another live entry of
  the same isolate and the same class names that entry: the owner is the
  isolate and the type word is the class, so the table tells one isolate's
  handles from another's, and one class's from another's, but not one of an
  isolate's objects of a class from another of the same class. That narrower
  residual is the one the arena and host slot facilities still carry whole
  (see `memory-classes.md`, Cost, for what a turn's dereference count
  implies).
- **Signal fields.** A runtime that tells a Wasm module its CPU budget is
  nearly spent does so by writing a constant into the module's linear memory
  from the handler of a per-thread CPU timer, while the turn runs. The
  handler can take no lock and allocate nothing, and a descriptor it reads
  from the ordinary heap -- a pointer and a size the isolate can rewrite --
  makes it an arbitrary write of a known constant, triggered by burning CPU.
  `gk_signal_field_register` keeps the descriptor in a fixed table
  (`GK_SIGNAL_FIELDS`, 8192 entries) in the supervisor control block instead:
  it is called host-side (from a hostcall handler, in workerd) for the
  calling thread's active arena, which it takes from gk's record of the
  thread and never from the caller, with the buffer named by its offset
  within that arena and its length and the field by its offset within the
  buffer, and it refuses, with nothing recorded, a buffer outside the arena,
  a field outside the buffer, any sum that overflows, a bad kind, a thread
  with no arena and a full table. What it keeps is the field's absolute
  address, its arena and its kind, under a handle validated like a host
  slot's, which `gk_signal_field_unregister` accepts only from a thread whose
  active arena is the field's. `gk_signal_field_write` is the signal-handler
  side: it finds the thread's record by kernel tid (the thread-local index
  is a hint it verifies and otherwise ignores), takes the arena from the
  record, walks that arena's entries without a lock, bounds-checks each
  address against the arena again and writes the value, reading nothing the
  guest can write and allocating nothing; the entries are published with
  their gate flag stored last and retired with it cleared first, so a walk
  that interrupts a registration or an unregistration on the same thread --
  the only thread that mutates that arena's entries, since it holds the
  isolate's lock -- sees either the complete entry or none. Destroying an
  arena drops its fields. The test registers a field from ring 3 through a
  hostcall handler and writes it from the host and from a `SIGUSR1` handler
  the guest raises at itself mid-turn, feeds the handler a buffer past the
  arena, one past its end, a field at the buffer's end and three bytes short
  of it, a length that wraps, a buffer too short and a kind past the limit
  and sees each refused with the count unchanged, sees an unregister refused
  from another arena, from no arena and for garbage handles, fills the table
  and empties it, sees a destroyed arena drop its field, shows a ring-3 read
  and write of the table faulting, times the write -- **about 60 ns** with
  eight fields registered, 50 with one, on the test machine, most of it the
  `gettid` that validates the thread record -- and runs 20000
  register/read/unregister rounds from ring 3 and 20000 from the host under a
  100 us CPU-time timer whose handler writes the fields, with every read
  consistent. The guard build runs the write path with allocation forbidden.
  In workerd, `TrackedWasmInstanceList` (`io/tracked-wasm-instance.h`)
  registers each module's fields through the `WASM_SIGNAL` hostcall from the
  turn that instantiates it, and its signal-handler entry points call
  `gk_signal_field_write` when `gk_initialized()` says the guest kernel is on
  -- a word in supervisor memory, not a heap flag a turn could flip to send
  the writer back to the heap list, which is what the writers walk without
  the guest kernel. `wasm-signal-test.sh` shows the signal reaching a module
  at ring 3 and not reaching a buffer the turn aimed the heap entry at.
- **A budget on what the refusal paths say.** Every path that refuses
  something a turn asked for reports it, and a turn chooses what it asks:
  unbudgeted, one worker forcing refusals in a loop makes the host write
  hundreds of megabytes a second of log -- a denial of service on the disk
  and on every other tenant's diagnostics, costing the turn no more than the
  call it was making anyway. `gk_log_budget` gives each report site
  `GK_LOG_BURST` reports per second and counts the rest, out of buckets in
  the supervisor control block: a counter in process memory would be one a
  turn can zero. The first report at a site always passes, so the first
  evidence of an attack is never what is dropped, and the report that passes
  carries the number held back since the last one. At guest ring 3, where the
  block is unreachable, the call decides nothing and answers `-EPERM`; the
  caller's ring comes from CS, so a handler on the host side of an exit takes
  the direct path however the guest has set its own flags. gk's own refusal
  paths use it through `write_stderr`-style composition rather than `fprintf`,
  which allocates and takes stdio's lock; workerd's use it through
  `WD_LOG_REFUSAL` (`util/host-memory.h`), and at ring 3 ask the host for the
  verdict through the hostcall their journal already uses.
  The buckets are an arena's, taken from the calling thread's own record:
  asking for a verdict is something a turn can do without having a report to
  make, so shared buckets would be one tenant's to spend on every other
  tenant's behalf. A turn can still spend its own isolate's budget, turning
  the detail of its own refusals into a number that still rises; and the
  bound on the process becomes the burst per site per live arena rather than
  per site. `gk_test.c`'s `log_budget_check` and `log-flood-test.sh` cover
  it.
- **Reports a handler makes wait for the end of the turn.** A hostcall
  handler runs with host privilege at a moment the guest chose, which may be
  the middle of the allocator with its lock held, so it must not take the
  heap -- and `KJ_LOG` formats, which does. `gk_in_handler` says whether the
  calling thread is in one, from the thread's record in the supervisor
  control block rather than from a thread-local a turn could clear, and
  answers no at ring 3 without reading the record. A report made under a
  handler is rendered into a fixed buffer where the refusal was found, kept
  in the thread's pool (host memory, and a copy, since the handle or pointer
  it names may name nothing by then), and written from `~ResolveTurn` at its
  own file and line -- the same line, a moment later. The list is short
  because the budget is spent first, so a report held back costs no slot; a
  turn that fills it anyway has the overflow counted and said so.
  `gk_test.c`'s `handler_flag_check` covers the flag, and the whole probe set
  under the guard build covers the rest.
- **Thread words.** Per-thread state the host or a signal handler must read
  from memory no ring-3 code can write -- the opposite of thread-local
  storage, which a turn can rewrite -- lives in `GK_THREAD_WORDS` 64-bit words
  in the thread's record in the supervisor control block.
  `gk_thread_words_set`/`gk_thread_words_get` address them by a validated key,
  find the thread's record lock-free (the thread-local index hint checked
  against the record's kernel tid, scanned for otherwise), and so are usable
  from a signal handler for about a `gettid` each (~50 ns). A thread with no
  record reads `-ESRCH`; a fresh record and a guest-created thread read
  zeroes, never a creator's words. In workerd the watchdog's progress counter
  (`util/thread-scopes.c++`), which its signal handler dereferences while the
  turn runs, keeps its state here rather than behind a thread-local pointer.
  `gk_test.c`'s `thread_word_check` sets words on the host, reads them from a
  `SIGUSR1` handler interrupting a ring-3 turn, shows the words unreachable
  from ring 3 and reachable from ring 0, and times the read; its negative
  build (`gk_test_neg`) places the words in guest memory and the case fails.
- **Turn directory.** A bound on where the path syscalls a confined turn
  forwards may land. With `gk_confine_paths` on, a thread with a turn
  directory (set host-side, from a hostcall handler, by `gk_turn_dir_set`)
  has its `openat` forwarded as `openat2` with `RESOLVE_BENEATH` and
  `RESOLVE_NO_MAGICLINKS` and its `newfstatat`/`statx` as an `O_PATH` open
  under the same rules, so an absolute path, a `..` or a symbolic link that
  would leave the directory, and a `/proc` magic link are refused by the
  kernel atomically with the lookup. `unlinkat`, `mkdirat`, `readlinkat`,
  `faccessat` and `faccessat2`, which act on a name rather than opening it,
  are split: the path is copied into host memory once, its parent is opened
  beneath the directory under the same rules, and the syscall runs against
  that descriptor with the final component alone, never followed. Every other
  path syscall, and `fchdir`, is refused, and a thread with no directory
  refuses them all. The descriptor is checked by device and inode on every
  use, so one the guest closed and reused names nothing. In workerd this
  bounds SQLite's `currentVfsRoot` (`util/sqlite.c++`), the dirfd of the path
  syscalls its native VFS issues from inside a turn and a thread-local the
  turn can write: each native VFS call that may name a file has the VFS's
  root made the turn directory through the VFS root hostcall, whose handler
  takes the descriptor from a host slot, so a rewritten `currentVfsRoot`
  makes that call fail rather than land elsewhere (`vfs-root-test.sh`; see
  memory-classes.md R7). `gk_test.c`'s `turn_dir_check` opens, stats,
  unlinks, makes and removes a directory, reads a link and checks access
  beneath the directory, sees a wrong dirfd, an absolute path, a `..`, a
  symlink out, a reused descriptor and the rest refused, and times a
  confined open; its negative build removes the confinement and the case
  fails with the file landing at a guest-chosen directory. The cost falls on
  the calls that name files: opening a Durable Object's database made 26
  hostcall pairs on the test machine, while a steady-state `/get` or `/put`
  on an open database makes none (its commit runs on the host side, and the
  turn's own syscalls are two `fcntl` locks), so the request p50 was the same
  with the turn directory on and off (about 1.7 ms for a read and 1.9 ms for
  a write under the guest kernel, oha, one connection, 10 s samples).
- **Threads as vCPUs.** Each host thread that enters the guest gets its own
  vCPU, stack and TLS base, sharing the VM and memory. Four threads run in the
  guest concurrently, and two threads each locked into their own arena run at
  the same time, each isolated from the other's cage. Threads the guest itself
  creates (via `clone`/`clone3` with `CLONE_VM|CLONE_THREAD`) are intercepted so
  the new thread enters the guest as a vCPU too, at its creator's privilege: a
  thread that ring-3 code spawns is launched into ring 3 by a `SYSRET` stub
  (its syscalls forward and return by `SYSRET`, a privileged instruction or a
  touch of gk's memory or another arena faults, and the fault ends that thread
  alone), while a ring-0 creator's thread starts in ring 0. vCPUs of ended threads are
  pooled and reused, so the vCPU count is bounded by peak concurrency, not by the
  number of threads ever created (KVM cannot reclaim a vCPU id, so the objects
  are recycled). Each vCPU takes exceptions on a private IST stack via a per-vCPU
  TSS, so a fault never pushes onto the interrupted code's stack (which would
  corrupt the x86-64 red zone that leaf functions use).
- **Syscall policy.** A filter can bound what forwarded code may do; a denied
  syscall is not forwarded and the guest sees `-EPERM`. The test denies `write`
  and confirms the guest gets `-EPERM` while other syscalls still work. A
  denial on an error path must not recurse: a JS turn in workerd stringifies
  its stack traces as addresses only (`jsg/guest-run.c++`, through KJ's
  `ExceptionCallback::stackTraceMode`), since the symbolizer KJ's full mode
  runs is a child process the filter denies, and the symbolizer itself
  (`patches/capnp-cpp`) reports a `pipe` or `fork` it cannot make as an
  unsymbolized trace rather than throwing out of a `noexcept` launch into
  `std::terminate`, whose handler would stringify a trace again, without end.
- **Demand-paged MMU.** The guest's memory is backed lazily. On a guest page
  fault the in-guest `#PF` handler hypercalls; the host checks the faulting
  address against its own address space and, if committed, backs it with a KVM
  memslot and a PTE, then the handler `iretq`s to retry. Backed ranges are
  tracked in an interval tree (a treap keyed by start) of non-overlapping
  windows, so every mapped page has a backing memslot. Memory is backed one
  aligned 2 MiB window at a time; memslots are created once and never deleted.
  Faults into another arena's region are refused, preserving isolation.
- **Huge host pages behind large commits.** A range of 2 MiB or more that an
  arena's owner commits in one piece (an ArrayBuffer backing store, a large
  object page, a semispace) is advised `MADV_HUGEPAGE` as the commit is
  reflected. The host then backs it with 2 MiB pages on its first touch from
  the guest, which KVM maps with one nested-page-table entry per 2 MiB: one
  nested page fault (a VM exit) per 2 MiB instead of one per 4 KiB. The
  guest's own PTEs stay 4 KiB and the arena walls are unchanged. Smaller
  commits, V8's 256 KiB regular heap chunks in particular, are deliberately
  left with 4 KiB pages: they could not take a huge page at their first touch
  anyway, and advising them would only let khugepaged collapse sparsely used
  ranges later and inflate RSS across many isolates. The advice is applied
  per commit rather than to the reservation, since a fixed `mmap` (V8's
  decommit, sometimes issued by a host thread gk never sees) replaces the VMA
  and loses it. `GK_NO_THP=1` disables the advice; a kernel whose
  `transparent_hugepage/enabled` is `never` ignores it.
- **Global pages across the per-turn root switch.** The guest runs with
  `CR4.PGE`, and the pages of the shared runtime (the binary's text and data,
  glibc, the C++ heap: everything demand-paged into the base root outside every
  arena) are mapped global, so their TLB entries survive the root switch a turn
  makes when the previous turn on the thread ran another isolate. The switch is
  the guest's own `mov %cr3` at the start of the turn (a guest CR3 load is not
  intercepted under nested paging and keeps global entries; a CR3 set through
  `KVM_SET_SREGS` or `kvm_run` makes KVM flush the whole guest TLB, global
  entries included). Nothing inside an arena is ever global: an arena's private
  tables differ per root, and a surviving translation would hand one isolate's
  page to the next isolate's turn. Nor is any page of a thread stack, and a
  slot that has ever held a global PTE is never given to an arena. The test
  alternates two arenas on one thread and, by dropping PTEs without a flush,
  shows that an arena page's translation is re-walked after a switch away and
  back while a shared page's survives it, and that gk's own flush drops global
  entries too.
- **The return from a turn, and vCPU runners.** Every turn ends in a VM exit
  that returns to userspace, and on a CPU where the kernel mitigates VMSCAPE
  ("IBPB before exit to userspace", the default; see
  `/sys/devices/system/cpu/vulnerabilities/vmscape`) that return issues an
  IBPB, which discards the CPU's branch predictions; on an EPYC 9255 the exit
  path discards its return predictions as well. The code after the return --
  the host side that finishes the request and dispatches the next turn --
  then runs on a cold front end, as does the next turn's guest code, whose
  predictions were on that CPU too: the event-loop thread retires a
  hello-world request's instructions at a quarter of its native rate, with
  30 times the mispredicts and 18 times the instruction fills from beyond
  L2, and a hello-world request costs 139 us instead of 24. That
  is where the guest kernel's CPU cost goes; nested paging's page walks are
  about 3% of it (`measure/findings.md`, section 4).

  `GK_RUNNER=1` gives each vCPU a runner: a host thread whose only work is
  the `KVM_RUN` ioctl, to which the thread that owns the vCPU hands every run
  and from which it takes the result back (KVM runs a vCPU for whichever
  thread of the process asks; the guest's registers, stack and TLS are the
  owner's regardless, and the owner still handles every exit). The IBPB then
  lands on the runner's CPU, where it costs a few dozen handoff instructions,
  and the owner keeps its predictions across the turn. The guest still
  enters cold, so the runner recovers the host side's share. The runner has
  to be on another core: `GK_RUNNER_PIN=other` pins the owner where it is
  and the runner to the next CPU on a different core that shares the
  owner's last-level cache (the two exchange the turn's working set through
  it; across CCDs a resident runner's hello costs 50 us against 28, see
  `measure/findings.md`), and hello world then
  costs 68 us with both sides sleeping on a futex through the handoff
  (`GK_RUNNER_SPIN=0`, the default; two wakeups per turn, one logical CPU
  busy, 83 us of CPU per request against 118 to 144 with the guest kernel
  on and 74 with it off) or 59 us with both spinning (`GK_RUNNER_SPIN=-1`,
  which keeps a second logical CPU busy for the whole turn). Unpinned, the
  scheduler usually separates the two threads and hello world costs 93 us.
  On the owner's SMT sibling (`GK_RUNNER_PIN=sibling`) the runner recovers
  nothing: the flush empties predictor state the core's two hyperthreads
  share. `gk_stats.runners` and `runner_runs` count the runners and their
  runs.

  `GK_RUNNER=resident` keeps the runner inside `KVM_RUN` across turns, so
  that on a turn's own path nothing returns to userspace on the vCPU's CPU
  and no IBPB is issued there: the guest's predictions survive from one turn
  to the next as the owner's do. Between turns the vCPU runs gk's park loop
  (`gk_park_loop` in `gk_asm.S`) at ring 0, with GS pointing at its park
  frame, a `gk_park` in supervisor memory that the host writes and ring 3
  cannot reach. The owner starts a turn by writing the frame -- the root to
  load, the entry stack, function, argument and ring, the values
  `enter_guest` otherwise puts in `kvm_run` -- and bumping a sequence word;
  the loop loads the root and `SYSRET`s into the turn. The turn ends with
  the exit sentinel reaching the vCPU's own `SYSCALL` entry
  (`gk_park_syscall_tramp`), which stores the result in the frame and echoes
  the sequence word instead of leaving the VM (a ring-0 turn returns into
  `gk_park_exit_tramp`, which does the same). The loop spins its budget
  (`GK_RUNNER_SPIN`, the same knob) and then halts with `STI;HLT`, which
  KVM services inside the kernel because the VM has an in-kernel LAPIC (a
  split irqchip, enabled at init); the owner kicks a halted vCPU with
  `KVM_SIGNAL_MSI` in NMI delivery mode, which wakes `HLT` whatever IF is
  and needs no EOI; the handler returns where it landed, so a kick that
  arrives after the loop already launched costs the turn one return, and
  when it landed between the loop's last check and its `HLT` (an NMI is not
  held back by the `STI` shadow) the handler returns past the `HLT`, so no
  wakeup is lost.
  The owner spins the same budget and then sleeps on the vCPU's eventfd,
  which the loop signals through an `ioeventfd` (an `OUT` the kernel turns
  into the signal without leaving `KVM_RUN`) and the runner writes when a
  run does return. A forwarded syscall, a hostcall, a demand fault and a
  fatal fault still return to userspace, one per event, and are served as
  before: the runner's ioctl returns, the owner handles the exit, the runner
  runs the vCPU again; a fault that ends the turn leaves the vCPU out of
  `KVM_RUN`, and the next entry starts it through `gk_park_entry`, which
  parks from a fresh state, with the turn's root loaded through `kvm_run`
  first. A between-turn TLB flush is deferred to the loop's launch path; a
  vCPU that goes to the pool, or that a stub must run on, is first asked to
  leave the loop (`park_leave`). The loop runs under the last turn's root
  and walks it on every TLB miss, so `gk_arena_destroy` first moves every
  parked vCPU still under the arena's root (the loop publishes its root in
  the frame) to the base root, and waits for the loop to say it has, before
  it frees the tables. The frame is trusted
  as `enter_guest`'s register values are trusted: ring-0 guest code could
  rewrite it, and could load CR3 directly, so the loop adds no capability
  ring 0 lacks. A vCPU whose id is above 254 (an xAPIC NMI cannot address
  it; 255 is the broadcast id) or that cannot get an `ioeventfd` (KVM's PIO
  bus holds 1000) gets a plain runner. `gk_stats.resident_vcpus`, `park_turns` and `park_kicks`
  count the resident vCPUs, the turns that ended in a park loop and the
  kicks. On the EPYC 9255 with the mitigation on, hello world then costs
  25.4 us with both sides spinning (`GK_RUNNER_SPIN=-1`; two logical CPUs
  per event loop) and 28.5 us with both sleeping (the default), against
  24.1 us with the guest kernel off and 138.0 us with it on, at 0.01
  returns to userspace per request; the heavier worker runs at native
  speed. The sleeping arrangement's CPU per request is dominated by KVM's
  halt polling between turns (`measure/findings.md`, section 4, "What a
  resident runner recovers"). That polling wants a second logical CPU per
  event loop, so the best placement depends on how full the machine is:
  one process serves 18,784 requests per logical CPU with the runner in
  the owner's cache and 13,262 on its SMT sibling, while sixteen
  processes on 48 logical CPUs serve 9,361 in the cache -- the
  per-process choice collides, and pinned threads cannot migrate off a
  contended CPU -- against 14,592 on the sibling, which follows from the
  owner's own core and so cannot collide. `GK_RUNNER_PIN` does not pick
  between them; `other` is the quiet machine's choice.
- **W^X.** Pages are mapped with their host protection: executable pages are not
  writable and writable pages are not executable (NX is enabled in the guest,
  and `CR0.WP` is set so the ring-0 guest cannot write a read-only page either).
  `mprotect`, `munmap`, a fixed `mmap` and `mremap` (both the range it leaves
  and the one it arrives at, so no translation survives at an address the
  mapping has left) are reflected into the affected guest PTEs and the
  TLB is flushed (the guest runs `INVPCID` type 2, or toggles `CR4.PGE` on a
  CPU without `INVPCID`; either drops global entries too. `INVPCID` is not
  intercepted under nested paging, so that flush costs one VM exit where the
  two intercepted `CR4` writes cost three. Re-setting identical control
  registers through KVM does not flush, and a `CR3` reload would keep the
  global entries). A plain `mprotect` that leaves the
  range readable rewrites the PTEs the guest already has, in place, to the
  protection the syscall itself just set; `munmap`, a fixed `mmap`, an
  `mprotect` to `PROT_NONE` and `pkey_mprotect` drop them, so the next access
  re-maps from the host. This is what lets code that flips pages between
  writable and executable work.
- **Protection keys (PKU).** V8's sandbox assigns protection keys with
  `pkey_mprotect` and flips its PKRU to write its code and pointer tables only in
  controlled windows. gk virtualizes this rather than stripping it: each range's
  key is tracked, reflected into the guest PTEs (bits 62:59), and the guest's own
  `wrpkru` sets its per-vCPU PKRU, so the guest CPU enforces the key exactly as it
  would natively and a violation arrives as a `#PF` with the PK bit. The host VMA
  is left keyless (the `pkey_mprotect` reaches the host as a plain `mprotect`) so
  KVM's page backing never pkey-faults. A single switch (`GK_VIRTUALIZE_PKEYS`)
  turns the whole scheme off, since gk already isolates by page-table root and
  does not depend on the keys for security. The keys themselves come from the
  host: V8 allocates its sandbox, JIT and pointer-table keys at initialization,
  outside any guest entry, so gk never sees those `pkey_alloc` calls and accepts
  any key in a forwarded `pkey_mprotect` rather than only the ones it saw handed
  out. A key it cannot reflect fails with `ENOMEM`, the one errno V8 tolerates
  from `pkey_mprotect` (it aborts on any other). One gap remains: pages keyed by
  a `pkey_mprotect` that V8 issues directly on the host, outside any guest entry
  (in workerd, the pointer tables in the arena tail at isolate initialization,
  V8 patch 0048), carry no key in the guest PTEs until the guest re-keys them.
  Those pages are still isolated by the arena's page-table root; only the key is
  not virtualized for them.
- **Guest on the caller's stack.** `gk_run(fn)` runs the guest on a private
  per-vCPU stack. `gk_run_here(fn)` instead runs it on the calling thread's own
  stack -- the guest's `rsp` continues from the call site, as a native call of
  `fn` would -- so stack-derived state stays valid across the host/guest
  boundary. An embedder whose conservative GC scans from a frame pointer captured
  on the host, or whose stack-limit check auto-detects the OS thread stack (both
  true of V8 in workerd), then sees the same stack the guest runs on. gk's own
  `KVM_RUN` loop moves to a side stack so it never collides with the guest, and
  the main thread's growable stack is expanded on demand by a probe syscall,
  since a guest access alone does not trip the kernel's stack growth.

The guest CPUID is the host's, so compiled code and glibc's ifunc resolvers
select the same instruction set inside the guest as outside it, AVX-512
included where the CPU has it. `gk_init` enables what that needs
(`CR4.OSFXSR`, `OSXSAVE`) and gives every vCPU an `XCR0` with every user state
component the CPUID advertises, so an instruction the CPUID promises never
raises `#UD` for a disabled state component. The test runs an AVX-512
instruction at ring 3 and ring 0 and checks its result on a CPU that has one.

## arm64

The design is the same on arm64; the sections above use x86 terms. Guest ring 0
is EL1 (running on `SP_EL0`) and ring 3 is EL0, and a CR3 load is a `TTBR0_EL1`
write. The differences that matter:

- **Exceptions go out by hypercall.** The vector table at `VBAR_EL1` saves the
  frame into the vCPU's control block (`struct gk_vcb`, a supervisor page that
  `SP_EL1` points at) and issues an `hvc`. A KVM SMCCC filter turns that into a
  `KVM_EXIT_HYPERCALL` for gk. A guest `svc` (a syscall) and a data or
  instruction abort (a fault) take the same route. On resume the vCPU enters
  `gk_dispatch`, which applies any TLB maintenance gk deferred to it
  (`tlbi vmalle1`, a single-address `tlbi vaae1` after a demand fault, or a new
  `TTBR0`), then resumes the frame or launches a turn.
- **Guest-physical addresses differ from host-virtual ones.** The IPA space (40
  bits at most, often less) is smaller than the 48-bit host address space, so
  memslots get their guest-physical range from a bump-and-free-list allocator,
  kept congruent to the host address modulo 2 MiB. Page-table entries hold those
  addresses. A destroyed arena's guest-physical ranges are not reused while a
  vCPU may still be running under its root.
- **The vDSO runs on the host.** arm64 KVM refuses a memslot over `[vvar]`, so
  `[vvar]` is a hole in the memslot map and `[vdso]` is never mapped into the
  guest. A guest call into a vDSO function takes an instruction abort at its
  entry, and gk runs the function on the host with the guest's arguments
  (`vdso_call`).
- **Ring 0 cannot execute writable user pages.** arm64 treats a page EL0 can
  write as privileged execute-never, and V8's code space is writable and
  executable on arm64. When ring 0 executes such a page, gk withholds its W in
  the guest page table, and a later write maps it writable again: W^X emulated
  per page (`pte_exec_at_el1`). Ring 3 is unaffected.
- **No protection keys.** arm64 Linux has no `pkey_*`, so the PKU
  virtualization described above is absent.
- **Any host page size.** The guest's own granule is 4 KiB on every host (the
  CPU must support it: `gk_init` fails otherwise). Everything the host kernel
  does -- mappings, protection changes and their reflection into the guest,
  arena size rounding -- follows the kernel's page size, read at `gk_init`, so a
  16K kernel (Apple Silicon) works as a 4K one does.

gk has been run on an Apple Silicon Mac in an Ubuntu 26.04 VM under Lima (nested
KVM through macOS Virtualization.framework). Its functional tests pass there, but
the setup is not representative for performance:

- A stage-2 invalidation by the Linux VM's KVM, which any `mprotect` or
  copy-on-write break of guest-backed memory causes, appears to discard all of
  the macOS hypervisor's shadow translations for the guest. Every page the guest
  touches next is translated again by the hypervisor, at about 1 us per page. A
  ring-3 pointer chase over 64 MiB runs at native speed in the steady state and
  about 10 times slower after a few such invalidations on unrelated pages. In
  workerd, where V8 changes page protections continuously, this turns young-
  generation GC pauses of a few milliseconds into pauses of up to seconds, so
  latency measured with the guest kernel on in this setup says nothing about gk
  itself.
- A vCPU sometimes keeps no TLB entries across `KVM_RUN` at all, so `gk_test`
  reports the global-page survival checks as not observable rather than failed.

## macOS

On macOS the hypervisor is Hypervisor.framework (HVF), arm64 only. `make`
links the framework and ad-hoc signs `gk_test` with `gk.entitlements`
(`com.apple.security.hypervisor`), which HVF requires; no root and no system
changes. The guest model is the arm64 one above, with the same vector table,
dispatcher and page tables, and `gk_test` passes with the checks that do not
apply skipped. The differences, mostly in `gk_hvf.c`:

- **Stage 2 is filled page by page and pins what it maps.** KVM backs a memslot
  from the host page tables as the guest touches it and follows the host's
  later changes; HVF's `hv_vm_map` takes a range the host has mapped and pins
  its pages into stage 2 then and there, and refuses pages whose maximum
  protection excludes writing (the binary's text and read-only data, the dyld
  shared cache). So a region gets no stage-2 mapping when created; each host
  page enters stage 2 when it gets its first guest PTE, right after gk has read
  its host mapping (a Mach VM region query rather than `/proc/self/maps`). A
  page HVF refuses is shadowed: a copy from a refused pool is mapped read-only
  and executable in its place. The commpage (two read-only kernel pages libc
  reads for the time) is shadowed too, and its copies are refreshed before
  every turn. A guest-side `munmap`, fixed `mmap` or decommit drops the range
  from stage 2 along with its PTEs, so the next access maps the host's current
  page.
- **A host-side remap is not seen.** A page the guest has touched stays the
  physical page it was: a host `mprotect` or `madvise` keeps it, but a mapping
  replaced from outside the guest (`munmap`/`vm_deallocate` and a new mapping at
  the same address) leaves the guest reading the old page and writing where the
  host never looks, where KVM would follow the host. Thread stacks, which come
  and go at the same addresses, are dropped from stage 2 and from every root at
  thread entry and exit (`stack_refresh`). Any other replacement is a known gap.
- **Syscalls are macOS's.** A guest `svc #0x80` carries the number in `x16`
  (negative for a Mach trap) and reports a BSD error through the carry flag,
  which gk puts back into the frame's SPSR. `mmap`/`munmap`/`mprotect` and the
  Mach traps `vm_allocate`/`vm_map`/`vm_deallocate`/`vm_protect` on the task
  itself are reflected into the guest page tables as the Linux calls are. Mach
  traps are allowlisted (`mach_trap_allowed`): a Mach message can have the
  kernel act on the process through its task port, so `mach_msg` and every
  trap outside the VM traps on the task itself, port-name management,
  semaphores and timing are refused with `KERN_PROTECTION_FAILURE`. Thread
  creation (`bsdthread_create`, the workqueue calls) is refused with `EPERM`: gk
  does not intercept Mach thread creation as it does `clone`, so guest-created
  threads are unsupported and the corresponding tests are skipped.
- **A vCPU belongs to its thread.** HVF vCPUs cannot be run from another
  thread, so one is created per host thread and destroyed when the thread ends;
  the vCPU id, control block and guest stacks are pooled and reused as on
  Linux. The thread pointer is `pthread_self()`, whose descriptor sits at the top
  of a pthread's stack, which is where the host-frame wall stops.
- **The vCPU resumes under a root that may be gone.** Between turns a vCPU
  waits at the dispatcher with its last root in `TTBR0_EL1`, which a
  destroyed arena may have freed; HVF keeps no TLB entry for the dispatcher
  across `hv_vcpu_run`, so gk loads the next root from the host before a turn
  that switches roots (the dispatcher then re-loads it and invalidates). KVM
  on arm64 does the same.
- **No vDSO, no protection keys, no huge-page advice.** The guest reads the
  counter directly (`CNTVCT_EL0` with a zero vtimer offset is
  `mach_absolute_time`), arm64 has no `pkey_*`, and macOS has no
  `MADV_HUGEPAGE`.

On macOS there is no Bazel build or workerd integration, guest-created
threads are unsupported, and performance is unmeasured. `hv_vcpu_exit_t`, which HVF
allocates, is readable from the guest, and a shadow pool page is never reused.

## How it would map to workerd

| gk | workerd |
| --- | --- |
| `gk_init` | one-time process setup, alongside `IsolateGroup` creation in `setup.c++` |
| `gk_arena` | one isolate's V8 cage |
| `gk_arena_enter` | the isolate-lock switch, where `MemoryProtectionKeyScope` in `jsg.c++` sets the pkey today |
| `gk_run` | running JS/runtime code for the locked isolate |
| a thread's vCPU | a workerd thread that runs isolate code (event loop, V8 workers) |
| syscall forwarding | the host side of the process performing real I/O |

On a Zen 3 test machine the CR3 write behind `gk_arena_enter` costs about 217
cycles and does not trap to the host, while a forwarded-syscall VM exit round
trip costs about 4.7 microseconds and a hostcall round trip about 6.4 (the
exit plus the exchange-frame copies and the handler). The switch is roughly
two orders of magnitude cheaper than an exit, so batching the dominant
syscalls is what decides the overall cost, and a hostcall is for an operation
rare enough to pay for an exit each time. A ring-0 resolve (`gk_resolve`),
which stays inside the guest, costs about 28 ns on the Zen 5 test machine,
so a crossing a turn makes on every dereference belongs on that path.

## Limitations (remaining work)

- **Most of the shared runtime memory is reachable from ring 3.** The ring-3
  wall covers other isolates' arenas, gk's control state and host memory, not
  the rest of the runtime's own memory: the C++/KJ heap, glibc, and every
  guest thread's stack (the JS turn runs on the calling thread's stack, above
  host frames) are user-mapped. Code execution inside an isolate can corrupt
  that shared state and, via a host return address on the stack, potentially
  reach host execution.

  That memory is also *readable*, which matters even without code execution.
  The per-isolate arena covers what V8 reserves page-granularly -- the
  sandbox, trusted and code ranges, pointer tables and cppgc pages, all
  routed through `jsg::ArenaPageAllocator` -- so another isolate's V8 heap is
  unmapped and a stray read cannot reach it. It does not cover the C++
  objects that come from the process allocator: `i::Isolate` and malloc'd V8
  state, JSG `Wrappable`, `IoContext`, the objects an `IoOwn` names, and the
  KJ allocations that carry request and response bodies. Those share one
  heap across isolates, so an escape with only a read primitive can still
  recover another isolate's in-flight data from it.

  Confining this needs the runtime's mutable memory out of ring 3's reach, so
  that an arena maps only the isolate's own pages, the runtime's text and
  read-only data, and a per-isolate channel for talking to a host-side
  runtime. The memory for that exists: `gk_host_alloc` (see "Host memory"
  above) gives the host side memory that no guest root maps, and
  `util/host-memory.h` places a C++ object there. What is still missing is the
  interface: the C++ bindings reach runtime state through direct pointers, so
  an object moved into host memory faults the turn until the turn's accesses
  to it become messages. The channel token key is in host memory, the two
  functions that read it run on the event loop, outside the guest, when a
  turn asks for a token, the handler reaches the key through a host slot
  handle (`HostSlot<T>`), so a write primitive that rewrites the handle makes
  the host refuse the use rather than read a key of the guest's choosing, and
  the key and every token nonce are drawn from a buffer in host memory
  (`util/host-entropy.h`) rather than from the thread-local one a turn can
  write. The actor-id namespace key is in host memory the same way, and its
  readers, which are synchronous to JS, reach it through a hostcall (see
  "Hostcalls" above): the HMAC runs host-side inside the turn, on a key the
  turn cannot read, with the random half of a unique id drawn from the
  host-memory buffer in the same call. The descriptor the Wasm shutdown
  signal's handler writes through is in gk's signal field table (see
  "Signal fields" above), registered through a hostcall and found through
  gk's record of the thread rather than through the heap entry or a
  thread-local. An `IoOwn` -- the reference a JS heap object holds to a KJ
  I/O object, the largest family of pointers the host loaded from guest
  memory -- names its object by a handle into the ring-0 resolve table (see
  "Ring-0 resolve" above), resolved on every use, so a rewritten one fails
  the use rather than redirecting it, and the context's record of the
  object -- the owned-list node whose links the host follows at teardown
  and on the delete-queue drain, and whose disposer the host calls -- is in
  host memory, written by the host, as is the context's record -- the delete
  queue, its lock and its signal -- which the holder names by the context's
  handle beside the object's; the object it names is still in the shared
  heap, and the disposer the node records is the one the turn journaled
  when it created the object.
  What a rewritten handle can still do is name another live host slot, or
  another live resolve entry of the turn's own isolate: a host slot table
  turns "any address" into "one of the live tenants", and telling a
  legitimate use from a swapped one is an ownership question it cannot
  answer, as with arenas below. The resolve table answers half of it: an
  entry records the page-table root it was registered under and the ring-0
  lookup compares that against `CR3`, so another isolate's entry is refused
  (see "Ring-0 resolve" above); what it does not tell apart is two entries
  of the same isolate. Every other object listed above is still in
  the shared heap in the default build.

  With jemalloc as the allocator
  (`--//src/workerd/server:allocator=jemalloc`) the arena also covers the C++
  objects the runtime allocates for the isolate while its lock is held: each
  isolate has a jemalloc arena over the upper part of its gk arena's tail
  (`jsg::HeapArena`), and the isolate lock switches the thread onto it, so
  JSG wrappers, `IoOwn`'d objects, V8's per-turn malloc'd state and the KJ
  allocations that carry request data are unaddressable from other isolates'
  turns too; `src/workerd/jsg/guest-arena-test.c++` checks this. The arena's
  bookkeeping is shared, so a free from any context touches only shared
  memory, and jemalloc's thread cache is off process-wide, since it would
  recycle one isolate's freed blocks into another's allocations. A thread of
  its own stays bound to the shared arena from the first `HeapArena` on,
  because jemalloc returns every dirty page of an arena to the kernel when
  the last thread bound to it leaves, which the event loop would otherwise do
  each time a scope switched it onto an isolate's arena -- and then fault
  every page of the next request's buffers back in
  (`keepSharedHeapArenaPopulated()` in `jsg/heap-arena.c++`; what it cost,
  and the coin it was, are in `measure/findings.md`). What stays
  shared, and readable, is what is allocated with no isolate lock held -- the
  isolate object itself and the rest of what V8 allocates while creating it
  (creation also grows process-wide registries, which must not die with one
  isolate), and whatever V8's background threads allocate on an isolate's
  behalf, since they run on their own arenas -- and what is placed in the
  shared heap on purpose from under a lock, through `jsg::SharedHeapScope`,
  because a turn allocates it but it is not the isolate's. That covers
  process-wide state a turn initializes or grows: everything ICU allocates
  (`u_setMemoryFunctions()` in `jsg/setup.c++`), the C library's buffer for
  `stdout`, which it would allocate on the first write to the stream -- V8's
  `--trace-gc` line from inside a turn, say -- and is given beforehand
  (`setvbuf()` beside the ICU hooks), everything V8's `Intl`
  builtins allocate (the hooks of `patches/v8/0049-Run-Intl-builtins-under-an-
  embedder-shared-allocation-scope.patch`, which V8 keeps process-wide caches
  of locales, calendars and date formats behind), what V8's wasm type
  canonicalizer grows (the hooks of `patches/v8/0050-Canonicalize-wasm-types-
  under-an-embedder-shared-allocation-scope.patch`; installing the
  `WebAssembly` JS API canonicalizes the builtins' signatures on every
  `Context::New`, so the first isolate in the process places the
  canonicalizer's containers and a later one's rehash moves them), the
  thread's timer queue
  entries (`TimerChannel` in `server.c++`), and the V8 platform's worker-thread
  task queues and job state (`jsg::V8PlatformWrapper`). It also covers the
  whole of an in-process Cap'n Proto call, which is every service binding and
  every JS RPC call; that one is described on its own below. Each use is a
  confidentiality concession: subrequest URLs and headers, response headers, a
  service-binding callee's `IoContext`, whatever `Intl` formats, and the
  canonical wasm type table, are readable from every isolate's ring 3.

  One registration is placed by interposing on the call that makes it rather
  than by a scope at its site. Every function-local static with a non-trivial
  destructor registers that destructor through `__cxa_atexit` when it is first
  initialized, and `glibc` keeps the registrations in a chain of 32-entry
  blocks whose first block is static: `__new_exitfn` walks the chain from its
  head on every registration and allocates another block once the head block
  is full. A registration made inside a turn therefore puts a block of
  process-wide state in that isolate's arena, and every later registration --
  from any isolate, for any static -- faults *reading* that block on its way
  down the chain. Scoping the code that registers is no remedy, which is why
  the embedder scope the `Intl` builtins run under does not stop ICU's
  registration from `SimpleDateFormat::subFormat` faulting the next isolate:
  what faults is the read. So `__cxa_atexit` is interposed with the same
  `--wrap` the allocation census uses for `malloc`, and every registration
  runs in the shared heap from the moment the first arena is created
  (`jsg/heap-arena.c++`). Before that every thread is on an automatic arena,
  which is the shared heap, and until then the wrapper reads nothing but the
  flag that says so: the program's first registrations are made during static
  initialization, where evaluating `heapArenasAvailable()` could both re-enter
  a static that is still being initialized and latch its answer before
  `malloc_conf` has been set. Reserving blocks ahead of demand instead of
  interposing would only postpone the fault, since nothing bounds how many
  statics are first reached inside a turn.
  `src/workerd/experimental/guest-kernel/atexit-arena-test.sh` checks the
  result against a running server: two isolates formatting dates behind a
  third whose turns registered first.

  One index is placed by an allocator of its own rather than in the shared
  heap. `ThreadIsolation`'s JIT-page map
  (`src/common/code-memory-access.h`) is the `Address -> JitPage*` index that
  answers "which code allocation is this PC in?" for GC-safe code lookup
  (`Heap::GcSafeFindCodeForInnerPointer`) and for stack iteration
  (`StackFrameIterator::ComputeStackFrameType`). Both read it at ring 3 inside
  a turn -- the second from `Runtime_CompileOptimizedOSR` -- while an
  optimizing compile in a turn adds entries to it, so one isolate's turn
  faults walking to an entry another's registered. The map is not the write
  protection's bookkeeping and cannot be switched off with it: V8 builds it
  whether or not thread isolation is enabled (`ThreadIsolation::Initialize`,
  which says so), and its readers have no other implementation. What placed
  the entries in an isolate's heap is that every allocation of the index goes
  through the platform's `v8::ThreadIsolatedAllocator`, whose `Allocate` is
  `return malloc(size)`, above a TODO in V8 saying it ought to return
  thread-isolated memory.

  That allocator is an embedder interface, so `jsg::wrapThreadIsolatedAllocator`
  (`jsg/thread-isolated-allocator.h`) wraps the platform's and serves the index
  from slabs it takes from the OS, with no V8 patch: `StlAllocator` and
  `ThreadIsolation::ConstructNew` both go through it, so the map, its nodes,
  the `JitPage`s and the two mutexes all land in process memory outside every
  arena, which every root maps. The slabs are keyed with the protection key the
  platform's allocator allocated, which V8 takes as `ThreadIsolation::pkey()`,
  so the index is also read-only outside the `RwxMemoryWriteScope` /
  `CFIMetadataWriteScope` windows V8 already enters around every read and write
  of it -- with one exception, `ThreadIsolation::TearDown`, which deletes the
  map and its mutex with neither scope and which `V8System`'s destructor
  therefore brackets with `jsg::ThreadIsolatedWriteScope`; that one belongs in
  V8. That part is not a guest-kernel property and holds in the default
  build: before it, workerd enabled V8's key-based write protection for JIT
  code while leaving the bookkeeping of that protection in ordinary writable
  heap. What the key is worth is bounded, and not by workerd: `WRPKRU` is
  unprivileged and PKRU is ring-3 writable, and V8 reaches it through glibc's
  exported `pkey_set`, so the key stops an adversary who can write memory and
  not one who can redirect control flow. Keeping the index out of a turn's
  reach at all is the page table's job.

  What a turn can still learn from the index is the code ranges and allocation
  sizes of every isolate in the process -- layout, not contents, since a read
  of another arena still faults -- and what it can do to it, between the
  windows in which the key is open, is what it can do to any shared object:
  rewrite an entry so that the host's `StartOfJitAllocationAt` answers with an
  address of the turn's choosing, which `GcSafeFindCodeForInnerPointer` hands
  to an unchecked cast. Both of those hold in the default build already.
  `src/workerd/jsg/thread-isolated-allocator-test.c++` checks the allocator
  itself, including that the memory it hands out carries the key, and
  `guest-arena-test` part 7 is where the misplacement showed as a fault.

  One thread-wide structure is placed without the shared heap, because it is
  below `jsg`: the resolve journal a turn registers its owned objects through
  (`hostMemory`'s `TurnJournal`). It is created by whichever isolate first runs
  a turn on the thread and read and written at ring 3 by every turn after
  that, so it gets pages of its own, outside every arena, rather than a block
  of the creating isolate's heap.

  **The in-process Cap'n Proto call.** The call's host-side plumbing is shared
  by construction. The caller's turn builds the request message with its
  segments and capability table, the stubs its arguments serialize to with
  their membranes and pending events, and the call context and promise
  plumbing `send()` makes; the callee's turn reads those, writes the response
  message the same way, and builds the stubs its return value serializes to;
  and each side then frees what the other built, which faults as surely as a
  read does, since `operator delete` is reached through the object's
  destructor.

  The scope that places it is not one scope around the call, because the call
  is not one dynamic extent in `workerd`: it is four windows, each a run of
  straight-line code in one isolate's turn, with that isolate's own work on
  either side of it. They are, in `api/worker-rpc.c++`, building and sending
  the request (`callImpl()`, from resolving the destination through
  `send()`); writing the results (the functor `handleResult()` hands to
  `awaitJs()`, around `serializeJsValueWithPipeline()` and the call pipeline);
  and the two capability *imports* -- taking a capability out of a message is
  as much a crossing as putting one in, because it instantiates the hook that
  carries it and records it in the wrapper table of each membrane between the
  two ends (`deserializeRpcReturnValue()`, `JsRpcStub::deserialize()`, and the
  caller's `ExternalPusher` as `handleResult()` imports it). The session's own
  capabilities and membranes (`JsRpcSessionCustomEvent::run()` and
  `sendRpc()`) are placed where they are made.

  Two things outside the call need the same placement, and neither is a
  Cap'n Proto object. A request's `IoContext::PendingEvent`
  (`io-context.c++`) is held by every capability the request exports, so a
  stub released in the calling isolate's turn runs its destructor there -- and
  the destructor schedules the context's hang-abort task, which the context's
  own next turn cancels, so that allocation is placed as well. And a
  worker's entrypoint names (`Server::ErrorReporter`) are built while its own
  modules are evaluated but read by every caller that starts a subrequest to
  it, which for a service binding is a turn of the calling isolate.

  `src/workerd/experimental/guest-kernel/rpc-arena-test.sh` checks the result
  against a running server: a caller and a callee in separate arenas, with
  stubs taken across, called and disposed of in the caller's turn, and a
  third worker holding the first arena.

  Cancelling a call that is still in flight is an ownership question rather
  than a placement, and the windows do not answer it. An `AbortController`
  abort is delivered inside the aborting turn, and a subrequest's teardown
  reaches the callee's own memory: the callee's request is served from the
  callee's turns, so the promise plumbing those turns build for it, the
  request's `IoContext` and the `jsg::Ref`s its frame holds are in the
  callee's arena, which the caller's turn cannot address. The frame itself is
  not the problem -- `kj`'s `HttpClientAdapter` calls
  `WorkerEntrypoint::request()` from the caller's turn, so the frame is
  allocated in the caller's arena -- and what faults is the first thing the
  frame owns that the callee's own turn built: the promise node its
  `co_await context.run(...)` awaits. Placing that node would move the fault
  to the next such object and no further.

  Handing the frame to the callee's context, as a cross-context `IoOwn` is
  handed to its own context, does not work either, because the frame is not
  independently owned. `kj` drops a promise's dependency before its
  attachments, and the frame borrows the request body pipe, the url and the
  headers the caller attached above it, and `this`, which the caller's
  `kj::HttpClient` owns; a frame deferred past those reads freed memory.
  Taking out only what `kj::Canceler` destroys is no better: destroying the
  wrapped promise before `cancel()` returns is the guarantee the class exists
  to give, and what it destroys is an interior node of a graph whose other
  parts stay alive, so deferring that node alone reorders the teardown
  against live owners.

  What can be deferred, because it reorders nothing, is the *delivery* of the
  abort. `AbortSignal::wrapDeferred()` (`api/basics.h`) joins the
  subrequest's response promise exclusively with a cancellation promise that
  the abort rejects: all the aborting turn does is reject a fulfiller, and
  `kj`'s exclusive join destroys the subrequest when it fires, which is on
  the event loop, outside the guest, where every arena is addressable. That
  is where `workerd` destroys a cancelled request whenever the abort does not
  come from JS. The three subrequest wraps in `api/http.c++` take it -- the
  response promise, the WebSocket response and the actor-retry attempt -- and
  everything else keeps `wrap()`, whose synchronous cancellation a promise
  that borrows state from whoever holds the registration still needs. The
  cost is that the subrequest runs on until the event loop reaches the join.
  `abort-signal-test.sh`'s `/fetch-abort` drives it end to end, and a
  negative build, `--copt=-DWORKERD_GK_NEG_ABORT_DEFER`, puts `wrap()` back
  under the subrequest wraps so that the teardown runs in the turn again.

  The list is not complete yet. There are further process-wide registries that
  grow from whichever isolate first needs an entry, and each one faults the
  next isolate that reads it.

  The one that remains is V8's wasm engine. `WasmEngine`'s `NativeModuleInfo`
  map and the per-isolate sets `UseNativeModuleInIsolate` fills, the
  `NativeModuleCache` keyed on wire bytes, `WasmCodeManager`'s allocation pool
  and each `NativeModule`'s own code vectors are members of the process-wide
  `GlobalWasmState`, with no embedder allocator between them and
  `operator new`, so they need a V8 patch of the same shape as the two that
  exist. The type canonicalizer, which patch `0050` places, is only one part of
  that state: two workers instantiating the same module from a turn still fault
  in `wasm::InstanceBuilder::InstanceBuilder`, reached from
  `WasmEngine::SyncInstantiate`, on an entry the first worker's turn wrote.
  Only a worker that uses wasm reaches it.

  The thread's event loop needs more than placement. Its run queue is an
  intrusive doubly linked list through the queued events, so arming an event
  from a turn (fulfilling a promise, attaching a continuation to a settled
  one) writes the link of the neighbor at the insert point, which may be
  another isolate's promise node. `kj::EventLoop::GuardedArmScope`
  (`patches/capnp-cpp/0003-Add-EventLoop-GuardedArmScope.patch`), which
  `jsg::runInGuest()` enters around every turn, links guard sentinels of the
  loop's own next to each insert point for the turn's duration, so a turn's
  events are inserted next to a guard or next to each other. Disarming is not
  covered: a turn that destroys one of its own events that was queued before
  the turn began (a timer callback cancelled while already due, a fetch
  aborted while its response is queued) unlinks it from neighbors that may be
  another isolate's, and faults.

  With this, two workers using `Intl` with different locales, two using
  `setTimeout`, and two plain workers serve concurrently under load without
  faults, and a destroyed isolate's arena is destroyed with it -- heap arena,
  reservation and page-table slot, which the next isolate reuses
  (`src/workerd/jsg/guest-arena-test.c++`, part 7). That part is not yet
  reliable: in one direct run of the test binary out of about twenty, the
  first ring-3 turn of the isolate created after a destruction faulted and
  the process then hung until killed. The cause is not known; the missing
  cross-vCPU shootdown (below) is a candidate, since the slot is reused.

  Request and response *bodies* cross such a call through a relay rather
  than in shared memory. Body bytes travel through a `kj` one-way pipe whose
  two ends are held by different isolates, and the pipe copies between the
  writer's buffer and the reader's in whichever of the two operations arrives
  second, so the copy runs in one isolate's turn over a buffer in the other
  isolate's arena. Nor are the bytes all that crosses: the state object the
  first operation leaves on the pipe is a promise node in the first party's
  arena, which the second party reads, writes and fulfils, and fulfilling it
  arms the first party's continuation, a write into that arena too. Placing
  the buffers alone would therefore not do. `ReadRelay`
  (`io/worker-interface.c++`), installed by `asHttpClient()` at the reading
  end of each pipe -- in front of the request body the callee reads and of
  the response body the caller reads -- makes every operation on the pipe
  from the event loop, outside any turn, where every arena is addressable: a
  read yields to the event loop, reads from the pipe into a staging buffer
  the host allocates, and copies into the reader's buffer from there. The
  pipe then sees only host-side reads, whose node and staging buffer are in
  the shared heap; the writer's turn fills the staging buffer and arms the
  relay's coroutine, allocated in the shared heap for that; and each side's
  own buffers keep their isolate provenance, so the copy sites in
  `api/streams/internal.c++` and `api/js-readable-stream.h` are unchanged.
  The relay is installed only where heap arenas are active, so a build
  without them uses the pipe directly. It costs a copy and an event-loop
  turn per chunk and forgoes the synchronous read fast path on such bodies.
  It also settles how the request body pump is cancelled: an abort delivered
  inside the caller's turn destroys the pump and so drops the pipe's write
  end there, which reaches whatever the callee left on the pipe, and with the
  relay that is a node the host allocated, so the pump keeps `wrap()` and
  needs no deferral. `body-arena-test.sh` checks both directions, a passed-
  through body and the abort; a negative build,
  `--copt=-DWORKERD_GK_NEG_BODY_RELAY`, leaves the relays out and the test
  fails on its first body. Not relayed: a `connect()` tunnel through a
  binding, whose two-way pipe crosses the same way, and a WebSocket's
  frames, which are a separate item. What the relay does not change is a
  drop of the response body inside the caller's turn (`handleHttpResponse()`
  in `api/http.c++` returns early when the signal has aborted since the
  response was delivered), which aborts the pipe's read end against what the
  callee left on it and cancels the callee's request; that reaches the
  callee's arena as it did before, and is not covered by the test.

  Three more things cross the same binding outside its bodies, and none of
  them through the kj HTTP interface whose url and headers `kj`'s adapter
  clones in the shared heap. A request's `cf` blob travels as
  `IoChannelFactory::SubrequestMetadata`: serialized by the caller's turn
  (`Request::serializeCfBlobJson()`), read by the callee's turn (`CfProperty`
  in `api/global-scope.c++`), and present on every request a worker forwards
  rather than builds, since the listener makes a `{"clientIp": ...}` blob for
  each connection; `WorkerEntrypoint`'s constructor copies it into the shared
  heap. A callee that throws hands the caller the `kj::Exception` its turn
  made from the JS error, whose storage is in the callee's arena and which the
  caller's turn reads in `jsg::exceptionToJs()`; `exceptionToPropagate()` in
  `io/worker-entrypoint.c++` clones it into the shared heap on the event
  loop, where a response's headers already go. A JS RPC call's exception
  crosses without help: `LocalClient::call()` forks the dispatch promise, and
  each branch of a fork receives its own clone of the exception
  (`kj::ForkBranch::get()`), made on the event loop; leaving the clone out of
  `JsRpcTargetBase::call()` faults nothing. And a JS RPC call that rejects
  drops the
  caller's exported capabilities inside the callee's turn, so the caller's
  `IoContext::PendingEvent` destructor runs there and the hang-abort task it
  schedules went into the callee's arena, where the caller's next
  `registerPendingEvent()` faulted cancelling it; the destructor now
  allocates under the scope the event itself is made under. Each is a
  confidentiality concession of the kind the shared heap's other tenants are:
  the blob, the error's description and the task are readable from every
  isolate's ring 3 for as long as they live. `binding-crossing-test.sh`
  checks all three, on the benchmark's own cdn pair among others. None of the
  probes before it forwarded a request or failed a callee -- each built its
  subrequests from a URL string and every callee answered -- which is how the
  pair faulted under the benchmark while twenty probes passed.

  What the relay also leaves as it was is how the response promise itself
  crosses. The caller's turn attaches its continuation to a promise whose head
  `SharedHeapHttpClient::request()` (`io/worker-interface.c++`) allocated under
  a shared-heap scope, and `kj` co-locates a promise's dependents into the
  1 KiB arena block its head was allocated in while they fit
  (`PromiseDisposer::appendPromise`, `kj/async-inl.h`), so the chain the caller
  builds on top of the head -- the relay wrap, the `.catch_()` in
  `fetchImplNoOutputLockAttempt()`, the exception-wrapping `.then()`,
  `awaitIo()`'s own `.then()` and the `kj::TaskSet::Task` it ends in -- starts
  in the shared heap. A chain that outgrows its block starts the next block in
  the arena of whichever turn adds the node, which for these nodes is the
  caller's. That overflow does not make the crossing fault, because the
  response is not delivered inside the callee's turn: the in-process
  `kj::HttpService`-to-`kj::HttpClient` adapter fulfils the response promise
  from the event loop, where every arena is addressable, so the callee's turn
  never arms a continuation the caller placed in the caller's arena. Padding
  the chain with a hundred co-located `.then()` nodes, so the `Task` and most
  of the chain land in the caller's arena many times over, faults no turn and
  condemns no isolate. Co-location is therefore not what keeps this crossing
  safe, and the chain's length does not matter to it.

  Nor can a worker lengthen the chain from JS: the nodes between the head and
  the `Task` are a fixed, bounded set built by the C++ fetch path, and a
  worker's `.then()` chains, `await`s and stream plumbing hang off the V8
  promise that `awaitIo()`'s resolver settles and off the JS-side stream
  machinery, never off these `kj` nodes. Driving 4000-deep `.then()` chains,
  4000 sequential `await`s, 200 concurrent calls and 500-call re-fetch loops
  through an in-process binding faults no turn.

  This covers the in-process HTTP service binding. A promise whose fulfilment
  does run inside a foreign turn still faults on a continuation placed in that
  turn's unaddressable arena -- that is exactly the body pipe's state node,
  which the second party fulfils from its own turn and which `ReadRelay`
  therefore moves to the event loop (above).

  Cap'n Proto promise pipelining over a JS RPC binding
  (`binding.foo().bar()`, a call made on the promise of an earlier call's
  result) is the one way a worker's JS extends a cross-isolate `kj` chain,
  and it is safe for the same reason. Each call on a pending promise asks the
  pending call's `QueuedPipeline` for a pipelined capability, which is a
  `QueuedClient` holding branches of the pipeline's promise, and sends the
  call into it, where it waits as a continuation on the client's
  call-forwarding fork (`capnp/capability.c++`); the chain grows by that much
  per hop, as deep as JS makes it. All of it is built by
  `JsRpcPromise::getClientForOneCall()` and `send()`, inside the caller's
  `callImpl()` window, so it is in the shared heap; and none of it fires in
  any turn. The pipeline settles from the fork of the dispatch promise that
  `LocalClient::call` makes, which resolves after the callee's turn has
  returned to the event loop -- the `awaitJs()` fulfiller the callee's turn
  arms is the callee's own, and `JsRpcTargetBase` never sets a call's
  pipeline early: its `callPipelineFulfiller` is declared and never armed,
  and `CallContext::setPipeline()` is used only by `connect()` tunnels -- and
  the queued call is forwarded from that continuation into
  `LocalClient::call`'s `evalLater()`, so the callee's turn for it is entered
  later through its own lock, like any call's. Placing the queued client and
  its branches in the caller's arena on purpose faults no turn, which it
  would have to if any foreign turn read or armed them. `rpc-pipeline-test.sh`
  drives the shapes JS can give it -- a call on a stub's promise, eight hops,
  a plain-object result and a property path through it, a third isolate's
  stub, fifty calls queued on one pending promise, a settled promise, a
  rejected call, abandoned calls and an echoed stub -- and under
  `--copt=-DWORKERD_GK_NEG_RPC_SHARED_HEAP` fails on its first route, on the
  callee reading the caller's request, which is the four windows' own
  crossing and not one of pipelining's.

  Also not covered: BoringSSL's lazily allocated per-thread error state, which
  a `crypto` call in a turn would place on that turn's arena -- untested, but
  by the same reasoning as the registries above.
- **The memory classification of runtime objects is applied to few of
  them.** Which side of the boundary each runtime C++ class belongs on is
  written down in `memory-classes.md`, together with the rules that decide
  it, the splits the classes that straddle need, and the places the code
  violates it today. The clang-tidy check `workerd-memory-class` enforces it
  for the classes that carry a `WD_MEMORY_CLASS_*` annotation
  (`src/workerd/util/memory-class.h`), which is a handful so far; every other
  class is unchecked. Placement by class exists for H only, as an explicit
  choice per object (`HostOwn<T>` and `HostSlot<T>`, `util/host-memory.h`)
  rather than something the allocator decides, and six kinds of object have
  been placed: the channel token key, the host entropy buffer, the actor-id
  namespace key, the Wasm signal descriptors, the owned-list nodes behind
  `IoOwn` handles and the context records that hold each context's list
  head and delete queue.
- **The host acts on data a ring-3 turn can write.** The runtime's heap is in
  the guest's address space, so every host-side structure the guest can reach
  is attacker-controlled input to the host, and the dangerous cases are the
  ones that reach a privileged operation. Those are validated: a host call
  that names an arena has the pointer checked against gk's own arena pool
  before anything is read through it, the arena page allocator refuses an
  address outside the tail it owns before handing it to `mprotect` or
  `mmap(MAP_FIXED)`, the memory syscalls a turn issues itself are checked by
  range before they are forwarded (see the syscall-policy entry below), a
  turn's outcome crosses to the host as bounded plain bytes rather than as a
  `kj::Exception` the guest wrote, and the guest-turn depth is restored from
  a value saved in a host frame instead of being decremented, since a
  rewritten counter would otherwise make the next turn run outside the guest,
  a host-memory object is reached through a host slot handle that gk
  validates against its own table rather than through a pointer the host
  loads from the heap, an `IoOwn`'s object is reached through a resolve
  handle the same way, with the pointer behind it admitted to the table only
  if it is ordinary user memory the turn could write anyway, and the list
  the host walks to release such objects is in host memory, a hostcall's handler is selected by an index gk
  checks against its table and sees a host-memory copy of the arguments,
  never the guest's memory, the Wasm signal handler writes through a
  descriptor gk validated against the thread's arena and keeps in its own
  table, and the isolate's jsg data slots -- among them the lock slot
  `jsg::Lock::from` reads, which is in the C++ isolate object in the shared
  heap and so writable from any isolate's turn -- are put back from the host
  frame that entered the guest once the turn returns, with the lock's
  constructor and destructor writing the slot and never reading it. What validation cannot settle is ownership: a pointer
  swapped for another live arena's, or a handle swapped for another live
  slot's, is indistinguishable from a legitimate call, so a turn can still
  name a different isolate's arena and cost that isolate its memory, or name
  another tenant's host-memory block. Nor does it help with the objects a turn hands back --
  the non-void results of `IoContext::run`, which are mostly `kj::Promise`,
  and the actions one context queues for another, closures the host moves
  by pointer and destroys when their context has ended -- which are heap
  objects carrying their own vtables and disposers. Those become safe when
  the runtime's mutable memory is out of ring 3's reach, not by marshalling
  them.
- **No cross-vCPU TLB shootdown IPIs yet.** Adding mappings needs none (a
  not-present entry has nothing stale to flush), which is why dynamic `mmap`
  and per-thread stacks work across vCPUs. Removing or shrinking a mapping
  (`munmap`, `mprotect` to less access) that another vCPU has cached is handled
  only lazily: the reflection runs under the global lock and flushes the faulting
  vCPU's TLB when a fault repeats at the same address, rather than sending an
  IPI-driven flush to every vCPU. So a stale entry on another vCPU can briefly
  retain the old permission until its next repeated fault; for a shared
  runtime page, whose entries are global, that vCPU's own root switches do not
  drop it either, only its next flush does.
- **No signal delivery** into the library's API yet; only faults are caught,
  via the in-guest IDT handlers. (Injecting a host signal as a guest interrupt
  is demonstrated separately and would be wired in here.)
- **Syscall policy is an embedder filter over a built-in memory broker.**
  `gk_set_syscall_filter` bounds which syscalls a turn may forward. On top of
  that, a ring-3 turn's forwarded syscall goes through a broker (see the ring-3
  syscall memory broker in `gk_linux.c` and `gk_hvf.c`): every pointer argument,
  including those nested in an iovec or msghdr, must name memory the turn could
  reach itself, and a syscall gk has no descriptor for, or one that acts on host
  rather than guest state, is refused. There is still no default syscall
  allowlist (the embedder supplies one) or host-side seccomp on the forwarding
  thread itself yet.
  workerd's filter (`guestKernelSyscallFilter` in
  `src/workerd/server/workerd.c++`) admits syscalls by number, and for the
  ones that change mappings also by range, through `gk_mapping_syscall_ok`:
  `mprotect`, `pkey_mprotect`, `munmap`, `madvise`, `mmap` with `MAP_FIXED`
  or `MAP_FIXED_NOREPLACE`, and `mremap` (its old range extended to the new
  size, and the destination of an `MREMAP_FIXED` move) are refused with
  `EPERM`, before anything reaches the host, when the range overlaps gk's
  supervisor or refused memory, an arena other than the one the calling
  thread is active in (every arena when none is), or the runtime image:
  the loaded segments of the executable and of the shared objects present at
  `gk_init`, and the main thread's stack. The calling thread's arena comes
  from gk's own record in supervisor memory, so ring 3 cannot change what the
  check compares against. An `mmap` the kernel places is not range-checked,
  since it can only land in free space and the process allocator grows during
  a turn. The path syscalls on the allowlist (`openat`, `newfstatat`,
  `statx`, `unlinkat`, `faccessat2`) are bounded by the turn directory (see
  "Turn directory" above): a turn has one only for the length of a native
  SQLite VFS call that names a file, beneath that VFS's root, and none
  otherwise, so an escaped isolate cannot open a file by path except a
  Durable Object's own database files during such a call. What that leaves
  open: `read`, `write`, `fcntl`, `mmap` and `close` take a descriptor by
  number, so an escaped isolate can still act on any file the process has
  open, and `socket`, `connect`, `bind`, `accept4`, `sendto`, `recvfrom` and
  the rest of the socket set are on the allowlist, so it can reach the
  network, as the process. The range check
  covers only the sets above: the heap, glibc's data, other threads' stacks
  and any object `dlopen`ed after `gk_init` can still be remapped or
  unmapped by a turn, as they can be written by it directly (see the first
  entry), and `brk` is unchecked. A kernel-placed `mmap` can also be steered
  by its hint into free space the runtime relies on staying free, such as the
  growth room below the main thread's stack.
- **The MMU backs whole 2 MiB windows, not exact VMAs.** It reparses
  `/proc/self/maps` per fault (each read also settles the fault's aligned
  256 KiB neighborhood, see `demand_map_neighbors` in `gk_pt.c`, so a growing
  mapping costs a read per 64 pages rather than per page), flushes the whole
  TLB on each `mprotect`/`munmap`,
  and only the faulting thread's TLB (no cross-vCPU shootdown). Backing in aligned
  2 MiB windows rather than exact mapping bounds is a **cost** choice, not a
  correctness one: exact per-page memslots are safe, but each memslot create
  costs ~20 us and a V8-sized heap of 4 KiB slots would exceed KVM's ~32764-slot
  limit. Neither choice lets the fault path allocate: the faulting guest thread
  may be inside glibc `malloc` (see the signal-safety note in `gk_region.c`).
  A production MMU could track
  exact VMAs (see the FreeBSD `sys/vm` note in `gk_region.c`) for density and add
  per-range cross-vCPU shootdowns; both are refinements, not correctness fixes.

## Running V8 inside gk (status)

A separate spike links this library against workerd's V8 and runs an isolate
inside the guest. With the demand-paged MMU it now **runs a JavaScript program
end to end**: `v8::V8::Initialize`, `Isolate::New`, context creation, compile and
execution all happen in guest ring 0, and the script returns the correct result.

What it took, beyond the MMU:

- `CR4.PKE` enabled and the host PKRU cleared: glibc `pkey_set` executes
  `rdpkru`, and V8 write-protects its pointer tables with a protection key that
  the host side must be able to back.
- V8 runs its whole lifecycle inside the guest, so its recorded thread and TLS
  state match where it executes (initializing on the host and running in the
  guest tripped a thread-local null dereference).
- `Isolate::SetStackLimit` for the guest stack: V8 auto-detects the stack via
  `pthread_getattr_np`, which returns the OS thread's stack, not the guest
  stack, so without this it reports a false stack overflow.

The JIT works too, with V8's default configuration (no special flags): a
JIT-optimized hot loop runs to the correct result across repeated runs. W^X plus
mprotect reflection handles the code pages that flip between writable and
executable, and the protection keys V8's sandbox assigns from inside the guest
are virtualized into it (see the PKU point above, including the gap for pages
keyed only from the host, which are isolated by the arena's root alone), so the
isolate runs with the sandbox enabled and its keys
honored, not stripped.

V8 also runs **multi-threaded** now, with its default platform: the background
GC/compiler worker threads V8 spawns are intercepted at `clone`/`clone3` and
enter the guest as vCPUs, and the whole isolate lifecycle (including joining the
workers on shutdown) runs in guest ring 0 with the correct result. The worker
pool's threads are long-lived, and the vCPU pool bounds the vCPU count even under
heavy thread churn.

So both jitless and JIT V8 run a JavaScript program end to end inside the guest.
A fuller solution would virtualize PKU (reflect the guest key state and the
page's key in the guest PTE) instead of stripping it. The spike is not part of
this library.
