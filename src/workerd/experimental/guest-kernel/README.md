# gk: a guest-kernel isolation library (prototype)

`gk` runs the calling process inside a KVM virtual machine as its own guest
ring 0 (the Dune model), forwards its syscalls to the host, and gives each
isolate a private memory arena backed by a separate guest page-table root. The
goal is per-isolate, hardware-enforced memory isolation with no fixed cap on
the number of isolates, using upstream KVM rather than a bespoke hypervisor.

This is a research prototype. It is x86-64 Linux only and needs read/write
access to `/dev/kvm` (no root, no kernel module). The library builds and runs
standalone with the included Makefile (`make run`); on x86-64 Linux it is also
built as a Bazel `cc_library` and wired into the workerd runtime behind the
`WORKERD_EXPERIMENTAL_GUEST_KERNEL` environment variable, which runs each
isolate's JS turn at guest ring 3.

## Files

- `gk.h`, `gk.c` — the library.
- `gk_asm.S` — the in-guest trampolines and exception handlers.
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
- `fault-test.sh` / `fault-test.capnp` — end-to-end check against a built
  workerd that a fault inside the guest during a JS turn fails only that
  request and condemns only that isolate, and the process keeps serving.
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
  residual the table cannot close, stated as such; a turn's write into
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
                                             // inside the guest, no VM exit (about 19 ns)

int   gk_signal_field_register(uintptr_t off, size_t len, uint32_t field, unsigned kind,
                               gk_signal_handle *h); // a word in the thread's arena a signal handler may write
int   gk_signal_field_unregister(gk_signal_handle);
long  gk_signal_field_write(unsigned kind, uint32_t value); // from a signal handler: no lock, no allocation

int   gk_thread_words_set(unsigned first, unsigned n, const uint64_t *v); // per-thread words in supervisor memory
int   gk_thread_words_get(unsigned first, unsigned n, uint64_t *v);       // for state a signal handler reads
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
  generation}`, registered `GK_PROT_SUPER` -- a weaker class than the
  `GK_PROT_REFUSE` of host memory and the page tables, since a SUPER page is
  in every root with `U` clear while a REFUSE page is in none, and the right
  one here because the lookup has to run inside the guest; host memory stays
  on REFUSE -- and a handle is validated as a host slot handle is: the index
  is the handle's low 20 bits, which the capacity of exactly 2^20 bounds by
  masking, the entry must be live and its generation the handle's, so a
  handle to an unregistered or recycled entry resolves to nothing and
  nothing is ever read through the handle's own value. The lookup takes no
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
  entry, and only a pointer to ordinary user memory -- outside gk's
  supervisor and refused ranges and outside every arena -- so a turn can
  launder nothing into the table that it could not already write directly,
  and cannot aim the host at gk's state, host memory or another arena. The
  capacity is set from the measured live count: five entries per in-flight
  request parked on a timer, eight per open WebSocket (memory-classes.md,
  Cost), so 2^20 holds about two hundred thousand such requests, in 16 MiB
  that cost nothing until written. The test registers an entry and resolves it
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
  timer interrupts (`irq_exits`) and are discarded. **18.8 ns per resolve**
  on the test machine, the median over kept sub-runs with the empty turn's
  cost subtracted, against 6.4 us for a hostcall: 340 times cheaper. A
  negative build (`GK_NEG_RESOLVE`, in `gk_test_neg`) removes the generation
  check and the case fails on the never-issued generations and the recycled
  slot. It also reserves three entries, sees them resolve to nothing from
  ring 3 until committed, commits one and sees it take once, and sees a
  commit refused for a committed, retired or never-issued handle and for a
  NULL, supervisor, refused or arena pointer, from commit and from register
  alike. The guard build runs the host-side path with allocation forbidden;
  the ring-0 path is asm with no call in it. What it does not give: ring 3
  can resolve as often as it likes with any handle, and learns only the
  pointer behind a live handle it holds, which the lookup makes safe to keep
  in guest memory but does not make safe to reach from there -- the object's
  own placement does that -- and a handle rewritten to another live entry's
  names that entry, the ownership residual the arena and host slot facilities
  carry too (see `memory-classes.md`, Cost, for what a turn's dereference
  count implies).
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
  and the runner to the next CPU on a different core, and hello world then
  costs 68 us with both sides sleeping on a futex through the handoff
  (`GK_RUNNER_SPIN=0`, the default; two wakeups per turn, one logical CPU
  busy, 83 us of CPU per request against 118 to 144 with the guest kernel
  on and 74 with it off) or 59 us with both spinning (`GK_RUNNER_SPIN=-1`,
  which keeps a second logical CPU busy for the whole turn). Unpinned, the
  scheduler usually separates the two threads and hello world costs 93 us.
  On the owner's SMT sibling (`GK_RUNNER_PIN=sibling`) the runner recovers
  nothing: the flush empties predictor state the core's two hyperthreads
  share. `gk_stats.runners` and `runner_runs` count the runners and their
  runs. What the runner leaves, and what taking it would need, is in the
  findings.
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
which stays inside the guest, costs about 19 ns on the Zen 5 test machine,
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
  another isolate's live resolve entry: the
  table turns "any address" into "one of the live tenants", and telling a
  legitimate use from a swapped one is an ownership question the table cannot
  answer, as with arenas below. Every other object listed above is still in
  the shared heap. Moving the malloc'd objects into the arena needs a
  per-isolate allocator arena, which means an allocator with extent hooks;
  workerd uses tcmalloc, which has none.
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
- **Syscall policy is a single filter hook, and it leaves files and the
  network open.** `gk_set_syscall_filter` is the only gate on a forwarded
  syscall: gk has no default allowlist, and no host-side seccomp on the
  forwarding thread. workerd's filter (`guestKernelSyscallFilter` in
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
  256 KiB neighborhood, see `demand_map_neighbors` in `gk.c`, so a growing
  mapping costs a read per 64 pages rather than per page), flushes the whole
  TLB on each `mprotect`/`munmap`,
  and only the faulting thread's TLB (no cross-vCPU shootdown). Backing in aligned
  2 MiB windows rather than exact mapping bounds is a **cost** choice, not a
  correctness one: exact per-page memslots are safe, but each memslot create
  costs ~20 us and a V8-sized heap of 4 KiB slots would exceed KVM's ~32764-slot
  limit. (The heap corruption once blamed on on-demand memslot creation was
  actually fault-path re-entrancy into glibc `malloc` — the memslot ioctl is not
  involved; see the signal-safety note in `gk.c`.) A production MMU could track
  exact VMAs (see the FreeBSD `sys/vm` note in `gk.c`) for density and add
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
