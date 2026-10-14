# Guest-kernel on/off benchmarks

What per-isolate hardware isolation costs: request latency, throughput, where
the cycles go, and how many fewer isolates fit in a host. Each driver runs the
same workload twice, once with `WORKERD_EXPERIMENTAL_GUEST_KERNEL` unset and
once with it set, against the same binary.

## Status: reconstructed harness

The scripts here are a reconstruction. The originals ran on a machine that has
since been torn down and were never checked in, so these were rebuilt from the
recorded method: workload shapes, core pinning, durations, repeat counts, the
statistics taken, and the counter modifiers are all as used. Two things to know
before trusting a number produced here:

- The analysis scripts (`summarize.py`, `perf-split.py`, `iso-fit.py`) were
  checked against the recorded measurements and reproduce the published figures
  below, so the arithmetic and the counter handling are right.
- The drivers have not been re-run end to end since the rebuild, because that
  needs x86-64 hardware with `/dev/kvm`. Expect to debug a path or a flag on
  first use, and treat the tables below as the record of what was measured
  rather than as output of this code.

Two parameters were not recorded and are chosen here: the CDN object size
(64 KiB, in `workloads/origin.js`) and the exact JSON payload shape
(`workloads/json.js`). Absolute numbers for those two workloads therefore will
not match the tables below; the off-vs-on ratio is what carries over.

## Requirements

x86-64 Linux with read/write access to `/dev/kvm`, a `workerd` built with
guest-kernel support (`WORKERD_HAS_GUEST_KERNEL`), and `oha` 1.5 or newer as
the load generator (the drivers ask it for JSON with `--output-format json`).
`profile.sh` additionally needs `perf` and either root or
`kernel.perf_event_paranoid` at 1 or less.

## Running

```
export WORKERD=/path/to/workerd
./bench.sh                  # latency and throughput, all workloads, off vs on
./bench.sh hello cpu        # just these
./profile.sh                # guest-side vs host-side cycles per request
./iso-ramp.sh               # resident memory per idle isolate
```

Knobs are environment variables read by `common.sh`: `SERVER_CPUS`,
`CLIENT_CPUS`, `PORT`, `DUR`, `REPS`, `OUT`. The defaults describe a 24-core /
48-thread machine; set the two CPU lists for the machine in use before reading
any result.

## Workloads

| Workload | Shape | Why it is here |
| --- | --- | --- |
| `hello` | fixed string, no allocation or I/O | the floor: shows the fixed per-request cost with nothing to amortize it against |
| `json` | one serialize/parse round trip | a short request that does real work in the isolate |
| `cdn` | fetches a 64 KiB object from an origin worker and forwards it | I/O-heavy, and enters two isolates per request |
| `cpu` | about 1 ms of compute | long enough that a fixed per-request cost is a small fraction |
| `alloc` | a fresh 4 MiB `Uint8Array` per request, touched | memory-allocation-heavy; the one workload the arena makes faster |

## Method

The details below are the ones that changed results when they were got wrong.

**Pin the server and the client to disjoint cores.** The overhead is almost
entirely CPU, so a server sharing cores with the load generator measures the
scheduler. The published numbers used four physical cores for the server and a
separate set for the client.

**Warm before measuring.** The first requests into an isolate are parsing and
JIT; with isolation on they also fault the arena's pages in. Every driver warms
first and discards it.

**Take a median of repeats, and keep the spread.** A single short run varies by
several percent, and some of the deltas being measured are that small.
`oha-median.py` runs each measurement `REPS` times (3 by default), reports the
median, and also reports the spread of the repeats so a reader can tell a real
change from noise. A delta inside the spread is not a result.

**Measure both c=1 and c=8, and do not mix them up.** They answer different
questions, and for the same change they give very different percentages. At
c=1 the machine is idle and the result is pure added latency. At c=8 on four
cores the machine is oversubscribed, so there is no idle headroom to absorb the
extra CPU and the throughput loss is larger than the latency increase would
suggest. Quoting a latency delta from one and a throughput delta from the other
is how the overhead gets misreported.

**Ask the PMU for guest cycles explicitly.** This is the trap that invalidated
an earlier round of profiling. On AMD + KVM, cycles spent executing inside the
guest fall into neither `:u` nor `:k`, so a plain `perf stat -p <pid>` on a
workerd running JS in the guest reports a small fraction of the real cycles and
an apparent clock far below the actual one. The guest's cycles have to be
requested as `cycles:G`, the host's as `cycles:H`. `profile.sh` names a
modifier on every event. Confirm the reported frequency matches between the two
modes before reading a cycle delta as time.

**Compare like with like when splitting guest from host.** Stock workerd's
socket I/O already runs in the kernel; with isolation on that work relocates
behind a VM exit but is not new. So the added cost is

