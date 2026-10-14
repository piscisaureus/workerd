# Guest-kernel isolation spike

A standalone demonstration, on real hardware, of the memory-isolation idea
behind `gk` (see `../../README.md`): run a process inside KVM as guest ring 0
(the runtime, "kernelspace") and give each isolate its own hardware-enforced
memory view by switching guest page tables. This is the Dune model, reduced to
the smallest programs that prove each property.

These programs came first and stand alone: each is a single `.c` file that
depends on nothing but `/dev/kvm`, so a property can be confirmed, or a cost
measured, without the library in the way. The library they led to is
`../../gk.c`.

Everything here is x86-64 Linux and needs only read/write access to `/dev/kvm`.
No root, no kernel module, no device emulation. Each program uses the
documented KVM ioctl API and a few bytes of guest code, hand-encoded and
verified by disassembly.

## Build and run

```
make run        # builds and runs all nine milestones
```

Numbers below were measured on an AMD EPYC 7443P (Zen 3, 24 cores, Ubuntu
24.04, kernel 6.8) with the default CPU mitigations active.

## Milestones

| File | Property demonstrated |
| --- | --- |
| `m1_hypercall.c` | The host/guest boundary: guest code runs under KVM and calls back into the host through a port-I/O exit, which the host services and resumes. This is the primitive that becomes syscall forwarding. |
| `m2_longmode.c` | 64-bit long mode with guest-controlled four-level page tables; paging verified by a write through a virtual address and a read back. |
| `m3_isolation.c` | **The core.** Two page-table roots as two isolates. One guest-virtual "cage" address resolves to different physical memory under each root, while the runtime's pages are shared. A guest `mov cr3` switches isolates; the same pointer reads A, then B, then A. |
| `m4_fault.c` | The boundary. Isolate B has a private page, present only in root B. Under root A the same address is unmapped, so touching it raises a page fault that the guest's own IDT handler catches. The runtime is kernelspace handling the isolate's illegal access. |
| `m5_exitcost.c` | Gate 1: the VM-exit round-trip cost, since every forwarded syscall is an exit. |
| `m6_switchcost.c` | Gate 1: the CR3 switch cost, measured in-guest with RDTSC, confirming the switch does not trap. |
| `m7_syscall.c` | Syscall forwarding. The guest points LSTAR at a trampoline; a `syscall` traps there, hypercalls to the host, which performs the real syscall and resumes. Forwards `write(1, ...)` and `getpid()`. |
| `m8_guest_enter.c` | Dune-style enter. A real compiled C function runs inside the guest at its actual address, with the process's own memory visible through guest-physical == host-virtual page tables, and its syscalls forwarded. The leap from hand-encoded payloads to running compiled code. |
| `m9_signal.c` | Asynchronous signal delivery. The guest runs with interrupts enabled; a helper thread fires a timer signal that interrupts the vCPU, and the host injects it as a guest interrupt that an in-guest IDT handler services and returns from with `iretq`. |

## Results

| Primitive | ns | cycles |
| --- | --- | --- |
| CR3 switch (in-guest, incl. code-path TLB refill) | 76 | 217 |
| VM-exit round trip (port I/O, handled in userspace) | 4660 | 13300 |

The switch is about 60x cheaper than an exit. The exit is about 13x a native
`getppid` (347 ns, measured by `../baseline/syscall-cost.c`), inflated by the
AMD return-thunk and retpoline mitigations that apply to every world switch.
This is why the design's cost is decided by how many exits remain per request,
not by the switch.

## What this does not cover

- Running workerd itself inside the guest. m8 enters a single hand-written
  function through raw syscalls; a real workload needs libc mapped (TLS and
  the FS base), the full `/proc/self/maps` mapped rather than the few regions
  m8 covers, and a broad syscall-forwarding table. That is what the library
  in `../..` goes on to do.
- Cross-vCPU TLB shootdown hypercalls.
- Batching or shared-memory I/O rings, which would remove the per-syscall
  exit that `m5` measures.

## Where this led

The primitives above are packaged into the reusable library in `../..`:
`gk_init()` to make the process a guest-capable VM, `gk_run()` /
`gk_run_user()` to run a function in the guest with its syscalls forwarded,
and `gk_arena_*` for the per-isolate hardware-isolated memory cages. See
`../../README.md`.
