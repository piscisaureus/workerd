# gk: a guest-kernel isolation library (prototype)

`gk` runs the calling process inside a KVM virtual machine as its own guest
ring 0 (the Dune model), forwards its syscalls to the host, and gives each
isolate a private memory arena backed by a separate guest page-table root. The
goal is per-isolate, hardware-enforced memory isolation with no fixed cap on
the number of isolates, using upstream KVM rather than a bespoke hypervisor.

This is a research prototype. It is x86-64 Linux only, needs read/write access
to `/dev/kvm` (no root, no kernel module), and is built with the included
Makefile rather than Bazel. It is not wired into the workerd runtime; the
`BUILD.bazel` here only marks the directory as its own package so the runtime
build never globs these sources.

## Files

- `gk.h`, `gk.c` — the library.
- `gk_asm.S` — the in-guest trampolines and exception handlers.
- `gk_test.c` — the test/example.
- `Makefile` — `make run` builds and runs the test.

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
- **Arena isolation.** Two arenas act as two isolates. Under arena A's root,
  A's cage is readable and B's cage is not merely denied but unmapped: touching
  it faults, and the fault address equals B's base.
- **Threads as vCPUs.** Each host thread that enters the guest gets its own
  vCPU, stack and TLS base, sharing the VM and memory. Four threads run in the
  guest concurrently, and two threads each locked into their own arena run at
  the same time, each isolated from the other's cage.
- **Syscall policy.** A filter can bound what forwarded code may do; a denied
  syscall is not forwarded and the guest sees `-EPERM`. The test denies `write`
  and confirms the guest gets `-EPERM` while other syscalls still work.
- **Demand-paged MMU.** The guest's memory is backed lazily. On a guest page
  fault the in-guest `#PF` handler hypercalls; the host checks the faulting
  address against its own address space and, if committed, backs it with a KVM
  memslot and a PTE, then the handler `iretq`s to retry. Memslots are created
  per aligned 32 MiB chunk, so they never overlap and every mapped page is
  backed. Faults into another arena's region are refused, preserving isolation.

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

- **No cross-vCPU TLB shootdown yet.** Adding mappings needs none (a
  not-present entry has nothing stale to flush), which is why dynamic `mmap`
  and per-thread stacks work across vCPUs. Removing or shrinking a mapping
  (`munmap`, `mprotect` to less access) that another vCPU has cached would
  need an IPI-driven flush; that is not implemented.
- **No signal delivery** into the library's API yet; only faults are caught,
  via the in-guest IDT handlers. (Injecting a host signal as a guest interrupt
  is demonstrated separately and would be wired in here.)
- **Syscall policy is a single filter hook.** `gk_set_syscall_filter` bounds
  forwarded syscalls, but there is no default allowlist or host-side seccomp on
  the forwarding thread itself yet.
- **Dynamic mappings** that land in a fresh top-level (PML4) entry after an
  arena is created are not reflected into that arena's root. Allocations near
  existing mappings are, because the subtrees are shared.
- **The MMU is a fixed-chunk scheme.** It maps everything writable and reparses
  `/proc/self/maps` per fault. A production MMU should track VMAs in an interval
  tree (see the FreeBSD `sys/vm` note in `gk.c`) and create memslots that follow
  `mmap`/`mprotect`/`munmap` exactly, honoring per-page protection and issuing
  cross-vCPU TLB shootdowns.
- **Not integrated with the workerd build.** Integrating with V8's cage would
  require the V8 sandbox to be enabled in the build first.

## Running V8 inside gk (status)

A separate spike links this library against workerd's V8 and enters an isolate
inside the guest. With the demand-paged MMU plus `CR4.PKE` (glibc `pkey_set`
uses `rdpkru`) and clearing the host PKRU (V8 write-protects its pointer tables
with a protection key), V8 initializes, `v8::Isolate::New` completes, and
hundreds of pages are demand-mapped correctly. The next barrier is a V8
thread-local access during context/compile setup (V8 recorded stack/TLS state on
the host thread but runs on the guest stack); resolving that is the remaining
work before a script runs end to end. The spike is not part of this library.
