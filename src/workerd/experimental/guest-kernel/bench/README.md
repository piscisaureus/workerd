# Benchmarks for the guest-kernel prototype

Three suites, in the order they were written. Each answers a different
question, and the early ones are kept because they are what decided the design
was worth building.

| Directory | Question | Needs |
| --- | --- | --- |
| `baseline/` | What does a request cost in stock workerd? How many syscalls and isolate locks does it perform? | a workerd binary |
| `kvm-micro/` | How much does a VM exit cost, and a guest page-table switch? Do the isolation properties hold on real hardware? | `/dev/kvm` only |
| `gk-bench/` | What does per-isolate isolation cost end to end: latency, throughput, cycles, memory? | `/dev/kvm`, `oha`, `perf` |

The order is also the argument. `baseline/` counts the syscalls a request
makes, `kvm-micro/m5_exitcost.c` prices one forwarded syscall at a VM exit, and
together they said the design was affordable only if few syscalls per request
remain. `kvm-micro/m6_switchcost.c` then showed the isolate switch itself
costs about 60x less than an exit, so the cost lives in the crossings, not the
isolation. `gk-bench/` measures what it actually came to.

Everything here is x86-64 Linux. `gk-bench/` is a reconstruction of the harness
that produced the published numbers; see its README for exactly what that means
before quoting a figure from it.

## Headline

Per-isolate hardware isolation costs a roughly constant 30 to 50 us of CPU per
request, which is +24% latency on a 1 ms request and +31% on a trivial one, and
20% throughput at matched concurrency. An allocation-heavy workload is 2.3 to
3.4x *faster* with isolation on. Memory is +1.03 MB per isolate (+63%), about
87% of which is V8's per-cage floor rather than the guest kernel's own
bookkeeping, and there is a hard ceiling near 185 isolates per process.

The overhead is the host world-switch layer, not address translation: host-side
IPC is 0.33 to 0.47 on every workload, and cutting dTLB misses by 43% with
2 MiB guest pages changed latency by 0.4%. See `gk-bench/README.md` for the
full decomposition.