```
guest_extra = cycles:G(on) - cycles:u(off)
host_extra  = cycles:H(on) - cycles:k(off)
```

which is what `profile.sh` prints the two halves of.

**For isolate density, create every isolate and warm it.** `iso-gen.py` emits
N deliberately distinct workers on N sockets; identical module text would be
one script and one isolate. `iso-ramp.sh` sends a request to each before
reading RSS, then `iso-fit.py` fits `RSS = base + per_isolate * N` across the
series. The slope is the number that matters; a single large N conflates it
with the process baseline.

## Published results

Measured on an AMD EPYC 7443P (Zen 3, 24 cores / 48 threads, 256 GB, Ubuntu
24.04, kernel 6.8), four cores for the server, median of three 5-second runs.
Noise is roughly 5 to 10 percent; the `hello` off baseline was the least
reproducible cell, ranging 101 to 116 us across rounds.

### Latency and throughput

| Workload | lat c=1 off -> on | delta | rps c=1 off -> on | delta | rps c=8 delta |
| --- | --- | --- | --- | --- | --- |
| hello | 110 -> 144 us | +31% | 8508 -> 6822 | -20% | -42% |
| json | 131 -> 163 us | +25% | 7596 -> 6092 | -20% | -30% |
| cdn | 201 -> 309 us | +54% | 4950 -> 3222 | -35% | -45% |
| cpu | 1117 -> 1389 us | +24% | 894 -> 719 | -20% | -22% |
| alloc | 3102 -> 1261 us | **-59%** | 322 -> 792 | **+146%** | **+147%** |

The cost is a roughly constant 30 to 50 us of CPU per request, which is why it
hurts short requests most. `cdn` pays it twice because it enters two isolates.
`alloc` is 2.3 to 3.4x faster with isolation on: stock workerd spends about
8.7M kernel cycles per request demand-zeroing and unmapping a fresh 4 MiB
buffer, and the arena serves it from memory that is already mapped.

### Where the cycles go

Per request, from `profile.sh`. `guest_extra` and `host_extra` are the added
cost as defined above; the cycle deltas reproduce the measured latency deltas
to about 1%.

| Workload | off | on total | on guest | on host | guest_extra | host_extra | guest IPC | host IPC |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| hello | 398K | 519K | 188K | 332K | -105K | +226K | 0.66 | 0.46 |
| json | 525K | 641K | 297K | 345K | -102K | +219K | 0.81 | 0.47 |
| cdn | 1005K | 1492K | 729K | 764K | -64K | +551K | 0.57 | 0.42 |
| cpu | 4300K | 5417K | 4982K | 434K | +828K | +289K | 3.87 | 0.41 |
| alloc | 11709K | 4984K | 3086K | 1898K | +115K | -6840K | 2.66 | 0.33 |

Host-side IPC is 0.33 to 0.47 on every workload: that is the world-switch
layer, front-end-bound and heavily branch-mispredicted. For `hello` and `json`
the whole overhead is that fixed host layer. `cpu` is the only workload whose
overhead is mostly guest-side, and it is software work, not address
translation: guest IPC under isolation (3.87) matches the baseline, and forcing
2 MiB guest page-table entries cut dTLB misses by 43% while changing latency by
0.4%, so the cost is the runtime's own in-guest work (demand faults, syscall
emulation, memory management), not TLB pressure.

The absolute instruction and IPC digits carry maybe 10 to 15% uncertainty from
counter-mode differences; the robust signal is the 8 to 10x contrast between
guest and host IPC. Cycle counts agreed to better than 0.5% across modes.

### Memory and isolate density

Idle isolates, each warmed with one request, fitted over N = 1 to 150:

| | base | per isolate |
| --- | --- | --- |
| off | 42.6 MB | 1.63 MB |
| on | 42.5 MB | 2.65 MB |

That is +1.03 MB per isolate, or 63% more, so about 39% fewer isolates per GB
at the margin (roughly 645 down to 395). The process baseline is unchanged.
About 87% of the increase (~892 kB) is V8's own per-cage floor, paid because
each isolate gets its own `IsolateGroup` and sandbox rather than sharing one:
pointer-table segments and trusted/code-range pages that a shared cage pays
once. The guest kernel's own structures account for only about 63 kB per
isolate.

There is also a hard ceiling of about 185 isolates per process: an arena
occupies a whole 512 GiB PML4 slot, and only the slots between
`GK_FIRST_CAGE_SLOT` and `GK_USER_SLOTS` are available, less the handful the
binary, heap, libraries and stacks take. Exceeding it is fatal to the process,
not just to the isolate. The reservation is virtual
(`PROT_NONE`/`MAP_NORESERVE`), so it costs address-space slots rather than RAM;
lifting the ceiling means 5-level paging or slotting arenas at a finer
granularity than a PML4 entry.
