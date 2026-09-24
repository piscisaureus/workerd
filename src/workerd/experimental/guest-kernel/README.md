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
- `fault-test.sh` / `fault-test.capnp` — end-to-end check against a built
  workerd that a fault inside the guest during a JS turn fails only that
  request and condemns only that isolate, and the process keeps serving.
- `surface-test.sh` / `surface-test.capnp` / `surface-test.js` / `add.wasm` /
  `oob.wasm` — end-to-end check against a built workerd that a broad V8 surface
  (Wasm including an out-of-bounds trap, WebCrypto, irregexp and a JIT-heavy
  loop) runs correctly with the worker's JS turn at guest ring 3, and that a
  Wasm bounds violation surfaces as a `WebAssembly.RuntimeError` rather than a
  guest fault.

## API

```c
int   gk_init(void);                         // become a guest-capable VM
long  gk_run(long (*fn)(void*), void *arg);  // run fn in the guest, forward syscalls

gk_arena *gk_arena_create(size_t size);      // a private per-isolate cage
void     *gk_arena_base(const gk_arena*);
gk_arena *gk_arena_enter(gk_arena*);         // select this thread's active cage (NULL = base)
void      gk_arena_destroy(gk_arena*);

void gk_set_syscall_filter(gk_syscall_filter); // bound what forwarded code may do
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

  This is the extent of the guarantee: cross-arena isolation and gk's own
  control state hold against arbitrary code execution inside an isolate. The
  shared, non-arena runtime memory does not: the C++/KJ heap, glibc, and every
  vCPU's guest stack (including the caller's stack that `gk_run_here_user` runs
  the turn on) are user-mapped and readable and writable from ring 3. A sandbox
  escape that reaches arbitrary code execution can therefore corrupt shared
  runtime state and, through a host return address on that shared stack, may
  reach host execution. Keeping the runtime's own memory out of ring 3's reach
  is remaining work (see Limitations).
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
  and confirms the guest gets `-EPERM` while other syscalls still work.
- **Demand-paged MMU.** The guest's memory is backed lazily. On a guest page
  fault the in-guest `#PF` handler hypercalls; the host checks the faulting
  address against its own address space and, if committed, backs it with a KVM
  memslot and a PTE, then the handler `iretq`s to retry. Backed ranges are
  tracked in an interval tree (a treap keyed by start) of non-overlapping
  windows, so every mapped page has a backing memslot. Memory is backed one
  aligned 2 MiB window at a time; memslots are created once and never deleted.
  Faults into another arena's region are refused, preserving isolation.
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
- **W^X.** Pages are mapped with their host protection: executable pages are not
  writable and writable pages are not executable (NX is enabled in the guest,
  and `CR0.WP` is set so the ring-0 guest cannot write a read-only page either).
  `mprotect` and `munmap` are reflected into the affected guest PTEs and the
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

Enabling SSE and AVX in the guest (`CR4.OSFXSR`, `OSXSAVE`, and `XCR0`) is
required before compiled code and glibc, which use those instructions, will
run; `gk_init` does this.

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
trip costs about 4.7 microseconds. The switch is roughly two orders of
magnitude cheaper than an exit, so batching the dominant syscalls is what
decides the overall cost.

## Limitations (remaining work)

- **The shared runtime memory is reachable from ring 3.** The ring-3 wall
  covers other isolates' arenas and gk's control state, not the runtime's own
  memory: the C++/KJ heap, glibc, and every guest thread's stack (the JS turn
  runs on the calling thread's stack, above host frames) are user-mapped. Code
  execution inside an isolate can corrupt that shared state and, via a host
  return address on the stack, potentially reach host execution. Confining it
  needs the runtime's memory, or at least its stacks and control-flow data, kept
  out of ring 3's reach.
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
- **Syscall policy is a single filter hook.** `gk_set_syscall_filter` bounds
  forwarded syscalls, but there is no default allowlist or host-side seccomp on
  the forwarding thread itself yet.
- **Dynamic mappings** that land in a fresh top-level (PML4) entry after an
  arena is created are not reflected into that arena's root. Allocations near
  existing mappings are, because the subtrees are shared.
- **The MMU backs whole 2 MiB windows, not exact VMAs.** It reparses
  `/proc/self/maps` per fault (each read also settles the fault's aligned
  64 KiB neighborhood, see `demand_map_neighbors` in `gk.c`, so a growing
  mapping costs a read per 16 pages rather than per page), flushes the whole
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
