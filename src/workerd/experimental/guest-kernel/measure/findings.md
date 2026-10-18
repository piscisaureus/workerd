# Density: the guest kernel against a process per isolate

## The question

The guest kernel runs each V8 isolate's JS turn inside a KVM guest at ring 3
with the isolate's memory in a private page-table arena, so that many isolates
share one process and one event loop and still have hardware memory isolation.
The alternative that needs no new machinery is one operating-system process per
isolate. The claimed advantage of the guest kernel over that alternative is
density: no kernel scheduler entity per isolate, and a switch between isolates
that is a page-table load rather than a context switch plus an IPC.

The criterion for the comparison, fixed before the numbers were in: if a
process per isolate comes within about 20% of the guest kernel on marginal
memory and can sustain the dispatch and cold-start rates, the guest kernel is
not worth its complexity.

## The machine and the build

One AMD EPYC 9255 (Zen 5), 24 cores and 48 threads, 376 GB, Linux 7.0.0-38,
KVM with NPT, transparent huge pages on `madvise`, KSM available. Everything
below is from an optimized build (`bazel build -c opt
//src/workerd/server:workerd`) of the current tree. Build configuration turns
out to matter a great deal; the last section says why.

The CPU's speculative-execution mitigations were all at their defaults, which
on this machine means the VMSCAPE mitigation is **on**
(`/sys/devices/system/cpu/vulnerabilities/vmscape`). That one matters to any
comparison involving the guest kernel, because its cost falls on guest-host
transitions, which is the side of the ledger the guest kernel pays. Check that
file before trusting a repeat of these numbers: a run with the mitigation
disabled flatters the guest kernel for a reason no deployment can rely on, and
the figures here would not be comparable to it.

## Method

A tenant is one worker with its own V8 isolate. The arrangements are:

- **one process, guest kernel off** -- a single workerd holding every tenant.
  This has no isolation between tenants and is the floor, not a candidate.
- **one process, guest kernel on** -- the same, with
  `WORKERD_EXPERIMENTAL_GUEST_KERNEL` set.
- **one process per tenant** -- one single-tenant workerd per tenant, launched
  independently.

Every tenant is the same handler with a distinct constant, so no two compile to
identical bytecode and the arrangements differ only in how the isolates are
housed. Each tenant gets one request before measurement so its isolate really
exists. The scripts are beside this document and `run-all.sh` reproduces
everything.

Memory is read from `/proc/<pid>/smaps_rollup`, separating USS (private, the
true marginal cost of one more tenant), PSS (shared pages charged
proportionally, the right total when processes share the binary) and RSS, with
`MemTotal - MemAvailable` across the whole system as a cross-check that catches
kernel-side cost the per-process numbers miss. The marginal cost is the slope
of a least-squares fit across a sweep of tenant counts rather than a total
divided by a count, because the fixed cost is large: around 45 MB per workerd.
The fits are clean (R-squared 0.9999 or better) and the per-point differences
converge, so the slope is the marginal cost and not an artifact of the range.

**Fairness to the process model.** N independently launched processes share the
binary's text through the page cache but duplicate everything they dirty. A
real process-per-isolate design would pre-fork from a warmed parent and keep
far more through copy-on-write. Independent launches are therefore an upper
bound. To bracket the pre-forked floor without building it, the same
arrangement is measured again with KSM merging the tenants' identical anonymous
pages (`ksmshim.c` sets `PR_SET_MEMORY_MERGE` on each process). KSM merges
pages however they came to be identical, which is a superset of what survives a
fork, so what it leaves is a floor on the pre-forked design rather than a
prediction of it. Because near-identical tenants flatter any page-sharing
scheme, the whole memory sweep is also run with tenants of about fifty
kilobytes of generated source each, no two alike (`memdiverse.py`).

## 1. Marginal memory per tenant

Sweep of 1, 2, 4, 8, 16, 32, 64, 128, 256 tenants; the guest kernel stops at
128 because it runs out of page-table slots (see the ceiling, below).

| arrangement                     | marginal USS | marginal PSS | marginal system | fixed cost | tenants/GB |
| ------------------------------- | ------------ | ------------ | --------------- | ---------- | ---------- |
| one process, guest kernel off   | 1.60 MB      | 1.60 MB      | 1.61 MB         | 44.6 MB    | 640        |
| one process, guest kernel on    | 2.60 MB      | 2.60 MB      | 2.76 MB         | 44.8 MB    | 394        |
| process per tenant, independent | 8.92 MB      | 8.98 MB      | 10.34 MB        | 40.6 MB    | 114        |
| process per tenant, KSM-merged  | 4.52 MB      | 4.74 MB      | 7.15 MB         | 41.6 MB    | 216        |

With tenants of about fifty kilobytes of distinct source each, marginal per
tenant:

| arrangement                     | USS     | PSS     | system  |
| ------------------------------- | ------- | ------- | ------- |
| one process, guest kernel off   | 1.85 MB | 1.85 MB | 1.76 MB |
| one process, guest kernel on    | 2.86 MB | 2.86 MB | 4.25 MB |
| process per tenant, independent | 8.90 MB | 9.60 MB | 9.16 MB |
| process per tenant, KSM-merged  | 4.54 MB | 5.34 MB | 8.31 MB |

So the process model costs **3.4 times** the guest kernel's marginal memory as
launched independently, and **1.8 times** at the deduplicated floor. Tenant
diversity moves this very little: the floor rises from 4.74 to 5.34 MB while
the guest kernel rises from 2.60 to 2.86 MB, leaving the ratio at 1.9. On a
system basis, which counts the page tables and kernel structures the process
model needs and the per-process numbers do not show, the gap is wider: 2.76 MB
against 7.15 to 10.34 MB, a factor of 2.6 to 3.7.

The guest kernel's own penalty over no isolation at all is +63% marginal memory
(2.60 against 1.60 MB) and -38% tenants per GB (394 against 640). Those two
figures match what earlier work on different hardware reported, +63% and -39%.

**A ceiling the process model does not have.** Each isolate's arena is a 480 GB
reservation that occupies one 512 GB page-table slot, and gk places arenas from
PML4 slot 64 up to slot 255 (`GK_FIRST_CAGE_SLOT`, `GK_USER_SLOTS` in `gk.c`).
Bisection puts the limit at **183 tenants in one process**; the 184th fails with
"failed to create an isolate arena" and the process aborts. The cap follows
from the guest using four-level paging, so a larger host address space does not
lift it; it would take either five-level guest paging or more than one arena per
slot. Amortizing a workerd's 44.8 MB over 183 tenants adds 0.24 MB per tenant,
so the practical marginal figure for the guest kernel is 2.84 MB. A process per
tenant has no analogous cap.

**And the slot is not reclaimed in practice, so the cap is cumulative rather
than concurrent.** A slot comes back only when `gk_arena_destroy` runs, which
has one caller, `~GuestArena` (`jsg/setup.c++`), reached only when a
`jsg::IsolateBase` is destroyed. workerd holds every worker's isolate in its
service table for the life of the process and never drops one: running the
probe suite and a two-worker `workerd test` under `GK_DEBUG=1`, which logs
every destroy, recorded **zero** of them, and a condemned isolate is fenced
off rather than evicted (the `TODO(guest-kernel)` on `Worker::Isolate::condemn`
in `io/worker.h`). So 183 is the number of distinct isolates a process may ever
create, not the number it may hold at once. That is immaterial to a workerd
serving a fixed configuration, and it is the difference between a bound and a
leak for a long-running runtime that cycles tenants: such a runtime needs
isolate eviction that reaches `gk_arena_destroy` before the density figures
above describe it at all.

## 2. Cross-tenant dispatch

A request arrives at one tenant, which hands it to a second tenant through a
service binding and returns what that one answered. The difference from a
request the first tenant answers itself is the cost of the crossing. One
connection, two runs of twenty seconds each, median of the p50s.

| arrangement                   | own tenant | second tenant | crossing | share of the request |
| ----------------------------- | ---------- | ------------- | -------- | -------------------- |
| one process, guest kernel off | 23.5 us    | 47.0 us       | 23.5 us  | 50%                  |
| one process, guest kernel on  | 132.4 us   | 268.3 us      | 135.9 us | 51%                  |
| two processes, loopback TCP   | 23.3 us    | 63.9 us       | 40.6 us  | 64%                  |
| two processes, unix socket    | 23.5 us    | 63.4 us       | 39.9 us  | 63%                  |

**The claim does not hold.** A crossing under the guest kernel costs 136 us
against 41 us between two processes: the guest kernel is **3.3 times more
expensive** than the IPC it was meant to replace. The page-table switch itself
is cheap -- 76 ns, measured earlier and not contradicted here -- but it is not
what a crossing costs. A crossing costs a second guest turn, and a guest turn
is the expensive thing. The transport is not the process model's problem
either: a unix socket is no faster than loopback TCP, so the 41 us is the
second process's HTTP handling, its JS turn and a scheduler wakeup, not the
socket.

The same tax falls on requests that cross nothing. A tenant answering for
itself takes 23.5 us without the guest kernel and 132.4 us with it: **+109 us
on every request**, a factor of 5.6.

## 3. Cold start

Time to first successful response for a tenant that does not exist yet, as a
new worker loaded into a running process through a worker-loader binding
(median over 40 fresh names, with the same request against an already-loaded
name as the warm baseline), and as a whole new workerd (median over 30 spawns,
from exec to the first 200).

| way in                          | guest kernel | time to first response | warm baseline | the new isolate |
| ------------------------------- | ------------ | ---------------------- | ------------- | --------------- |
| new worker in a running process | off          | 1.72 ms                | 0.13 ms       | 1.58 ms         |
| new worker in a running process | on           | 6.60 ms                | 0.39 ms       | 6.21 ms         |
| a whole new process             | off          | 9.98 ms                | -             | 9.98 ms         |
| a whole new process             | on           | 21.34 ms               | -             | 21.34 ms        |

The process model sustains this. A whole workerd, started cold with no pooling
and no pre-forking, answers in 9.98 ms, which is within **1.5 times** of the
6.60 ms the guest kernel needs for a new isolate in a process that is already
running. The guest kernel's advantage here is small because it makes isolate
creation itself 3.9 times slower: 1.58 ms becomes 6.21 ms. A pre-forked pool
would put the process model at or below the 1.58 ms that creating the isolate
costs on its own, which is better than the guest kernel manages.

## What the three numbers leave out

**Requests per second.** One workerd serves every tenant from a single event
loop; `server.c++` sets up one `kj::AsyncIoContext` and waits on it. So a
process holding n tenants serves them from about one CPU, whatever n is, while
n processes serve them from n CPUs. Sixteen tenants, each kept busy on two
connections:

| arrangement                         | total rps | logical CPUs busy | rps per logical CPU |
| ----------------------------------- | --------- | ----------------- | ------------------- |
| one process, guest kernel off       | 31,024    | 2.09              | 14,857              |
| one process, guest kernel on        | 7,541     | 1.13              | 6,690               |
| process per tenant                  | 443,016   | 19.75             | 22,428              |
| process per tenant, guest kernel on | 118,544   | 17.77             | 6,669               |

A process per tenant serves **59 times** the aggregate rate of one guest-kernel
process holding the same sixteen tenants, because it uses the machine and the
single process cannot. Per CPU, which is what sizing a fleet uses, the guest
kernel costs **3.4 times** as much: 6,690 against 22,428 requests per second
per logical CPU. The last row shows the guest kernel's cost scales across
processes with nothing shared behind it -- 6,669 against 6,690 per CPU -- so
that factor is a property of the mechanism, not of the one-process arrangement.

Total CPU per request, measured as the server's utime+stime across a load run
divided by the requests that run completed, three rounds each:

| arrangement                   | hello                  | a 1000-iteration loop  |
| ----------------------------- | ---------------------- | ---------------------- |
| one process, guest kernel off | 72.4 us (69.6-73.0)    | 108.2 us (104.8-109.7) |
| one process per tenant        | 73.2 us (72.8-74.6)    | 107.0 us (106.7-108.1) |
| one process, guest kernel on  | 140.8 us (134.8-165.2) | 188.8 us (188.0-190.4) |

The two arrangements without the guest kernel are the same configuration and
agree to within 1%, which is the check that the measurement is sound.

**Where the tax goes.** It is not the crossings, and batching them cannot
recover it.

| guest kernel | worker | cycles/req | instructions/req | IPC  | VM exits/req | minor faults/req |
| ------------ | ------ | ---------- | ---------------- | ---- | ------------ | ---------------- |
| off          | hello  | 299,783    | 265,463          | 0.89 | 0.00         | 0.15             |
| on           | hello  | 514,440    | 281,004          | 0.55 | 1.89         | 0.80             |
| off          | loop   | 345,206    | 284,341          | 0.82 | 0.00         | 0.14             |
| on           | loop   | 601,489    | 313,112          | 0.52 | 1.83         | 0.69             |

A request executes 6% more instructions under the guest kernel and takes 72%
more cycles to do it; IPC falls from 0.89 to 0.55. It makes **fewer than two VM
exits per request**, and the library's own figures price an exit at 5.9 us and
an empty guest turn at 6.8 us, so crossings account for at most about 10 us of
the 68 us of extra CPU and the 109 us of extra latency. The rest is the same
code running slower, on both sides of the exit: a stall profile. Section 4
attributes it. It is not nested paging, whose page walks are priced at about
3% of the extra cycles, and not the arena root switch, which a single tenant
never makes; it is the kernel's return to userspace after the exit, after
which the code that finishes the request runs with cold branch predictors.
(These figures count the whole process; section 4 counts the event-loop
thread alone, where the request runs, and finds 9% more instructions for 3.7
times the cycles. The difference is V8's background threads, whose cycles the
guest kernel does not change and which dilute the per-process figure.)

## 4. Where the guest kernel's CPU goes

One workerd, one tenant, two handlers: hello, and a heavier one that builds,
serializes and parses a 200-key object. For each, the latency one connection
sees, then under a four-connection load the hardware counters of the
event-loop thread alone, split into guest mode and host mode and divided by
the requests served in the window; medians of three rounds (`stall.py`,
`stall.json`). The VMSCAPE mitigation was on throughout, as recorded in
`stall.json` by the script.

The split is the attribution. Host-mode code is native: no nested paging, no
arena root, nothing the guest kernel changes about its page walks. Cycles the
host side loses over the same instructions with the guest kernel off are
therefore not page-walk or TLB cost, and the counters say what they are.

| per request, event-loop thread   | off hello | on hello, guest | on hello, host | on hello, total | off heavy | on heavy, guest | on heavy, host | on heavy, total |
| -------------------------------- | --------- | --------------- | -------------- | --------------- | --------- | --------------- | -------------- | --------------- |
| cycles                           | 124.8k    | 209.9k          | 254.7k         | 464.5k          | 554.6k    | 798.4k          | 264.8k         | 1,063k          |
| instructions                     | 237.5k    | 125.6k          | 134.1k         | 259.7k          | 2,273k    | 2,489k          | 134.5k         | 2,623k          |
| IPC                              | 1.90      | 0.60            | 0.53           | 0.56            | 4.10      | 3.12            | 0.51           | 2.47            |
| branch mispredicts               | 145       | 1,640           | 3,034          | 4,674           | 1,203     | 4,632           | 3,217          | 7,849           |
| of which returns                 | 43        | 9               | 1,279          | 1,288           | 53        | 26              | 1,356          | 1,382           |
| instruction fills from beyond L2 | 356       | 2,568           | 3,951          | 6,519           | 2,657     | 7,401           | 5,747          | 13,148          |
| op-cache misses                  | 24.4k     | 51.0k           | 55.0k          | 106k            | 59.0k     | 107k            | 58.9k          | 166k            |
| dispatch slots empty, front end  | 380k      | 1,141k          | 1,620k         | 2,761k          | 827k      | 2,278k          | 1,684k         | 3,962k          |
| dispatch slots stalled, back end | 390k      | 320k            | 249k           | 569k            | 1,335k    | 1,428k          | 247k           | 1,675k          |
| page walks (data and instruction) | 35       | 78              | 43             | 121             | 79        | 121             | 67             | 188             |
| L1 ITLB misses that hit the L2 TLB | 3,030   | 4,822           | 2,797          | 7,619           | 3,336     | 5,754           | 2,687          | 8,441           |
| root switches                    | 0         | 0               |                | 0               | 0         | 0               |                | 0               |
| VM exits to userspace            | 0         |                 |                | 1.03            | 0         |                 |                | 1.09            |
| VM exits, all                    | 0         |                 |                | 1.29            | 0         |                 |                | 1.36            |
| p50, one connection              | 23.7 us   |                 |                | 139.4 us        | 123.7 us  |                 |                | 273.8 us        |

The core dispatches up to eight ops per cycle, so the slot counts are read as
cycles by dividing by eight. With the guest kernel on, the front end delivers
nothing to dispatch in 74% of all slots, against 38% without it; the back end
stalls 15% against 39%.

**The breakdown, hello.** The request costs 340k more cycles on this thread
(3.7 times), of which:

- **87% is front-end starvation**: the extra 2.38M empty dispatch slots are
  297k cycles. The front end has nothing because its branch predictions are
  gone: 4,674 mispredicts per request against 145, among them 1,279
  mispredicted returns on the host side against 43, and 18 times the
  instruction fills from beyond L2.
- **7% is back-end stall** (22k cycles), and the page walks are inside that.
  `walk-cost.c` prices a 4 KiB walk at 23.7 ns natively and 30.9 ns inside the
  guest, where it resolves through the host's nested tables at every level
  (20.5 ns when the host backs the memory with 2 MiB pages; `walk-cost.log`,
  and a repeat agreed to within 2 ns); at the 4.3 GHz the thread ran at, 102
  against 133 cycles. The guest side's 78 nested walks and the host side's 8
  extra native ones are then **11k cycles, 3.3%** of the extra, and that is
  an upper bound: the pointer chase that prices a walk serializes them,
  while an instruction stream overlaps them with other work.
- **0% is TLB refill after a root switch**, because there is none: a single
  tenant's vCPU already holds its arena's root and `sync_cr3` loads nothing.
  With two tenants whose requests alternate ("Two tenants alternating",
  below), every turn switches and the guest side walks 95 more times per
  request: about 12k cycles, 3.6% of the single-tenant extra.
- the remaining 6% is the 22k extra instructions at the native rate, and
  noise.

**Why the front end is cold.** The host side of the request is the evidence,
because nothing about it is virtualized: the same code at IPC 0.53 instead of
1.90, with 21 times the mispredicts. What happened to it is the kernel's
return to userspace after the VM exit that ended the turn. With the VMSCAPE
mitigation active, that return issues an IBPB, which discards the CPU's
indirect branch predictions, and on this CPU the exit path discards every
return prediction as well. `after-exit.c` isolates it: a fixed, fully
predictable 350k-instruction workload, run repeatedly on a pinned CPU with
one event between runs.

| between runs             | TSC ticks per run | mispredicts per run | of which returns | L1i fills from L2 per run |
| ------------------------ | ----------------- | ------------------- | ---------------- | ------------------------- |
| nothing                  | 422k              | 8,180               | 0                | 44k                       |
| one trivial syscall      | 440k              | 8,710               | 0                | 46k                       |
| one IBPB                 | 973k              | 23,500              | 12               | 150k                      |
| one `KVM_RUN` round trip | 1,402k            | 35,550              | 8,170 (all)      | 206k                      |

A syscall costs the code after it 4%. An IBPB alone (one `wrmsr` of
`PRED_CMD` through the msr driver, no VM involved) costs it 2.3 times. The
round trip through a guest whose first instruction exits -- one VM exit, one
return to userspace -- costs it 3.3 times (3.0 in the repeat recorded in
`after-exit.log`), mispredicts every one of its 8,000
returns and fetches 4.7 times as many instruction lines, because a front end
that does not know where the branches are fetches past them. The guest side
of a turn enters in the same state: its predictions were on that CPU at the
previous turn's return, and the flush took them too, which is why hello's
125.6k guest instructions run at IPC 0.60.

The instruction fills from beyond L2 are a consequence, not a second cause.
`after-exit` with a 535 KiB code footprint (more than the L1i, less than the
1 MiB L2) shows no loss of L2 contents across the round trip, 28 against 50
fills from beyond L2 per run, so the L2 keeps the code; the cold front end's
wrong-path fetches are what push the request's own code out of it.

**The exits.** 1.03 exits to userspace per request, which is the end of the
turn, plus 0.25 nested page faults the kernel handles without returning to
userspace (a host page the arena lost to V8's background sweeper, faulted
back) and a few timer interrupts. Nothing mid-turn remains to batch away.

**What this says about the other levers.** Huge pages on the guest's tables
would shorten each nested walk by a level, and on the host's backing would
take it from 30.9 to 20.5 ns: on 78 walks per request that is under 1 us,
against an excess of 109 us. PCID, which would let an arena's translations
survive the root switch, addresses a cost of about 12k cycles per request
that a single tenant does not pay at all. Pre-faulting addresses the
0.25 in-kernel faults, about 1 us. None of them is worth its memory.

### What a runner thread recovers

The thread that returns from `KVM_RUN` takes the flush. `GK_RUNNER=1` gives
the vCPU a runner, a host thread that does nothing but that ioctl: the
event-loop thread hands it each run and takes the result back, and
everything else -- the guest's registers, stack and TLS, the handling of
every exit -- stays as it was (see the runner section in `gk.c`). The flush
then lands on the runner's CPU, where it costs a few dozen handoff
instructions, and the event loop keeps its predictions across the turn. The
guest still enters cold, because its predictions were on the runner's CPU at
the previous return.

Where the runner sits decides what this is worth, and whether the two sides
spin through the handoff or sleep on a futex decides what it costs. Same
method as above, the counters now summed over both threads, with the
event-loop thread's own host-mode figures beside them; medians of three
rounds, and the spinning arrangement agrees to within 2 us with a separate
three-round run.

| hello                                   | p50       | rps, one connection | CPU per request   | logical CPUs busy | event loop's host cycles | its mispredicts | guest cycles | both threads' cycles |
| --------------------------------------- | --------- | ------------------- | ----------------- | ----------------- | ------------------------ | --------------- | ------------ | -------------------- |
| guest kernel off                        | 24.4 us   | 27,314              | 73.6 us           | 2.11              | 128k                     | 145             | 0            | 128k                 |
| guest kernel on                         | 138.9 us  | 6,814               | 118 to 144 us     | 1.0 to 1.3        | 252k                     | 3,128           | 217k         | 470k                 |
| runner on another core, both spinning   | 59.3 us   | 14,796              | 131 us, 2 CPUs    | 2.00              | 281k, mostly spin        | 94              | 196k         | 566k                 |
| runner on another core, both sleeping   | 68.0 us   | 13,013              | 82.8 us           | 1.00              | 63k                      | 125             | 204k         | 323k                 |
| runner unpinned, both sleeping          | 92.9 us   | 9,804               | 91 to 135 us      | 1.01              | 81k                      | 126             | 220k         | 359k                 |
| runner on the SMT sibling, spinning     | 137.6 us  | 6,716               | 214 us, 2 CPUs    | 2.00              | 457k, mostly spin        | 1,967           | 229k         | 919k                 |

| heavy                                   | p50       | rps, one connection | CPU per request   | logical CPUs busy | event loop's host cycles | its mispredicts | guest cycles | both threads' cycles |
| --------------------------------------- | --------- | ------------------- | ----------------- | ----------------- | ------------------------ | --------------- | ------------ | -------------------- |
| guest kernel off                        | 140.1 us  | 6,551               | 199.9 us          | 1.33              | 628k                     | 1,729           | 0            | 628k                 |
| guest kernel on                         | 271.0 us  | 3,509               | 295.9 us          | 1.18              | 260k                     | 3,228           | 800k         | 1,060k               |
| runner on another core, both spinning   | 174.6 us  | 5,460               | 366 us, 2 CPUs    | 2.00              | 780k, mostly spin        | 94              | 684k         | 1,570k               |
| runner on another core, both sleeping   | 207.2 us  | 4,553               | 226.7 us          | 1.00              | 123k                     | 125             | 732k         | 931k                 |

The event loop's host side is native again: 126k instructions in 63k cycles
(IPC 2.0, against 0.54) with 125 mispredicts (against 3,128, and 145 with
the guest kernel off). What it recovers, of hello's excess of 114.5 us of
latency over the guest kernel off:

- **70%** with the runner on another core and both sides spinning (59.3 us;
  2.2 times the requests per second on one connection) -- at the cost of a
  second logical CPU busy for the whole turn, so the CPU per request does
  not fall;
- **62%** with the runner on another core and both sides sleeping (68.0 us;
  1.9 times the rate), and **about 80% of the excess CPU per request**: 82.8
  us against 118 to 144 us, with 73.6 us the floor, on one logical CPU. Each
  turn costs two scheduler wakeups (1.99 context switches per request), which
  is the 9 us between this and the spinning arrangement;
- **40%** with the runner unpinned and sleeping (92.9 us): the scheduler
  usually keeps the two threads on different cores, but not always, and
  its wakeups are slower when it does not.
- **nothing** with the runner on the event loop's SMT sibling (137.6 us):
  the event loop keeps only part of its predictions (1,967 mispredicts),
  so the flush empties state the two hyperthreads of a core share, and the
  spinning sibling takes the core's front end from the guest besides.

For the heavier worker, 74% of the excess latency spinning and 49%
sleeping, and 72% of the excess CPU per request sleeping (226.7 us against
295.9 us, with 199.9 us the floor). The guest's share is larger there (its
2.5M instructions enter cold and run at IPC 3.1 against 4.1), and the runner
does not touch it.

What the sleeping runner leaves, per hello request, is 195k cycles over the
guest kernel off: the guest's cold entry (204k cycles for 120k instructions,
about 134k over the native rate, 31 us) and the runner's own return from the
run (57k host-mode cycles for 16k instructions: the kernel's exit path with
the IBPB, cold, and the futex handoff). Both are the one return to userspace
per turn, now on the runner's thread; the resident runner, next, removes
them.

The runner's memory is per vCPU, not per tenant: a 256 KiB stack mapping of
which a few pages are touched, a 64-byte record, and a kernel thread. The
marginal memory per tenant in section 1 is unaffected, and no page size or
mapping granularity changed.

### What a resident runner recovers

`GK_RUNNER=resident` keeps the runner inside `KVM_RUN` across turns, so the
vCPU's CPU sees no return to userspace on a turn's own path and no IBPB:
the guest keeps its predictions from one turn to the next, as the owner
keeps its own. The mechanism (the runner section in `gk.c`, and the README):
between turns the vCPU runs a ring-0 park loop of gk's own, reading a
per-vCPU frame in supervisor memory; the owner writes the next turn's entry
state (root, stack, function, argument, ring) into the frame and bumps a
sequence word, and the loop loads the root and `SYSRET`s into the turn. The
turn's exit sentinel reaches the vCPU's own `SYSCALL` entry, which puts the
result in the frame and echoes the word instead of leaving the VM. The loop
spins the same budget as the runner handoff (`GK_RUNNER_SPIN`) and then
halts with `STI;HLT`, which KVM services in the kernel (the VM gets a split
irqchip, so the LAPIC is in the kernel); the owner kicks a halted vCPU with
`KVM_SIGNAL_MSI` in NMI delivery mode, which wakes `HLT` whatever IF is
and needs no EOI; the handler returns where it landed (past the `HLT`
when it landed between the loop's last check and the `HLT`, which the
`STI` shadow does not cover for an NMI), so a kick that lands after the
loop already launched is harmless and none is lost. The owner spins the same
budget and then sleeps on the vCPU's eventfd, which the loop signals through
an `ioeventfd` (an `OUT` the kernel turns into the signal without leaving
`KVM_RUN`), and which the runner writes when a run does return.

The four questions this had to settle:

- **Whether the turn can end without a userspace exit.** It can, and does:
  on a hello request the vCPU's `KVM_RUN` returns 0.01 times per
  request by KVM's own `kvm_userspace_exit` tracepoint, against
  1.01 with the plain runner (the turn's end), and the exits
  that remain are the events the host must serve: a forwarded syscall, a
  hostcall, a demand fault, a fatal fault. Each returns to userspace once,
  is handled by the owner exactly as before, and the runner runs the vCPU
  again; the guest then re-enters cold for that turn. Hello makes none in
  the steady state; the gk_test suite, which faults on purpose, makes about
  one per turn through those (207k runs for 8.6k turns), which is the
  measure of what a turn that touches new pages or makes syscalls costs.
- **How registers and the result cross.** Through the park frame, a
  `gk_park` in the first page of the vCPU's IST region: supervisor memory
  that the host writes natively and ring 3 cannot touch, carrying in the
  five entry values and out the 64-bit result. `kvm_run` is read and written
  only once a run has returned, which is where every existing use of it
  sits (exit handling), and CR3, which cannot be read back from `kvm_run`
  while the vCPU is in flight, is tracked by the host as the root the loop
  was last told to load. The loop finds the frame through GS, set by
  `SWAPGS` from `KERNEL_GS_BASE`, which ring 3 can neither read nor write
  (CR4.FSGSBASE is clear).
- **Whether the ring-0 dispatcher can be made safe.** It reads the frame and
  nothing else: no value the turn can write reaches it, since ring 3 faults
  on the frame's page, and the five values it loads are the ones
  `enter_guest` puts into `kvm_run` in the other modes, trusted exactly as
  those are. The one thing a turn can do to it is end itself with any
  result, which it could already. Ring-0 guest code could rewrite the frame,
  and could also load CR3 directly, so the loop confers nothing ring 0 does
  not have. Every register but the five is cleared before the `SYSRET`, and
  the NMI handler writes nothing, so a kick that lands inside a turn (the
  two sides race, and either may win) costs the turn one return.
- **A faulting or abandoned turn.** The runner is always inside `KVM_RUN`,
  so every exit that reaches userspace is observed by it and relayed to the
  owner as before; nothing is unobserved. A fault that ends the turn
  returns through the port the handler reports on, `run_vcpu` returns
  `GK_EFAULT` with the vCPU left out of `KVM_RUN`, and the next entry on
  that thread starts it through `gk_park_entry`, which resets GS from a
  register, has the turn's root loaded through `kvm_run` first, and parks
  from a fresh state; `fault-test.sh` exercises that path. A vCPU that goes
  to the pool, or that a stub must run on, is first asked to leave the loop
  and waited for (`park_leave`), since a vCPU ioctl would otherwise wait
  behind the run. A parked vCPU keeps walking the last turn's root from
  its loop, so an arena's destruction first moves every parked vCPU still
  under that root to the base root and waits for the loop to say it has;
  the loop publishes its root in the frame for that, since the owner's own
  record runs ahead of the loop while it prepares an entry. A turn nobody
  waits for -- the owner thread gone -- runs to its end and parks, as it
  would run to its end today; there is no state in `kvm_run` to lose.

Same method as the runner table, interleaved with it; medians of three
rounds (`stall-resident.json`). The VMSCAPE mitigation read `Mitigation:
IBPB before exit to userspace` before and after, as the script records.
The build measured predates three corrections to the park loop that touch
nothing on a turn's path (the order of the launch's stores, the root a
parked loop walks, the NMI's window) and the choice of the runner's CPU
described under "Where the runner sits" below. On the final build the
one-connection hello latency is 24.5 to 24.8 us spinning and 27.8 to
28.9 us sleeping, launch after launch, against 23.1 to 23.5 us with the
guest kernel off (`haltpoll-*.json`). A client pinned to a CPU far from
the server's two adds about 10 us (37 to 40 us sleeping), which every
arrangement is subject to alike.

| hello                            | p50      | rps, one connection | CPU per request | logical CPUs busy | guest cycles | guest IPC | event loop's host cycles | its mispredicts | exits to userspace | exits, all | kicks | context switches |
| -------------------------------- | -------- | ------------------- | --------------- | ----------------- | ------------ | --------- | ------------------------ | --------------- | ------------------ | ---------- | ----- | ---------------- |
| guest kernel off                 | 24.1 us  | 32,844              | 30.0 us         | 1.05              | 0            | -         | 122k                     | 149             | 0                  | 0          | 0     | 0.00             |
| guest kernel on                  | 138.0 us | 6,817               | 122.9 us        | 1.01              | 209k         | 0.59      | 259k                     | 3,265           | 1.03               | 1.42       | 0     | 0.00             |
| runner on another core, sleeping | 91.5 us  | 9,930               | 104.5 us        | 1.00              | 226k         | 0.52      | 123k                     | 122             | 1.01               | 1.17       | 0     | 2.03             |
| resident, both spinning          | 25.4 us  | 31,055              | 62.4 us, 2 CPUs | 2.00              | 132k         | 0.98      | 130k                     | 94              | 0.01               | 0.11       | 0     | 0.00             |
| resident, both sleeping          | 28.5 us  | 28,045              | 52.5 us         | 1.44              | 79k          | 1.53      | 60k                      | 116             | 0.01               | 2.10       | 1.00  | 1.02             |

| hello, two tenants alternating | p50      | rps, one connection | CPU per request  | logical CPUs busy | guest cycles | guest IPC | guest page walks | exits to userspace | kicks |
| ------------------------------ | -------- | ------------------- | ---------------- | ----------------- | ------------ | --------- | ---------------- | ------------------ | ----- |
| guest kernel off               | 24.6 us  | 29,430              | 126.4 us         | 2.02              | 0            | -         | 0                | 0                  | 0     |
| guest kernel on                | 150.7 us | 6,135               | 187.4 us         | 1.11              | 238k         | 0.52      | 164              | 1.04               | 0     |
| resident, both spinning        | 26.5 us  | 28,929              | 124.8 us, 2 CPUs | 2.00              | 264k         | 0.90      | 199              | 0.02               | 0     |
| resident, both sleeping        | 30.3 us  | 25,944              | 91.2 us          | 1.46              | 130k         | 1.45      | 164              | 0.01               | 1.55  |

| heavy                            | p50      | rps, one connection | CPU per request  | logical CPUs busy | guest cycles | guest IPC | event loop's host cycles | its mispredicts | exits to userspace | kicks |
| -------------------------------- | -------- | ------------------- | ---------------- | ----------------- | ------------ | --------- | ------------------------ | --------------- | ------------------ | ----- |
| guest kernel off                 | 123.3 us | 7,316               | 184.9 us         | 1.35              | 0            | -         | 561k                     | 1,263           | 0                  | 0     |
| guest kernel on                  | 255.6 us | 3,694               | 275.7 us         | 1.18              | 717k         | 3.05      | 260k                     | 3,242           | 1.07               | 0     |
| runner on another core, sleeping | 185.6 us | 5,117               | 202.1 us         | 1.00              | 697k         | 3.03      | 61k                      | 131             | 1.04               | 0     |
| resident, both spinning          | 120.4 us | 7,785               | 253.2 us, 2 CPUs | 2.00              | 535k         | 4.03      | 539k, mostly spin        | 96              | 0.05               | 0     |
| resident, both sleeping          | 123.7 us | 7,564               | 147.9 us         | 1.12              | 486k         | 4.51      | 61k                      | 127             | 0.05               | 1.02  |

**The excess is gone.** Of hello's 113.9 us of excess latency over the
guest kernel off, the resident runner recovers **99%** with both sides
spinning (25.4 us, 1.3 us over the guest kernel off; 4.6 times the
requests per second of the guest kernel on and 95% of the rate with it
off) and **96%** with both sleeping (28.5 us, 4.4 us over; 4.1 times the
rate, 85% of the rate with it off). With two tenants alternating, so that
every turn loads another root, 98% and 96% (26.5 and 30.3 us against
24.6). For the heavier worker the turn is now no slower than native:
120.4 us spinning and 123.7 us sleeping against 123.3 us with the guest
kernel off, which is 102% and 100% of the 132.3 us excess, with the
guest's 2.1M instructions retiring at IPC 4.0 to 4.5 against 3.05 with the
guest kernel on and 4.05 for the same work natively.

The counters say why, and they are the ones the attribution above
predicted. The exits to userspace per request, by KVM's own
`kvm_userspace_exit` tracepoint, fall from 1.03 to 0.01 (0.05 for the
heavy worker: an occasional demand fault). The guest side is warm:
hello's guest instructions run at IPC 0.98 spinning and 1.53 sleeping
against 0.59, with 113 to 131 mispredicts against 1,682, and 87 to 145
instruction fills from beyond L2 against 6,349; the event loop's host
side is native, 60k to 130k cycles with 94 to 116 mispredicts against
the guest kernel off's 122k and 149. The sleeping arrangement's 2.10
exits of all kinds per request are the two the kernel services without
returning to gk, the HLT and the ioeventfd OUT; its one kick per turn is
the owner's `KVM_SIGNAL_MSI`, and its 1.02 context switches per request
are the owner's one sleep and wake (the runner handoff needed two). The
spinning arrangement takes 0.11 exits per request and no kicks.

What it costs. The spinning arrangement keeps the owner's CPU busy for
the whole turn and the vCPU's CPU busy between turns, so it is two
logical CPUs per event loop: 62.4 us of CPU per hello request against
30.0 us with the guest kernel off in this run. The sleeping arrangement
gives the owner's CPU back during the turn but not the vCPU's between
turns: KVM polls for up to 200 us (`halt_poll_ns`) before it lets a
halted vCPU's thread sleep, and under load the next turn always arrives
within that, so the vCPU's thread spends the gap polling in the kernel,
which is the 1.44 logical CPUs busy and the 52.5 us of CPU per hello
request. That is CPU spent waiting, not working; for the heavy worker,
whose turns are long against the gap, the sleeping arrangement costs
147.9 us of CPU per request against 184.9 us with the guest kernel off.
(The guest kernel off figure for hello's CPU per request is bimodal
between runs, 30 and 72 us, with V8's background threads idle or not;
the comparison within this run's rounds is the one to read.)

What remains, per hello request sleeping, is about 4 us: the kick (a
syscall), the HLT exit and the NMI delivery, the ioeventfd exit, and the
owner's wake, each a microsecond or so. The guest's share of the old
excess, 31 us of cold entry, is gone entirely: 79k cycles for 120k
instructions, against 204k. The resident vCPU costs two touched pages
more than a runner's (its park frame and the NMI stack, both inside the
IST region it already had), an eventfd, and a LAPIC in the kernel; the
marginal memory per tenant in section 1 is unaffected, and no page size
or mapping granularity changed. The library's own empty turn costs
2.75 us sleeping (against 6.77 us returning to userspace), and the exits
that still return -- a forwarded syscall, a hostcall, a demand fault, a
fatal fault -- cost what they did: the runner takes the IBPB on its
return and the guest re-enters cold for that turn, once per event.

### Where the runner sits

`GK_RUNNER_PIN=other` pins the owner to the CPU it is on when its vCPU is
created and the runner to the next CPU up on another core that shares the
owner's last-level cache. The cache is the condition that matters: the
owner builds the request on its CPU and the vCPU runs the turn on the
runner's, and the two hand the request's working set to each other through
it, the turn's input one way and its response the other. On the EPYC 9255
the last-level cache is a CCD's: four of them, CPUs 0-5 with 24-29, 6-11
with 30-35, 12-17 with 36-41 and 18-23 with 42-47 (sysfs
`cache/index3/shared_cpu_list`). With the owner pinned by `taskset` and the
runner by number (`haltpoll.py`; one connection, three launches each,
sleeping; `haltpoll-pin-*.json`):

| hello, resident sleeping              | p50, three launches | rps, one connection | CPU per request | vCPU thread's CPU per request | successful halt poll per request |
| ------------------------------------- | ------------------- | ------------------- | --------------- | ----------------------------- | -------------------------------- |
| owner 11, runner 12 (another CCD)     | 50.6, 50.2, 50.5 us | 16.8k to 16.9k      | 88 us           | 58 us                         | 27.4 us                          |
| owner 12, runner 11 (another CCD)     | 50.6, 50.6, 50.4 us | 16.8k to 16.9k      | 88 us           | 58 us                         | 26.7 us                          |
| owner 11, runner 10 (same CCD)        | 28.2, 28.1, 28.2 us | 28.8k to 29.0k      | 51 us           | 34 us                         | 14.7 us                          |
| owner 12, runner 13 (same CCD)        | 28.1, 27.9, 28.0 us | 28.5k to 29.4k      | 50 us           | 34 us                         | 14.4 us                          |
| owner 11, runner 35 (its SMT sibling) | 33.1, 33.1, 33.3 us | 25.1k to 25.3k      | 61 us           | 39 us                         | 19.4 us                          |

Across CCDs the same request costs 50 us against 28, and 37 us more CPU;
on the owner's SMT sibling 33 us, with the halt-polling runner on the
owner's own core. KVM's halt statistics for the VM (debugfs:
`halt_exits`, `halt_attempted_poll`, `halt_successful_poll`,
`halt_wakeup`, `halt_poll_success_ns`, `halt_poll_fail_ns`) say the halt
poll is not what differs. In every launch, slow or fast, each request
halts the vCPU once, the poll is attempted once and succeeds once, and
the vCPU's thread is never woken from a sleep (15 to 57 `halt_wakeup`
per round of 130k to 230k requests, the idle moments at the round's
edges; 4 to 12 ns of `halt_poll_fail_ns` per request): the next turn
always arrives inside the 200 us budget. What differs is how long the
successful poll lasts, 14.5 us per request on the same CCD against 27 us
across: that is the owner's time from the turn's end to the next launch
(the response, the client's round trip, the next request), lengthened by
the cross-CCD transfer of every line the turn wrote, and the turn is
lengthened the same way by the lines the owner wrote.

The next CPU up without the cache condition is on another CCD for 8 of
the 48 CPUs an owner may start on (5, 11, 17, 23 and their siblings 29,
35, 41, 47), so one launch in six is a slow one. Sleeping, one
connection, eight launches of two rounds each, p50 per round
(`haltpoll-before.json`, `haltpoll-after.json`):

- the next CPU up on another core, whatever its cache: 50.5 and 50.6 us
  (owner 11, runner 12), then 28.1, 28.1, 28.2, 28.1, 28.0, 28.1, 27.9,
  28.0, 28.2, 28.4, 27.9, 28.1, 28.1 and 28.3 us; 16.9k and 17.2k
  requests per second in the slow launch, 28.0k to 29.1k in the others;
- the next CPU up on another core in the owner's cache domain: 27.9,
  28.3, 28.3, 28.3, 28.4, 28.5, 28.2, 28.0, 27.8, 28.1, 28.2, 28.3, 28.4,
  28.9, 28.0 and 27.9 us; 27.4k to 29.2k requests per second. The owner
  that started on CPU 5 got its runner on 24 (the sibling of 0, in its
  domain) rather than 6. Forced onto CPU 11 by `taskset`, three launches
  measure 28.4, 28.2 and 28.6 us with the runner on 30; onto CPU 5, 28.3
  and 28.3 us with the runner on 24 (`haltpoll-after-cpu11.json`,
  `haltpoll-after-cpu5.json`).

So the sleeping arrangement's hello figure is 28 us, 27.8 to 28.9 us
over sixteen rounds, and the spinning arrangement's 24.5 to 24.8 us
(three launches, `haltpoll-spin.json`), against 23.1 to 23.5 us with the
guest kernel off (`haltpoll-off.json`). The CPU per request, same runs:
sleeping, 50 to 53 us on 1.37 logical CPUs, of which the vCPU's thread is
33.5 to 35.5 us -- the whole of the request's 34 to 36 us period, since
between turns it polls in the kernel (the 14.5 us of successful poll per
request) and during the turn it runs the guest -- and the owner's thread
with V8's the remaining 16 to 18 us; spinning, 63 to 64 us on 1.94 CPUs;
guest kernel off, 32 us on 1.00 CPU (76 us on 2.07 in one launch of
three, with V8's background threads awake). The VMSCAPE mitigation read
`Mitigation: IBPB before exit to userspace` and the host's `halt_poll_ns`
200000 before and after every run, as the script records. These three
figures were taken again after the per-turn work the resolve table now
does; see "Re-measured after the per-turn work" below.

### Two tenants alternating

Two tenants in one process and a client whose requests alternate between
them, one in flight at a time, so that every turn runs under a different
arena root than the turn before and loads a new CR3 (`httpbench` with a
second port; `stall-tenants.json`). The counter window keeps the one
connection, so the event-loop thread idles between requests and its
host-mode cycles include the wake-up path and are not comparable to the
single-tenant window above; the guest-mode figures and the latencies are.

| hello                                 | p50      | p50, one tenant | guest cycles | guest page walks | root switches |
| ------------------------------------- | -------- | --------------- | ------------ | ---------------- | ------------- |
| guest kernel off                      | 24.4 us  | 24.4 us         | 0            | 0                | 0             |
| guest kernel on                       | 153.9 us | 138.9 us        | 234k         | 172              | 1.11          |
| runner on another core, both spinning | 61.1 us  | 59.3 us         | 202k         | 108              | 1.00          |

| heavy            | p50      | p50, one tenant | guest cycles | guest page walks | root switches |
| ---------------- | -------- | --------------- | ------------ | ---------------- | ------------- |
| guest kernel off | 142.3 us | 140.1 us        | 0            | 0                | 0             |
| guest kernel on  | 290.9 us | 271.0 us        | 811k         | 214              | 1.01          |

The switch costs the guest side 95 more page walks per hello request (172
against 77), the arena's translations having been dropped with the root:
about 12k cycles, 3 us, which is what PCID could save and 3.6% of the
single-tenant excess. The request costs 15 us more than with one tenant,
though, and almost all of it is on the host side, where the second tenant's
connection and isolate add code and data that the cold front end then
misses on as well; with the guest kernel off the second tenant costs
nothing, and with the runner it costs 1.8 us. With the runner the guest
also walks less after a switch (108 against 172): its translations live in
the runner's core's TLB, where the host side's never displace them.

### What it costs per CPU when the machine is packed

Section 2's throughput sweep re-run with the resident runner, so that the
crossover below rests on one basis: sixteen tenants kept busy on two
connections each, three rounds, the optimized build, the VMSCAPE mitigation
on (`throughput.py`; `results/cross-*.json`). The guest kernel off and the
plain guest kernel rows reproduce section 2 to within 6% and 3%, which is
the check that the two sweeps are comparable.

| arrangement                                    | total rps | logical CPUs busy | rps per logical CPU | range         |
| ---------------------------------------------- | --------- | ----------------- | ------------------- | ------------- |
| one process, guest kernel off                   | 31,300    | 1.63              | 19,257              | 16,202-20,132 |
| one process, guest kernel on, every turn exits  | 7,824     | 1.11              | 6,918               | 6,912-7,194   |
| one process, resident, runner in the cache      | 25,905    | 1.38              | 18,784              | 18,399-19,239 |
| one process, resident, runner on the sibling    | 19,705    | 1.49              | 13,262              | 13,113-14,257 |
| one process, resident, runner unpinned          | 14,672    | 1.74              | 8,473               | 7,846-9,101   |
| one process, resident, both sides spinning      | 29,599    | 2.00              | 14,813              | 13,831-15,082 |
| process per tenant                              | 409,571   | 22.18             | 18,469              | 17,084-19,949 |
| process per tenant, resident, in the cache      | 166,230   | 17.46             | 9,361               | 7,063-10,291  |
| process per tenant, resident, unpinned          | 278,129   | 27.47             | 10,123              | 9,910-11,295  |
| process per tenant, resident, on the sibling    | 348,771   | 23.89             | 14,592              | 13,988-14,928 |

**The factor of 3.4 is gone, in one process.** A single process with the
resident runner in the owner's cache serves 18,784 requests per logical CPU
against the process model's 18,469, and against 19,257 with no isolation at
all: parity on both, within a spread the three rounds do not separate. What
section 2 measured was the cost of leaving KVM_RUN, not of the guest kernel.

**It does not survive packing the machine.** Sixteen guest-kernel processes
on 48 logical CPUs serve 9,361 per CPU with the same placement. A resident
vCPU needs a second logical CPU -- between turns it halt-polls there, which
is charged to the server as system time -- and when every event loop wants
one the machine runs out. The placement is also chosen per process with
nothing coordinating it, so owners and runners land on the same CPUs and,
being pinned, cannot migrate off them.

**Packed, the sibling is the right place.** An owner's SMT sibling follows
from the owner's own core, so sixteen pairs occupy sixteen cores with no
coordination and no collisions, and the figure recovers to 14,592 per CPU.
The best placement therefore inverts with occupancy: in the owner's cache on
a quiet machine (18,784 against the sibling's 13,262), on the sibling when
the machine is full (14,592 against 9,361). Neither default is right for
both. `GK_RUNNER_PIN` is left at the quiet machine's choice, and deriving it
from the machine's occupancy is open.

### Re-measured after the per-turn work

Everything above was measured before the per-turn work a resolve entry now
carries: the owner compared against CR3 in the ring-3 trampoline, the type
word returned beside the pointer, the per-arena entry list maintained at
register and retire, the owner checked on every handle a turn hands the
host, a context entry disowned and adopted around a service-binding
subrequest, the journal's spent positions settled on a refill, and the
isolate's lock depth saved and restored per turn. Those paths cost a ring-3
resolve 9 ns more than they did when the figures above were taken, so a
request that resolves a few tens of handles should lose a fraction of a
microsecond. That is what the repeat measures: nothing moved by as much as
half a microsecond per request, and nothing in the comparison changed
hands.

The one-connection hello latency, under the conditions of the runs above --
the optimized build, the VMSCAPE mitigation reading `Mitigation: IBPB
before exit to userspace` and the host's `halt_poll_ns` 200000 before and
after each run (the script records both), an otherwise idle machine, and
the runner in the owner's last-level cache in every launch
(`haltpoll-remeasure-*.json`):

| hello, one connection, p50        | before                   | after                    |
| --------------------------------- | ------------------------ | ------------------------ |
| resident sleeping, sixteen rounds | 27.8-28.9 us             | 28.1-28.6 us             |
| resident spinning                 | 24.5-24.8 us, 3 launches | 24.9-26.0 us, 8 launches |
| guest kernel off                  | 23.1-23.5 us, 3 launches | 23.1-23.6 us, 3 launches |

The sleeping arrangement and the guest kernel off are where they were; the
spinning arrangement is 0.4 us slower, and it is the only movement these
measurements resolve. The counters behind them are unchanged: sleeping
costs 50.9 to 52.5 us of CPU per request on 1.37 to 1.38 logical CPUs, of
which the vCPU's thread is 33.9 to 35.2 us and the successful halt poll
14.5 to 15.0 us, against 50 to 53 us on 1.37, 33.5 to 35.5 and 14.5 before;
spinning costs 64.6 to 67.2 us on 1.94 CPUs against 63 to 64 on 1.94. Every
request still halts the vCPU once, polls once, succeeds once, and never
wakes the vCPU's thread from a sleep.

The crossover re-run on the same basis -- sixteen tenants, two connections
each, the optimized build, the mitigation on -- over more rounds than
before, because the per-CPU figure is a ratio of two quantities that each
move several percent from round to round (`recross-*.json`):

| arrangement                                  | rps per logical CPU, before | after, median | after, range  | rounds |
| -------------------------------------------- | --------------------------- | ------------- | ------------- | ------ |
| one process, guest kernel off                | 19,257                      | 18,042        | 15,242-20,372 | 12     |
| one process, resident, runner in the cache   | 18,784                      | 18,462        | 17,065-19,697 | 12     |
| process per tenant                           | 18,469                      | 18,696        | 15,517-20,681 | 9      |
| process per tenant, resident, on the sibling | 14,592                      | 13,683        | 10,018-14,499 | 9      |

Three of the four medians land inside the spread of the three rounds each
earlier figure was the median of, and the aggregate rate behind the ratio
moves less than the ratio does: 30,316 requests per second against 31,300
with the guest kernel off, 25,981 against 25,905 for the resident runner in
one process, 413,818 against 409,571 for a process per tenant, and 310,633
against 348,771 for a process per tenant on the sibling. What the table
says is unchanged: one process with the resident runner is at parity with a
process per tenant on a quiet machine (18,462 against 18,696), and the
sibling is where the guest kernel's processes belong when the machine is
packed.

The fourth, the sibling arrangement, is 2% under the bottom of its earlier
range, and two of its nine rounds fell to the collision this section
already describes -- 10,018 and 10,125 per CPU on 17.5 busy CPUs, the
median tenant's p50 at 240 us rather than 72 -- which is the placement
lottery of sixteen independently pinned pairs and not something a turn now
does. Over the seven rounds that placed cleanly the median tenant's p50 is
72.4 us against 71.2 us before, while in the same session a process per
tenant with the guest kernel off gives 51.7 us against 51.2 us before. So
about 0.7 us of that 1.2 us is the guest kernel's and the rest is the
session's.

The library's own per-operation costs, from `gk_test` on an idle CPU, three
runs agreeing to the last digit: a ring-3 resolve 27.8 ns, a register 74 to
75 ns on the base root and 62 ns inside an arena, a retire 13 and 15 ns,
and the disown and adopt that move a context's entry between arenas 35 ns
each. The per-arena list still costs the register about a nanosecond inside
an arena, as it did when it was added.

The handler allocation guard costs a shipping build nothing. Without
`GK_GUARD_HANDLER_ALLOC` its bracket compiles to `0` and `(void)0` and
malloc is not wrapped, which is the build everything here was measured on.

## The answer against the criterion

**By the letter of the criterion, it does not fire.** A process per isolate
does not come within 20% of the guest kernel on marginal memory. It costs 1.8
to 3.4 times as much per tenant, 2.6 to 3.7 times on a system basis. Memory is
the one axis the guest kernel wins, and it wins it clearly.

**The belief the criterion rested on is wrong.** The criterion assumed the
guest kernel's dispatch and cold-start advantages were real and asked only
whether memory justified the complexity. On this machine:

- dispatch: the guest kernel is **3.3 times worse** than a process crossing,
  not better. The specific claim that motivates the architecture -- routing
  between tenants as a page-table load instead of an IPC -- is contradicted.
- cold start: the guest kernel is 1.5 times better than an unoptimized process
  spawn, and would be behind a pre-forked pool.
- requests per second: with every turn returning to userspace the guest
  kernel cost **3.4 times** the CPU per request. With a resident runner it is
  at parity in one process and costs 1.27 times when the machine is packed
  (see above). The 59 times aggregate rate when confined to one process
  stands: one process cannot use the machine.
- it has a hard ceiling of 183 isolates per process; the process model has
  none.

So the guest kernel buys a factor of 1.8 to 3.4 in memory with a factor of 3.4
in CPU. Which of those binds decides the answer, and the crossover is
computable. Taking 350 GB usable and 48 logical CPUs, a process per tenant
holds 38,975 tenants and serves 1.08 million requests per second; the guest
kernel holds 123,000 tenants and serves 321,000. The guest kernel therefore
holds more tenants only while the average tenant serves fewer than about
**8 requests per second**. Below that it holds up to 3.2 times more; above it
the process model holds up to 3.4 times more.

**The decision this supports.** The dispatch, CPU and crossover figures
above were taken with every turn returning to userspace, and section 4
shows that the cost they measure is the branch-predictor flush the
kernel's VMSCAPE mitigation puts on that return, not the guest kernel's
own mechanism: with a resident runner, which never returns, hello costs
25.4 us against 24.1 with the guest kernel off (28.5 us with both sides
sleeping), the heavier worker runs at native speed, and the per-request
tax that made a crossing 3.3 times a process crossing is down to a few
microseconds. The memory advantage stands at a factor of 1.8 to 3.4. What
the resident runner costs is CPU spent waiting rather than working, 2
logical CPUs per event loop spinning or a vCPU thread that halt-polls
between turns sleeping.

Recomputing the crossover on the basis used above, with the packed
machine's 14,592 requests per logical CPU: 48 logical CPUs serve 700,000
requests per second under the guest kernel against 887,000 with a process
per tenant, and the tenant counts are unchanged at 123,000 and 38,975. The
guest kernel therefore holds more tenants while the average tenant serves
fewer than about **18 requests per second**, where section 2's figures put
that at 8. Below about 6 it is memory-bound and holds 3.2 times more; above
18 the process model holds more, but by at most **1.27 times** rather than
3.4. The dispatch argument the design is built around is not contradicted
after all, once the turn stays inside KVM_RUN, and the band in which the
guest kernel is the denser arrangement is twice as wide as section 2 made
it. What remains open is the realistic worker mix, which decides both axes,
and whether a packed machine can be given a better placement than the SMT
sibling.

## Caveats, and what each would change

- **The worker is trivial.** A hello handler minimizes per-isolate heap, which
  understates both models' memory, and minimizes per-request work, which
  overstates the guest kernel's relative CPU tax. The heavier worker here moves
  the tax from 1.94 to 1.75 times total CPU, so a realistic worker would shift
  the crossover up. It would also raise per-tenant memory in both models, which
  shrinks the memory ratio and shifts the crossover down. **The direction of
  the net effect is not established by these measurements**, and a realistic
  worker mix is the single biggest open question in the comparison.
- **The pre-forked floor is a proxy.** KSM brackets it rather than measuring
  it, and errs optimistic: it merges pages a fork would not have kept shared.
  The true pre-forked figure is somewhere between 4.74 MB (PSS, KSM) and 8.98
  MB (PSS, independent), probably nearer the lower end plus the 1.4 MB per
  process of kernel-side cost the system-wide fit shows, so about 6 MB. At 6 MB
  the process model is 2.1 times the guest kernel, still far outside 20%.
- **The client shares the machine.** Absolute throughput ceilings are
  underestimates for every arrangement, and the comparisons are unaffected
  while the server uses two logical CPUs of 48, because the client load is
  proportional to the work served. The packed rows are the exception:
  sixteen server processes and the client together oversubscribe the
  machine, so those per-CPU figures are a lower bound, and the guest
  kernel, which wants a second logical CPU per event loop, is squeezed
  harder by the client than the process model is. **The packed comparison
  would have to be repeated with the client on another box to be tight**,
  and the 18 requests per second crossover with it.
- **Per-tenant latency under the guest kernel at saturation is not reported.**
  Some tenants were starved for the whole measurement window, so the per-tenant
  p50 in the guest-kernel throughput rows is not meaningful. Whether the single
  event loop is unfair between tenants under load is unmeasured and worth
  knowing on its own.
- **Three sweeps, one machine, one kernel.** The memory fits are
  near-deterministic; the dispatch figures reproduce across runs to within 2%;
  the CPU figures reproduce to within 5% except the guest-kernel hello case,
  which spread 135-165 us.

## What would sharpen it

1. **Take the remaining cost out of the sleeping arrangement.** The
   resident runner leaves hello at 28.5 us against 24.1, but at 52.5 us of
   CPU per request against 30.0, and the difference is the vCPU's thread
   polling in the kernel between turns for a wake that always comes within
   KVM's 200 us halt-poll budget: 14.5 us of successful poll per hello
   request at one connection, the owner's time between turns, and never
   a wakeup from sleep (see "Where the runner sits"). `KVM_CAP_HALT_POLL`
   sets that budget per VM, from the process, so the trade between the
   poll's CPU and a scheduler wakeup's latency (the runner handoff's
   9 us) can be made per deployment without touching the host; a budget
   below the gap makes every wake a scheduler wakeup. It was not measured
   here. The
   exits that still return to userspace -- a forwarded syscall, a hostcall,
   a demand fault -- each cost the turn a cold re-entry as before, so a
   worker that makes syscalls inside its turn pays per syscall; hello makes
   0.01 per request, and a realistic mix is the open question it was. The
   IBPB itself has not left the machine: when a halted vCPU's thread does
   schedule out, the next task to return to userspace on that CPU takes it
   once, a bounded cost to a bystander. And a vCPU with an id above 254
   needs the LAPIC in x2APIC mode to be kicked, which the code does not
   do yet.
2. **Build the pre-forked process model.** Fork a warmed workerd per tenant
   after V8 is initialized and before the tenant's script is known. It settles
   the memory comparison within 20% instead of within a bracket, and it is
   small next to the work the guest kernel still needs.
3. **Measure a realistic tenant mix.** Per-isolate heap and per-request CPU for
   workers that resemble the ones that would be hosted. Both numbers move the
   crossover and they move it in opposite directions.
4. **Measure fairness.** Whether one event loop can serve 183 tenants without
   starving some of them, under the guest kernel and without it.

## Earlier figures, re-run here

| figure                                      | reported earlier | measured here | note                                                                                                             |
| ------------------------------------------- | ---------------- | ------------- | ---------------------------------------------------------------------------------------------------------------- |
| marginal memory penalty of the guest kernel | +63%             | +63%          | reproduces                                                                                                       |
| tenants per GB penalty                      | -39%             | -38%          | reproduces                                                                                                       |
| hostcall (one VM exit)                      | 5.9 us           | 5.95 us       | reproduces                                                                                                       |
| in-guest ring-0 trap (resolve)              | 18.7 ns          | 20.3 ns       | reproduces                                                                                                       |
| signal-field write                          | about 60 ns      | 62 ns         | reproduces                                                                                                       |
| hello-world p50 under the guest kernel      | about 610 us     | 132 us        | see below                                                                                                        |
| port-I/O VM exit round trip                 | 4.75 us          | not re-run    | the standalone spike is not in this tree; the hostcall round trip is the same kind of measurement and reproduces |

The empty guest turn costs 6.77 us here, from the library's own timing.

The hello-world figure needs explaining, because it is the one that did not
reproduce and the reason matters. Run against a build without optimization, the
same measurement gives 650 us under the guest kernel against 413 us without it
-- which is where "about 610 us, roughly unchanged" comes from, and it puts the
guest kernel's tax at +57%. Against an optimized build the same two numbers are
132 us and 23.5 us, a tax of +462%. The absolute overhead is in fact lower on
the optimized build, 109 us against 237 us; what changes is everything it is
measured against. **Any performance figure for the guest kernel taken on an
unoptimized build understates its relative cost by roughly a factor of four**,
and the figures that have been guiding this work were taken that way.

## What the body relay costs

### The question

A request or response body crosses an in-process service binding through a `kj`
one-way pipe whose two ends are held by different isolates, and the pipe copies
between the writer's buffer and the reader's in whichever of the two operations
arrives second. Under per-isolate heap arenas that copy runs in one isolate's
turn over a buffer in the other isolate's arena, which the turn cannot address,
so `ReadRelay` (`io/worker-interface.c++`) stands at the reading end and makes
every operation on the pipe from the event loop: a read yields to the event
loop, reads from the pipe into a 64 KiB staging buffer the host allocates, and
copies into the reader's buffer from there. The cost was stated as one more copy
and one event-loop turn per body chunk, plus the loss of the synchronous read
fast path, and never measured. Bodies are a hot path, so this is the number.

### Why the obvious A/B is the wrong experiment

`bodiesRelayed()` answers `jsg::heapArenasAvailable()`, so the one configuration
that needs the relay is the one in which the bare pipe faults: the negative
build `--copt=-DWORKERD_GK_NEG_BODY_RELAY` condemns the reading isolate, which
makes it a check that the correctness test tests something and not a performance
baseline. Comparing the relay against a build with no arenas at all would price
the relay together with jemalloc and per-isolate placement, which is three
changes at once.

So the relay is varied on its own, in configurations where heap arenas are off
and the bare pipe is therefore safe, through a temporary knob in
`bodiesRelayed()` that reads the environment:

    static const bool forced = ::getenv("WORKERD_GK_FORCE_BODY_RELAY") != nullptr;
    return forced || jsg::heapArenasAvailable();

It is not committed, because it is a configuration nobody should ship. An
environment variable rather than a `--copt` so that **one binary serves both
arms** and no build difference can enter the delta. The knob logs which arm it
is in on its first call, and since `bodiesRelayed()`'s only two callers are the
two install sites in `asHttpClient()`, that log is the whole of the control;
every arm below was confirmed by it (`f = false`/`true`,
`heapArenasAvailable() = false`/`true`).

### Method

Three configurations, all `-c opt`, the VMSCAPE mitigation reading `Mitigation:
IBPB before exit to userspace` and the host's `halt_poll_ns` 200000 before and
after every run (`bodyrelay.py` records both):

| arm | binary | guest kernel | heap arenas | relay |
| --- | ------ | ------------ | ----------- | ----- |
| **N** off / **N** on | jemalloc (`--//src/workerd/server:allocator=jemalloc`), thread cache on | off | off | off / forced on |
| **G** off / **G** on | plain (no jemalloc, so arenas cannot exist) | on, `GK_RUNNER=resident GK_RUNNER_PIN=other`, sleeping | off | off / forced on |
| **A** | jemalloc, thread cache off | on, resident, sleeping | **on** | on, by `heapArenasAvailable()` |

N and G are each a paired comparison: the two arms are interleaved launch by
launch, so nothing the machine does over the hour lands on one arm rather than
the other. A has no second arm, because turning the relay off there is the
configuration that faults; what A measures is what a deployment pays, and the
relay's share of it is taken from N and G.

A front worker fetches a back worker through a service binding and keeps all the
size-dependent work inside that binding, answering the client with a byte count,
so the client's traffic is the same at every body size and in both directions.
The body is a cached `Uint8Array`, so no size-dependent encoding enters either
arm. The routes: `/down/<n>`, the back worker answers with n bytes which the
front reads with `arrayBuffer()`; `/up/<n>`, the front sends n bytes which the
back reads with `arrayBuffer()`; `/upthru/<n>`, the front sends n bytes, the back
returns them unread as its response body and the front reads that, so both
crossings are `kj` pumps rather than reads -- this is the path whose synchronous
fast path the relay gives up, since `pumpTo()`'s loop in
`api/streams/internal.c++` calls `tryReadSync()` unconditionally and the pipe
answers it whenever a writer is blocked with enough buffered;
`/downread/<n>`, read chunk by chunk through a JS reader; and
`/upchunk/<n>/<c>`, n bytes sent as n/c chunks from a JS stream, which is what
tells a per-chunk cost from a per-request one. One connection for latency, eight
for throughput; `httpbench`; the server tasksetted to one CCD (`12-17,36-41`) so
that the owner and its runner always share a last-level cache, and the client to
another (`0-5,24-29`). Six launches per arm in the focused run below and three in
the full sweep (`bodyrelay.py`; `results/relay-focus-*.json`,
`results/relay-{native,gk,arena}.json`).

### 1. What the relay costs, by itself

Six launches per arm, one connection, p50 per launch; the delta column is the
paired difference taken launch by launch, with its spread over the six.

| one connection, p50       | N off  | N on   | N delta, paired   | G off  | G on   | G delta, paired  |
| ------------------------- | ------ | ------ | ----------------- | ------ | ------ | ---------------- |
| response body, 0 B        | 75.7   | 77.2   | +1.4 (-0.2, +2.4) | 87.3   | 87.7   | +0.5 (-0.6, +1.8) |
| response body, 64 KiB     | 119.0  | 122.3  | +3.5 (+1.3, +4.8) | 166.5  | 170.1  | +4.1 (+0.8, +6.9) |
| response body, 4 MiB      | 1609.9 | 1642.8 | +32 (+9, +74)     | 4602.8 | 4664.1 | +138 (-21, +351)  |
| request body, 0 B         | 84.9   | 85.9   | +0.7 (+0.2, +3.3) | 91.3   | 92.1   | +0.5 (+0.1, +2.0) |
| request body, 64 KiB      | 126.1  | 131.2  | +4.1 (+2.6, +7.1) | 145.7  | 147.9  | +2.4 (+0.7, +2.8) |
| request body, 4 MiB       | 1185.9 | 1220.5 | +39 (see caveat)  | 1717.2 | 1763.5 | +42 (+37, +54)    |
| pumped both ways, 0 B     | 78.5   | 79.4   | +1.2 (-1.6, +2.9) | 87.9   | 88.8   | +0.6 (-0.4, +1.9) |
| pumped both ways, 64 KiB  | 117.6  | 123.1  | +5.8 (+4.7, +8.2) | 134.7  | 138.5  | +3.9 (+2.5, +5.4) |
| pumped both ways, 4 MiB   | 1133.6 | 1260.1 | +112 (+80, +170)  | 1668.0 | 1748.6 | +84 (+69, +98)    |

**About a microsecond per crossing, plus eight to fourteen microseconds per
mebibyte.** A crossing that carries no body at all costs 0.5 to 1.4 us, and the
spread of the six launches straddles zero in four of the six bodiless cells, so
one microsecond is the resolution rather than the figure. A 64 KiB body costs 2
to 6 us. A 4 MiB body costs, per relayed crossing, 42 us in the cleanest cell (G
request, 37 to 54 over six launches) and 42 us again in the second cleanest (G
pumped, 84 us over two relayed crossings, 69 to 98), which is 10.5 us per MiB;
the N cells put the same quantity at 8 us per MiB (response) and 14 (pumped).
Call it 8 to 14 us per MiB per crossing and do not read the ends of that range as
a difference between the paths.

**The cost is per byte, not per chunk, and the commit message's "per body
chunk" is wrong.** The decisive cell is a fixed 1 MiB body delivered as a
varying number of JS stream chunks (full sweep, N, one connection, three
launches): 1024 chunks of 1 KiB costs +33 us, 128 chunks of 8 KiB +35, 16 chunks
of 64 KiB +19 to +61 depending on the estimator, and 4 chunks of 256 KiB +113.
Two hundred and fifty-six times as many chunks does not cost more; it costs
slightly less. The reason is in the code: the relay's unit is a `tryRead` call,
not a body chunk, and workerd's readers ask for 128 KiB to 512 KiB at a time
(`AllReader::read`'s `amountToRead` in `api/streams/internal.c++`), so the number
of relay calls is set by the reader's buffer and the 64 KiB staging cap rather
than by how the writer chunked the body. The caveat on that cell is that the four
chunk sizes have wildly different baselines (6329 us at 1 KiB against 491 us at
256 KiB, the JS stream machinery), so these are deltas against different
quantities; the direction is what the cell establishes, and the direction is
unambiguous.

**There is no step at the 64 KiB staging buffer.** 64 KiB, one staging buffer's
worth, costs 3.5 us; 256 KiB, four buffers' worth, costs 6.6 (full sweep, N) --
not four times as much. Nor is there a knee anywhere between 0 and 4 MiB.

**The extra event-loop turn is not where the cost is.** This is what the G arm
is for: under the guest kernel every turn boundary is real work, and the relay's
`co_await kj::yield()` is exactly one more of them per read. G's deltas are the
same as N's -- 0.5 against 1.4 us bodiless, 2.4 against 4.1 at 64 KiB, 42
against 39 at 4 MiB on the request path -- so the yield costs nothing this
measurement can see, in the one configuration where it should have been
expensive. What the relay costs is the second copy. That it costs only 8 to 14
us per MiB rather than the 40 to 80 a second pass over the body would suggest is
the staging buffer's doing: at 64 KiB it stays in cache, so the DRAM traffic is
unchanged and only the cache-resident copy work is added.

**The synchronous read fast path is not measurably lost either.** The `upthru`
route is the pump path, where `tryReadSync()` is called unconditionally and the
relay always declines it; per relayed crossing it costs 42 us per 4 MiB under G,
which is the same as the request path's 42. So the fast path's loss is inside the
noise -- plausibly because a pipe answers `tryReadSync()` only when a writer
happens to be blocked with at least `minBytes` buffered at the instant of the
read, which is not the common case for a body being produced.

**Throughput.** At eight connections (full sweep, three launches) the rate falls
by 0 to 9% on bodies of 64 KiB and up and is unchanged on empty bodies
(13,859 against 13,827 requests per second). The consistent cells are the N
request path at 64 and 256 KiB, -8.2% and -8.7%; elsewhere the sign goes both
ways between launches and under the guest kernel one cell reads +18%. Only the
direction and the order of magnitude are reliable.

### 2. What a deployment pays, and how little of it is the relay

The same three rows, one connection, p50 per launch, six launches (`relay-focus-*.json`):

| one connection, p50     | N: no guest kernel, no arenas, no relay | G: guest kernel, no arenas, no relay | A: guest kernel, arenas, relay |
| ----------------------- | --------------------------------------- | ------------------------------------ | ------------------------------ |
| response body, 0 B      | 75.5-76.4                               | 86.7-87.7                            | 109.8-134.4                    |
| response body, 64 KiB   | 117.4-120.8                             | 163.3-166.6                          | 161.9-238.7                    |
| response body, 4 MiB    | 1599-1621                               | 4208-4657                            | 2868-5559                      |
| request body, 4 MiB     | 1163-2161                               | 1712-1726                            | 1781-4304                      |
| pumped both ways, 4 MiB | 1117-1151                               | 1658-1672                            | 1827-4349                      |

**The relay is exonerated.** A bodiless crossing costs 76 us with none of this
machinery and 110 to 134 us with all of it; a 4 MiB response body costs 1610 us
and 2868 to 5559. Against gaps of 33 to 59 us and 1258 to 3950 us, the relay's
own contribution is 1.4 us and 32 us: **about 1 to 4% of what the body path
costs a deployment, and about 1% at 4 MiB.** Whatever is expensive about a body
crossing an in-process binding under the guest kernel with heap arenas, the
relay is not it. The relay's share is carried over from the N and G arms, which
is sound to the extent that its cost is configuration-independent -- and N and G
agree on it to within the measurement, which is the evidence for that.

**But A moves three things at once and only the relay's share is isolated.**
A differs from N in the guest kernel, in per-isolate placement, in jemalloc with
its thread cache off, and in the relay; G sits between them but reaches it by a
different route, since the plain binary is also a different allocator
(tcmalloc). So the ladder orders the configurations and does not apportion them.
The bodiless column is the one place an apportionment is tempting -- 76 to 87 us
is +11 us for the guest kernel and 87 to 110-134 is the rest for arenas plus the
allocator -- and even there the +11 carries the allocator change with it.

**And A is bimodal between launches by a factor of up to 1.8, which no
apportionment survives.** Two of the six A launches landed in a fast mode and
four in a slow one, consistently across all nine routes at once: 110-113 against
132-134 us bodiless, 2868-2869 against 5089-5559 us on a 4 MiB response,
1781-1790 against 4272-4304 on a 4 MiB request. In the fast mode A's 4 MiB
response is *faster* than G's, which is to say faster with per-isolate arenas
than without them -- so the claim that per-isolate placement carries the large
per-byte cost is **not** established by these numbers, and neither is its
opposite. The earlier full sweep's three A launches all happened to land in the
slow mode (133.0, 133.3, 133.1 us bodiless; 5321, 5391, 5497 us at 4 MiB), which
is why they looked tight at 3.4 times N; read as a 3.4 times figure that is one
mode of a two-mode distribution. N and G are not bimodal in the same way, so
this is a property of the jemalloc-with-arenas arm and its cause is not known
here. What the per-byte cost is attributable to is therefore open, with one
observation that bears on it: the guest-kernel arm G has arenas **off**, and a
4 MiB response body already costs 2.6 to 2.9 times N there, so whatever that
cost is it is not specific to per-isolate placement.

**What would separate the allocator from placement.** Four cells, in order of
cost:

1. The plain binary with the guest kernel **off**. It needs no knob at all, and
   it calibrates the allocator step that currently moves with the guest kernel
   between N and G. It was not run here, and it should have been.
2. The jemalloc binary with the guest kernel on and arenas suppressed, which
   holds the allocator fixed across the guest-kernel step. `heapArenasAvailable()`
   is false exactly when jemalloc's thread cache is on, and
   `checkHeapArenaSupport()` makes that combination fatal under the guest kernel,
   so this cell needs a temporary knob demoting that `KJ_REQUIRE` to a warning.
3. The jemalloc binary with the thread cache off but no per-isolate arena
   created, which separates "jemalloc without its thread cache" -- every
   allocation taking an arena bin lock -- from placement itself. That needs a
   knob that leaves the per-isolate `HeapArena` uncreated while the thread
   cache stays off.
4. All of them over at least eight launches, reported per launch rather than as
   a median, since two launches in six moved everything here by 1.8 times and a
   three-launch median hid it completely.

### Caveats

- **The relay's cost is measured where the relay is not needed.** Both paired
  arms have heap arenas off, which is the point -- it is the only way to run the
  bare pipe safely -- but it means the relay was never priced in the arena
  configuration it exists for. The N and G arms agreeing is the argument that it
  transfers; it is an argument, not a measurement.
- **The 4 MiB cells are the loose ones.** N's response body at 4 MiB spreads its
  paired delta over +9 to +74 us and G's over -21 to +351; the figure quoted for
  4 MiB rests on the two cells whose six launches agreed (G request, +37 to +54;
  G pumped, +69 to +98). N's **request** body at 4 MiB is worse than loose: its
  baseline is bimodal between 1163 and 2161 us within one arm, and one launch's
  paired delta came out at -945 us, so that cell says nothing and is quoted only
  for its median.
- **One 64 KiB cell was unstable in the three-launch sweep**: the response body's
  relay-off arm spread 175 to 204 us, which is why its delta read +14 us there
  against +4.1 in the six-launch run. The six-launch figure is the one to use.
- **The request-against-response asymmetry at zero bytes does not reproduce.**
  The three-launch sweep had +0.6 us on the response side and +2.7 on the
  request side, which would have been a sensible story -- the request body's
  relay adds a `BodyRelayService` coroutine per subrequest while the response
  body's adds a `.then()`. Over six launches the ordering reverses, +1.4 against
  +0.7, and both spreads straddle zero. There is a fixed cost of about a
  microsecond and this measurement does not resolve which direction pays more
  of it.
- **The relay caps every read at 64 KiB**, so a JS consumer that would have been
  handed a larger chunk is handed 64 KiB and makes more loop iterations. That is
  from reading `ReadRelay::relay()` and `AllReader::read()`, not from counting
  iterations here, and it is the likely reason the `downread` route's delta
  (+3.5 us at 64 KiB, +34 to +66 at 1 MiB) runs above the `arrayBuffer()` route's.
- **The absolute latencies are not comparable with section 2's.** The front
  worker parses a URL and reads the body into a buffer, and the client is pinned
  to another CCD, which findings above price at about 10 us; a bodiless crossing
  here is 76 us where section 2's is 47. Only the within-arm deltas and the
  ordering of the three configurations are claimed.
- **The synchronous fast path in `AllReader::read()` is behind an autogate**
  (`stream-controller-sync-fast-paths`), which is off in these runs, so on the
  `arrayBuffer()` routes the relay forgoes nothing that was switched on. The
  pump loop's `tryReadSync()` is not gated and is the one that was really given
  up; that is the `upthru` route, and it showed no cost.
- **Three configurations, one machine, one kernel, one worker shape.** The
  bodies are a single flat `Uint8Array` or a JS stream of them; a real body
  arrives over a socket in whatever chunks the network gave it, and the number of
  `tryRead` calls -- which is the relay's real unit -- would follow from that
  rather than from this harness.
- The six probes were not re-run, and no compiled output changed: the knob was
  reverted before committing and the commit touches only `measure/` and this
  document.

## Where a body's cost goes, and what makes the arena arm bimodal

### The question

The body-relay measurement above left two things open. A 4 MiB response body
costs 2.6 to 2.9 times more under the guest kernel than natively with heap
arenas **off** (arm G), but G differs from N in the allocator as well as in the
guest kernel, so the ladder orders the three configurations without
apportioning the cost. And the shipping configuration (arm A) ran 1.8 times
slower in four of six launches than in the other two, on every route at once,
with no counters taken. This section apportions the per-byte cost to the
allocator, to the guest kernel, to the thread cache and to per-isolate
placement, one variable per step, and finds what the two modes of A are.

### Method

One jemalloc binary and one plain binary, both `-c opt` from the same tree,
serve every arm; the arms differ only in environment. Two temporary knobs,
reverted before committing and not part of the tree:

- `WORKERD_GK_SUPPRESS_HEAP_ARENAS` makes `heapArenasAvailable()` answer false
  whatever the thread cache's state and demotes `checkHeapArenaSupport()`'s
  `KJ_REQUIRE` to a warning, so the jemalloc binary runs under the guest
  kernel with no `HeapArena` created. Its first call logs the thread cache's
  state and its answer, and every launch's log was checked for that line.
- `WORKERD_GK_STATS_PERIOD` prints gk's counters (`gk_get_stats`) and, in the
  jemalloc binary, `HeapArena::take()`'s counters and jemalloc's per-arena
  statistics to stderr every half second from a host-only thread.

The same two workers and routes as the relay measurement (`bodyrelay.py`,
imported), on the same CPUs (server `12-17,36-41`, client `0-5,24-29`), one
connection, p50, with the VMSCAPE mitigation reading `Mitigation: IBPB before
exit to userspace` and `halt_poll_ns` 200000 before and after every run.
Every arm is launched afresh in every round and **every launch is reported on
its own**; the harness (`apportion.py`) also records, per launch, where the
owner and its runner were pinned, the CPU every thread last ran on, gk's
counters and KVM's per-VM debugfs counters over each benchmark window, perf
counters per thread over the bodiless and 4 MiB windows, and the process's
huge-page backing. `results/apportion-ladder.json` is the ladder;
`apportion-bimodal-A.json` and `-A2.json` are twelve and ten launches of A
alone; `apportion-purge.json` is the jemalloc-option experiment in section 3.
The fault traces and call graphs quoted in section 3 were taken by hand on
further launches (`perf record` on `exceptions:page_fault_user`,
`kvm:kvm_page_fault` and `syscalls:sys_enter_madvise`, and `strace`), not by
the harness.

| arm  | binary   | thread cache | guest kernel                                   | heap arenas |
| ---- | -------- | ------------ | ---------------------------------------------- | ----------- |
| P0   | plain    | (tcmalloc)   | off                                            | --          |
| P1   | plain    | (tcmalloc)   | on, `GK_RUNNER=resident GK_RUNNER_PIN=other`   | --          |
| J0   | jemalloc | on           | off                                            | off         |
| J0t  | jemalloc | off          | off                                            | off         |
| J1   | jemalloc | on           | on, as P1                                      | suppressed  |
| J1t  | jemalloc | off          | on, as P1                                      | suppressed  |
| A    | jemalloc | off          | on, as P1                                      | **on**      |

P1 is the relay measurement's G, J0 its N and A its A. Eight launches per
arm, interleaved round by round, six seconds per benchmark.

### 1. The ladder, per launch

Response body of 4 MiB, p50 in microseconds, the eight launches in round order:

| arm | launches                                    | range     |
| --- | ------------------------------------------- | --------- |
| P0  | 1615 1628 1610 1623 1611 1605 1641 1606     | 1605-1641 |
| J0  | 1618 1596 1597 1630 1624 1634 1603 1621     | 1596-1634 |
| J0t | 1692 1680 1682 1686 1720 1697 1688 1726     | 1680-1726 |
| P1  | 4277 4848 4294 4856 4856 4214 4838 4717     | 4214-4856 |
| J1  | 4640 4646 4682 4670 4712 4673 4688 4745     | 4640-4745 |
| J1t | 4740 4759 4763 4761 4758 4764 4789 4718     | 4718-4789 |
| A   | 5673 5691 5356 5729 5366 5337 **2874** 5382 | 2874-5729 |

Request body of 4 MiB (`up`), and the pumped 4 MiB body (`upthru`), the same way:

| arm | up, launches                                | range     | upthru, launches                            | range     |
| --- | ------------------------------------------- | --------- | ------------------------------------------- | --------- |
| P0  | 1193 1120 1110 1173 1115 1126 1215 1110     | 1110-1215 | 1152 1155 1148 1157 1185 1114 1111 1138     | 1111-1185 |
| J0  | 1163 1153 1119 1165 1191 1126 1281 1157     | 1119-1281 | 1112 1139 1118 1117 1122 1148 1243 1136     | 1112-1243 |
| J0t | 1219 1196 1162 1163 1200 1236 1225 1193     | 1162-1236 | 1275 1253 1271 1254 1260 1248 1255 1272     | 1248-1275 |
| P1  | 1737 1738 1725 1741 1731 1742 1732 1752     | 1725-1752 | 1668 1661 1660 1664 1664 1665 1667 1677     | 1660-1677 |
| J1  | 1732 1724 1736 1733 1738 1731 1745 1746     | 1724-1746 | 1662 1653 1668 1662 1662 1655 1674 1669     | 1653-1674 |
| J1t | 1775 1762 1763 1770 1760 1760 1766 1771     | 1760-1775 | 1695 1686 1687 1694 1686 1685 1687 1691     | 1685-1695 |
| A   | 4302 4310 4317 4333 4333 4315 **1817** 4347 | 1817-4347 | 4348 4349 4355 4373 4360 4347 **1835** 4384 | 1835-4384 |

No body:

| arm | launches                                            | range       |
| --- | --------------------------------------------------- | ----------- |
| P0  | 77.7 80.3 76.6 78.7 79.9 80.2 78.7 78.8             | 76.6-80.3   |
| J0  | 79.7 79.9 78.7 79.0 79.8 81.6 77.0 78.7             | 77.0-81.6   |
| J0t | 92.6 90.5 92.7 88.4 94.0 91.9 91.5 95.5             | 88.4-95.5   |
| P1  | 91.7 92.0 91.7 92.3 92.5 92.9 92.4 92.0             | 91.7-92.9   |
| J1  | 91.5 92.6 91.9 91.5 91.8 91.8 91.4 93.4             | 91.4-93.4   |
| J1t | 108.2 108.9 107.5 108.2 108.0 109.0 109.5 107.8     | 107.5-109.5 |
| A   | 135.4 134.4 136.6 134.2 134.8 135.9 **110.9** 135.9 | 110.9-136.6 |

### 2. The apportionment

Each step changes one variable; the figures are ranges over eight launches,
not medians, and a step is claimed only where the ranges separate.

**The allocator by itself costs nothing here.** P0 and J0 -- tcmalloc against
jemalloc with its thread cache on, no guest kernel -- overlap on every route:
1605-1641 against 1596-1634 us at 4 MiB, 76.6-80.3 against 77.0-81.6 us with no
body. The relay measurement's N (jemalloc) was a fair stand-in for the plain
binary.

**Turning the thread cache off costs 2 to 17%, with or without the guest
kernel.** J0 to J0t: +60 to +130 us at 4 MiB (+5%), +11 to +14 us with no body
(+15%), +40 to +120 us on the request path, +130 us pumped (+12%). J1 to J1t,
the same change under the guest kernel: +40 to +150 us at 4 MiB (+2%), +16 us
with no body (+17%), +30 us on the request path, +25 us pumped. This is the
price of every allocation taking an arena bin lock, and it is what
`heapArenasAvailable()` requires; it is modest at 4 MiB and the largest
relative term on a bodiless request.

**The guest kernel is the per-byte cost, 2.9 times at 4 MiB, allocator held
fixed.** J0 to J1 -- jemalloc with its thread cache on in both, the guest
kernel the only change -- is 1596-1634 to 4640-4745 us on a 4 MiB response
(+3050 us, 2.9x), 1119-1281 to 1724-1746 on a 4 MiB request (+570, 1.5x),
1112-1243 to 1653-1674 pumped (+500), and 77.0-81.6 to 91.4-93.4 with no body
(+13 us). The plain binary agrees: P0 to P1 is 1605-1641 to 4214-4856 us. So the
relay measurement's observation stands and is now clean: the large per-byte
cost belongs to the guest kernel and not to the allocator and not to
placement. (P1 is itself bimodal at 4 MiB, 4214-4294 in three launches against
4717-4856 in five, 13% apart, a smaller effect in the same arm that the relay
measurement's G showed as a 4208-4657 range; it was not chased here.)

Where the guest kernel's per-byte cost goes is visible in the counters, and
the same in every guest-kernel cell: **about 2000 nested page faults per 4 MiB
response** (KVM `pf_taken` per request: P1 1992-2036, J1 2014-2060, J1t
1976-2001; 610-634 per 4 MiB request), and 1 to 2 with no body. That is one
fault per 4 KiB of an 8 MiB working set per request, and with it the KVM
`pages_4k` count rises by about 1350 per request during the response benchmark
and falls by about 600 per request during the request benchmark: the host
pages behind a body are being unmapped and refaulted continuously, and every
refault is a VM exit. The guest's own demand faults are not it (18 to 21 per
request, each installing some 60 neighbouring PTEs). This bears directly on
the huge-page question and is left for it: it says the page size the guest
maps at is not the only thing that decides what a body costs under the guest
kernel; what unmaps the host pages is.

**Per-isolate placement costs +12 to +21% at 4 MiB in one mode and saves 39% in
the other, which is to say it has no single figure.** J1t to A, the thread
cache off in both and the `HeapArena` the only change: seven launches at
5337-5729 us against J1t's 4718-4789 (+550 to +1010 us), and one at 2874 us
(-1850 us). On the request path, 4302-4347 against 1760-1775 (+2540 us, 2.45x)
in seven launches and 1817 (+50) in one; pumped, 4347-4384 against 1685-1695
in seven and 1835 (+145) in one; with no body, 134.2-136.6 against 107.5-109.5
(+26 us) in seven and 110.9 (+3) in one. Over all the launches of A taken here,
about one in four was in the fast mode: 8 of the 30 harness launches (one of
eight in the ladder, five of twelve and two of ten alone) and 5 of the 17
instrumented by hand below. The honest statement is the pair: in
its fast mode placement costs 3 us on a bodiless crossing and about 50 to 150
us on a 4 MiB body, and in its slow mode 26 us and 550 to 2540 us, with the
request path paying most. The next section is about what the modes are.

### 3. The two modes of the arena arm

**What they are not.** Not CPU placement: the owner's and the runner's CPUs are
recorded per launch, both modes occur on the same pairs (owner 12/runner 13 was
slow twice and fast once in the twelve-launch run, 37/38 slow once and fast
twice), and the runner is always on another core of the owner's CCD here.
Not the arena's own layout: `HeapArena::take()`'s sequence is identical in
both modes to the byte (nine takes of 48 MiB in all by the first body, then
64 MiB each, 2 MiB-aligned), as are jemalloc's statistics for the two isolate
arenas. Not huge pages on the host: in both modes about 95% of the process's
resident anonymous memory is `AnonHugePages`, `thp_fault_fallback` stays at
zero system-wide, and free memory is unfragmented (29,000 free order-10
blocks). Not the client, the mitigation, or the time of day: the modes
alternate freely in an A-only run (slow, slow, fast, slow, slow, fast, slow,
fast, fast, slow, slow, fast), and the probability does not depend on what
ran before.

**What differs, per request, is a storm of page faults on the host side.** In
a slow launch the owner thread, in user mode and outside the guest, takes about
1500 page faults per 4 MiB response (`exceptions:page_fault_user`, 940,000 in
two seconds of load at 190 requests per second) on a set of about 2300 pages
that it faults again and again, up to 490 times each in those two seconds --
once per request. The same pages take the guest's nested faults: 445,000
`kvm:kvm_page_fault` in the same two seconds, read faults, on 3500 distinct
pages faulting up to 438 times each. In a fast launch the owner takes 8500
host faults in two seconds on 4300 distinct pages, none more than six times;
its nested faults per request are 654 to 686 where the slow launch's are 2035
to 2088 (eight launches of A in the ladder, per launch: 2085 2087 2037 2088
2041 2035 **654** 2059), and its CPU per 4 MiB response is 4150 us where the
slow launch's is 7400 to 7950. With no body the slow mode takes 6 nested faults
per request to the fast mode's 1, and 288-292 us of CPU to 250.

The pages are jemalloc's, not the arena's. With call graphs, the slow launch's
host faults are in the continuation of `kj::Promise<kj::Array<unsigned char>>`
(46,000 of 88,000 in 0.3 s) and in `__memmove` called from
`workerd::(anonymous namespace)` on the event loop (41,500): the relay's copy
into the reader's buffers and the reader's concatenation of them. Under
`strace`, each request of the slow mode is followed by `madvise(MADV_DONTNEED)`
on a 4 MiB-plus-a-page extent at the same address every time, on two extents
of 2.5 MiB and 1 MiB, on one of 136 KiB and on five single pages, all in the
`0x7e..` range where jemalloc's automatic arenas map their extents and none in
the isolate's region: about 8 MiB discarded per request, which is the 4 MiB
body read in 512 KiB parts (`AllReader::read`'s `MAX_BUFFER_CHUNK`) plus the
4 MiB it is concatenated into. The next request takes the same extents back
from the arena's free run, cold, and faults every page of them on the host as
it writes and in the nested tables as the guest reads. The fast launch's
`strace` over the same second shows no `MADV_DONTNEED` at all on those ranges.
jemalloc's own counters say the same: arena 0's `dirty_purged` is 4.9 million
pages after a slow launch of 70,000 large allocations and 0.6 million after a
fast launch of 100,000, with `pdirty` sitting at 0 in the slow mode and 1680
in the fast.

So the slow mode is **jemalloc's shared arena returning the body buffers to
the kernel on every free, and the fast mode is it keeping them**. The purge
costs the slow mode about 1500 host faults and about 1400 extra nested faults
per 4 MiB response, roughly a microsecond each, which is the 2.3 to 2.5 ms
between the modes; a bodiless request loses 25 us to the same thing at a
smaller scale.

**What purges it.** With `perf record -e syscalls:sys_enter_madvise
--call-graph dwarf` on a slow launch under 4 MiB load, 1051 of the 1390
`madvise` calls in half a second come from one stack:
`je_pages_purge_forced <- je_extent_dalloc_wrapper <- pac_decay_to_limit <-
je_pac_decay_all <- arena_decay_impl <- je_arena_decay <- thread_arena_ctl <-
mallctlbymib`. That is the `thread.arena` mallctl -- `switchThreadArena()` in
`jsg/heap-arena.c++`, which `HeapArenaScope` and `SharedHeapScope` call on
every entry and exit -- and in jemalloc it goes through `arena_migrate()`
(`src/jemalloc.c`), which ends with:

    if (arena_nthreads_get(oldarena, false) == 0) {
        /* Purge if the old arena has no associated threads anymore. */
        arena_decay(tsd_tsdn(tsd), oldarena, false, /* all */ true);
    }

So every time the event-loop thread leaves arena 0 for an isolate's arena and
no other thread is bound to arena 0 at that moment, jemalloc purges **all** of
arena 0's dirty pages, whatever its decay settings say (`all` bypasses them),
and the body buffers it freed a moment ago go back to the kernel. The count
fits: jemalloc's `dirty_npurge` for arena 0 is 236,000 to 248,000 in every
slow launch of every arm of the experiment below, which is the number of
scope switches the workload makes, not the number of frees. In the fast
launch of the same probe the stack appears 3 times in half a second, under
the ordinary decay path, and the bulk of the `madvise` calls are the guest
kernel's own (`forward_syscall`), the same 261 as in the slow launch.

**What decides it at launch** is therefore whether some other thread is
bound to arena 0 for the life of the process. jemalloc's per-arena
`nthreads` for arena 0, sampled every half second: in the slow launches it is
0 in 62 samples of 73 and 1 in the rest (the owner itself, between scopes);
in the fast launches it is 1 in 64 of 73 and 2 in 7, so a second thread sits
in arena 0 and the owner's departures never empty it. jemalloc binds a thread
on its first allocation to the arena with the fewest threads
(`arena_choose_hard`), and arena 0 has zero threads whenever the owner is
inside a turn: a thread whose first allocation lands in such a moment is bound
to arena 0 for good and the launch is fast; if every other thread's first
allocation lands while the owner is between turns, arena 0's only thread is
the owner and the launch is slow. Which thread it is in the fast launches was
not identified (the candidates are V8's workers and kj's threads, whose first
allocations are early and timing-dependent), so the coin's exact weighting is
not explained, only its existence: about one launch in four.

**The settings that do not change it, and the one that would.** Six launches
each, interleaved, four seconds per benchmark, `apportion-purge.json`:

| arm, 4 MiB response, p50 us        | launches                                | arena 0 pages purged, millions, per launch |
| ---------------------------------- | --------------------------------------- | ------------------------------------------ |
| A, as shipped                      | 5218 5244 5181 5207 5193 5243           | 8.3 8.3 8.4 8.3 8.3 8.3                    |
| `MALLOC_CONF=oversize_threshold:0` | 2772 2781 **5214** 2781 **5183** **5210** | 0.2 0.3 **8.4** 0.5 **8.4** **8.4**       |
| `MALLOC_CONF=dirty_decay_ms:-1`    | 5233 5178 5217 5221 5222 4300           | 8.3 8.3 8.3 8.4 8.3 8.6                    |
| `MALLOC_CONF=dirty_decay_ms:0`     | 5440 5405 5422 5307 5326 5347           | 10.2 10.2 10.2 10.3 10.2 10.4              |

Disabling decay does nothing, because `arena_decay(all)` ignores it;
purging on every free (`dirty_decay_ms:0`) makes every launch slow and a
little slower still (and 147 us bodiless against 133); and jemalloc's eager
purge of oversize coalesced extents (`oversize_threshold`), which was the
first suspect, is not it either -- with it disabled, three launches of six
were still slow with the same 8.4 million pages purged (the three fast ones
purged 0.2 to 0.5 million, as every fast launch does). The one knob that
would end the bimodality is not a jemalloc option: keep arena 0 bound to a
thread that never leaves it (a dedicated idle thread set to `thread.arena`
0, say), or stop migrating the event-loop thread with `thread.arena` at all
and allocate into the isolate arena explicitly (`MALLOCX_ARENA`) instead. Of
the two outcomes that would fix, the fast one is the one to want: it is 39%
faster at 4 MiB than the same configuration without placement, because the
body buffers stay warm in arena 0 instead of being discarded by every
allocator's decay.

### 4. Two things found on the way

**The isolate's heap region fills at about 6.5 MB per 4 MiB response and the
isolate is condemned when it is full.** `HeapArena::take()` is a bump allocator
over a 16 GiB reservation with no reuse of its own, and jemalloc never asks it
to reuse: after a GC frees the backing stores, the arena's `retained` reaches
6 to 10 GB while `take()` keeps being called for fresh 64 MiB extents (123
takes and 6.2 GB retained after 36 s of one launch; 178 and 9.7 GB after a fast
one). Resident memory grows 7 GB in six seconds in the slow mode and 12 GB in
the fast, and in the instrumented launches, which ran the 4 MiB response for
about fifteen seconds, every launch ended with `take_bytes` at 16 GiB
(`last_off=0x3ff80c000`), 16.4 GB of `AnonHugePages`, and then "guest-kernel:
JS turn faulted inside the guest; the isolate has been condemned", after which
every request on the front worker fails, at 10,000 per second. In the fast
mode that is about five seconds of 4 MiB responses.

Settled afterwards, with `heap-region-test.sh` and an allocation census: it is
the GC that is late, and the reuse is fine. The jemalloc-level loop (the hooks
and arena settings of `HeapArena` over a 4 GiB reservation, a turn's worth of
allocations per iteration, freed inside or outside the turn, with the
`thread.arena` switch that purges the arena on every exit) takes four extents
and 13 MB of region over 3000 iterations whatever the variant: jemalloc
recycles what the purge moves to `retained`, so the region's high-water mark
is the isolate's peak live heap. What makes the peak is `back`'s, not
`front`'s: a `Response` built from the `Uint8Array` keeps a 4 MiB kj copy of
it (`JsReadableStream::Buffer`, allocated inside `back`'s turn) until the
`Response` is collected, and `back` allocates almost nothing else on its JS
heap, so V8 has no reason to collect -- `--trace-gc` over 2000 responses showed
scavenges in `front` only (its `ArrayBuffer`s, which V8 does account) and no
mark-compact at all, while `back`'s jemalloc `allocated` climbed to 3.4 GB; a
forced full GC dropped it to 288 KB. Reporting the copy to V8 as external
memory is necessary but not sufficient: V8's reaction to reported growth is to
start incremental marking, which workerd disables, and the fallback is its
old-generation limit, gigabytes for an isolate with no heap limit (the region
still stepped up by about 1 GB at a time). The fix is that report plus a
collection jsg runs itself at V8's soft limit, 64 MiB of growth past the
low-water mark (`ExternalMemoryTarget::applyLocked` in `jsg/jsg.c++`). With
it, `back`'s region holds at 100 MiB over 3050 responses and 12 GB served,
where without it the same run grew it to 3.8 GB with a scavenge-timed plateau
on the way.

**Every guest-kernel cell refaults a body's host pages once per request**, as
section 2 says: 2000 nested faults per 4 MiB response whether the allocator is
tcmalloc or jemalloc and whether arenas are on or off, with the KVM 4 KiB
mapping count rising and falling by hundreds of pages per request. The
discards that cause it were not traced in the non-arena cells; in A's slow
mode they are jemalloc's purges, and in the others they are presumably the
allocator's own returns of memory (tcmalloc's and jemalloc's decay) plus V8's,
which the strace of a fast A launch shows as one `mprotect` and one
`MADV_HUGEPAGE` of 4 MiB per request in the V8 sandbox and batched
`MADV_DONTNEED`s of 8 to 40 MiB there. The huge-page task should start from
this: a 2 MiB guest leaf saves nothing on a page the host has discarded.

### Caveats

- **The bodiless mode and the 4 MiB mode are correlated but not the same
  flag.** In one instrumented launch of three the bodiless request was in the
  slow mode (132.5 us) while the 4 MiB response was fast (1690 us), and one
  launch of the ten-launch run had a slow bodiless figure (136 us) with a 4 MiB
  response between the modes (4360 us). The purge mechanism explains the 4 MiB
  mode; what makes a bodiless request lose 25 us and take five more nested
  faults is consistent with it (five single-page `MADV_DONTNEED`s per request
  were seen) but was not traced on a bodiless-only load, and should be.
- **The thread that keeps arena 0 populated in a fast launch was not named.**
  The purge path was observed (the call stack above) and `arena_migrate()`'s
  condition read; that a second thread's binding to arena 0 is what the modes
  differ in rests on jemalloc's `nthreads` samples, 7 of 8 launches examined
  agreeing and one borderline launch (116 us bodiless) showing 0 in half its
  samples. Logging every thread's first `arena_choose_hard` would settle it in
  one launch of each mode.
- **The ladder's A column has one fast launch in eight**, so its placement
  figures are the slow mode's with one exception, and the fast mode's are
  from the A-only runs and the probes. Both are reported; neither is "the"
  placement cost.
- **The fast mode's 4 MiB figures are measured over six seconds or less**,
  because in that mode the isolate's region fills in about five seconds of
  4 MiB responses and the isolate dies; the relay measurement's eight-second
  windows were long enough to reach that in principle, and it is possible its
  two fast launches were partly measured against a dying isolate.
- **Six probes not re-run; nothing compiled is committed.** Both knobs were
  reverted before staging and the commit touches only `measure/` and this
  document.

## Whether the soft-limit collection belongs in every build

### The question

`ExternalMemoryTarget::applyLocked` (`jsg/jsg.c++`) has the isolate collect
garbage once the memory reported through it has grown 64 MiB past its
low-water mark, V8's `kExternalAllocationSoftLimit`, with
`MemoryPressureNotification(kCritical)`. It went in for the heap-arena build,
where a worker serving 4 MiB bodies from a buffer spent its 16 GiB region in
seconds (section 4 above), but it runs in every build. The doubt: the comment
that shipped with it said V8's own limits are gigabytes for an isolate given
no heap limit, and workerd's measurement gave none, while a production worker
has one of about 128 MiB. If a limit that small makes V8 collect on its own,
the accumulation is a no-limit artifact and the collection, a full
memory-reducing GC that also drops V8's caches, is a cost imposed on
everyone for nothing. The doubt is settled below, against it: the heap limit
does not enter into when V8 collects for reported memory, and the exhaustion
reproduces under 128 MiB to the megabyte.

### What heap limit workerd gives an isolate

None, by default. `NullIsolateLimitEnforcer::getCreateParams()` in
`server/server.c++` returns an empty `v8::Isolate::CreateParams`, so V8
configures the heap from its own defaults (`Heap::ConfigureHeap`): a maximum
old generation of 1,400 MB on 64-bit (`700 * (kSystemPointerSize / 4) * MB`),
not from the machine. The only way to give a `workerd serve` isolate a limit
is the process-wide `Config.v8Flags`, `--max-old-space-size=128`, which lands
in the same `max_old_generation_size` that
`ResourceConstraints::set_max_old_generation_size_in_bytes()` sets; that is
what the limited arms below use. The 128 MiB of Cloudflare's runtime is the
figure `jsg/setup.c++` quotes beside `--noincremental-marking`; the enforcer
that applies it is not in this tree, so what else it does is not known here.

### What V8 15.4 does with reported memory, read from the source

`Heap::UpdateExternalMemory` (`src/heap/heap.cc`) is the whole of V8's
reaction to `AdjustAmountOfExternalAllocatedMemory`. It collects at once only
if `ReachedHeapLimit()`, which is the old generation's *capacity* against its
maximum (the global form that would count external memory is behind
`--enforce-global-heap-limit`, off by default). Otherwise, past the soft limit
it calls `StartIncrementalMarkingIfAllocationLimitIsReached`, which does
nothing when `IncrementalMarking::CanBeStarted()` is false, and workerd's
`--noincremental-marking` makes it false. `Heap::external_memory_hard_limit()`,
the old rule of half the maximum old generation, is still defined and no
longer called from anywhere. After that the reported total enters only
`GlobalConsumedBytes()`, which `ShouldExpandOldGenerationOnSlowAllocation`
checks against the global allocation limit *when the JS heap next needs an
old-space page*. So with incremental marking off, reported memory is collected
when the mutator next promotes a page's worth of objects, and the heap limit
decides only how soon that check starts refusing, which both arms below reach
within the first burst. V8 does collect for its own ArrayBuffer backing
stores on a different path, a scavenge in `AllocateExternalBackingStore` once
64 MB of young ones are outstanding, which is why `front`, whose bodies become
`ArrayBuffer`s, never needed help.

### Method

The same build as section 4 (jemalloc, `--platform_suffix=_jemalloc`), run
through the heap-region workload with `--trace-gc` set process-wide through
the capnp (`measure/extgc/region-*.capnp`) and sampled through the
`__gk_heap_region` probe after each burst of 250 responses
(`measure/extgc/region.sh`). Two arms by binary: the collection on, as
committed, and off, through the uncommitted knob `cost.sh` describes, which
leaves the report in place and skips only the collection. Two arms by
config: no heap limit, and `--max-old-space-size=128`. 3,050 responses of
4 MiB per run, 12 GB after the warm-up.

The cost ran the workers in `measure/extgc/cost-*.js` under oha at one
connection (eight for the second hello row), six seconds after two of
warm-up, workerd on CPUs 12-17,36-41 and oha on 0-5,24-29, arms interleaved
round by round with a fresh launch each, six launches per arm
(`measure/extgc/cost.sh`): `hello` answers a string; `bigheap` builds 500,000
objects on its first request and keeps them (42.5 MB live after a
mark-compact) and answers a string; `bigbody` answers 4 MiB from a buffer it
keeps, the shape that fills the region; `bigheapbody` does both. The
production-like cell is the default build (tcmalloc, guest kernel off) with
`--max-old-space-size=128`; the arena cell is the jemalloc build with the
guest kernel on and no limit. GC counts and pauses are from one further
launch per cell with `--trace-gc` and one worker per launch
(`measure/extgc/gcsum.py`), so that the isolate in the trace is known.

**The counts this section takes from a trace are low.** The binary measured
here gave `stdout` a fully buffered `BUFSIZ` buffer that nothing ever flushed,
so the end of every log -- up to 8 KiB, 33 lines in the cell that was measured
for it -- never reached the file. "A `--trace-gc` log that reaches the file"
below says how much and which figures it touches. The throughput, latency,
memory and region figures do not come from the trace and are unaffected.

### 1. The region under a 128 MiB heap limit

`back`'s region, MiB, after the warm-up and each burst, collection off
(`extgc-region-off-nolimit.txt`, `extgc-region-off-limit128.txt`):

| limit   | warm | 1   | 2    | 3    | 4    | 5    | 6    | 7    | 8    | 9    | 10   | 11   | 12   | mark-compacts |
| ------- | ---- | --- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ------------- |
| none    | 272  | 968 | 1800 | 1800 | 2440 | 3208 | 3208 | 3208 | 3528 | 4616 | 4616 | 4616 | 4616 | 4             |
| 128 MiB | 272  | 968 | 1800 | 1800 | 2440 | 3208 | 3208 | 3208 | 3528 | 4168 | 4168 | 4168 | 4168 | 4             |

The two arms are the same run to the megabyte through burst 8 and differ by
one burst's timing of the fourth collection after that. The four
mark-compacts come at bursts 1, 2, 5 and 10 in both, each at a step of the
old generation's committed size (0.8, 1.5, 2.5 and 3.2 MB in the trace): the
page expansions the reading above predicts, and nothing else. Between them
`back`'s jemalloc `allocated` climbs by 4 MB per response to 4.3 GB. With the
collection on, both arms hold `back`'s region at 100 MiB over the whole
12 GB, 359 and 360 mark-compacts of 0.75 ms on a 0.2 MB heap, two per
trigger, because `CollectGarbageOnMemoryPressure` runs a second collection
whenever it estimates 8 MB of potential garbage after the first
(`extgc-region-on-*.txt`). So the condition is not the absence of a heap
limit; it is the absence of incremental marking, which every workerd build
shares.

### 2. What the collection costs, per launch

Requests per second, six launches of each arm in round order; the arms are
the same binary. Default build, `--max-old-space-size=128`
(`extgc-cost-default-limit128.txt`):

| workload        | collection on                       | collection off                      |
| --------------- | ----------------------------------- | ----------------------------------- |
| hello, c=1      | 22914 22763 22506 22623 22819 22829 | 23012 23052 22530 22686 22974 23196 |
| hello, c=8      | 28970 28795 29105 28708 28938 29309 | 28911 29130 29190 29630 28816 28557 |
| bigheap, c=1    | 22077 22067 22095 22070 22355 21661 | 22177 22463 21575 21997 22486 22379 |
| bigbody, c=1    | 1267 1251 1238 1267 1260 1259       | 979 1016 1011 991 996 1015          |
| bigheapbody     | 345 351 355 351 347 348             | 875 921 874 890 900 972             |

jemalloc build, guest kernel on, no limit (`extgc-cost-jemalloc-gk-nolimit.txt`):

| workload        | collection on                 | collection off                |
| --------------- | ----------------------------- | ----------------------------- |
| hello, c=1      | 5863 5951 5994 5887 6065 5957 | 5944 5759 5902 5947 5882 5930 |
| hello, c=8      | 7764 8211 8192 7673 8185 7619 | 7741 7720 7445 8263 7945 7650 |
| bigheap, c=1    | 6052 6086 5993 5994 6068 6100 | 6046 6006 6049 5989 6056 6095 |
| bigbody, c=1    | 1057 1052 1051 1046 1041 989  | 878 888 866 888 944 911       |
| bigheapbody     | 408 423 422 412 419 419       | 621 636 762 778 783 674       |

Three launches of the default build with no limit
(`extgc-cost-default-nolimit.txt`) repeat the limited cell within its spread
in every row. Resident memory at the end of each six-second window, GB:
`bigbody` 1.1-1.3 on against 6.0-7.0 off in the default build, 1.0 against
7.6 under the arenas; `bigheapbody` 1.1-1.4 on against 9.9-15.6 off, and
1.1 against 17.0-19.2 under the arenas, which is the region a second or so
from spent.

What the rows say:

- **Where nothing reports memory in bulk, the collection costs nothing.**
  `hello` and `bigheap` never cross the soft limit, and their six launches
  per arm overlap entirely; what remains of the change on that path is one
  atomic add per adjustment.
- **With a small heap it is a gain.** `bigbody` is 25% faster with the
  collection on in the default build (mean 787-806 us against 983-1019) and
  15% under the arenas, because tcmalloc and jemalloc hand the next body the
  pages the last one freed, warm, where the off arm faults 4 MiB of fresh
  pages per response. The collection itself is 2 mark-compacts of 0.3 ms per
  16 responses, 5.5% of the window (1,161 in the trace, itself short by up to
  a buffer's worth of lines; the off arm's whole trace fits inside one buffer
  and so reached the file empty, which is why it appeared to collect not at
  all in 6,050 responses -- re-measured, its launches collect 0 to 7 times).
- **With a large heap it is the cost, and it grows with the heap.** At 42.5 MB
  live, `bigheapbody` runs at 345-355 against 874-972 requests per second in
  the default build: the trace shows 330 mark-compacts of 16.2 ms (35.9 max)
  in six seconds, 5.33 s of the window in collection, two per 64 MiB of
  bodies, each marking the whole heap; p99 goes from 1.4-1.5 ms to 40 ms.
  Under the arenas the same rows are 408-423 against 621-783 and 402
  collections of 11.4 ms. The pause is proportional to the live heap, so a
  worker at the 128 MiB production limit would pay about three times this
  per 64 MiB of bodies, around 6 ms per 4 MiB response.

The off arm's rows are not free either; they are the cost deferred. Its
process holds 6 to 19 GB after six seconds of one worker, 4 MiB per response
until the mutator next promotes a page's worth of objects, which for
`bigbody` was never in six seconds and for `back` was every 1 GB. In the
shared-heap build that is what the runtime did before the copies were
reported at all, since an unreported copy is invisible to V8 and the
expansion check alone; under the arenas it is the condemnation of section 4.

### The verdict

The collection stays in every build, unconditional, with the 64 MiB soft
limit. Of the alternatives:

- **Gating on `heapArenasAvailable()`** was the leading candidate and is
  rejected on the rows above: the shared-heap build accumulates the same
  4 MiB per response (6 to 16 GB resident after six seconds of one worker), and
  V8 has no path of its own that would collect it sooner in that build than
  in the arena one. The region is where the accumulation becomes fatal, not
  where it happens.
- **Gating on whether the isolate has a heap limit** is rejected on section 1:
  the limit does not change when V8 collects for reported memory, by the
  trace or by the source.
- **A soft limit scaled to the heap limit, the way V8's old hard limit was**
  (`max_old_generation_size / 2`) would give exactly 64 MiB at the 128 MiB
  production limit, so it changes nothing where it matters, and 700 MB for a
  `workerd serve` with no limit, where the region is 16 GiB and the shared
  heap unbounded, so it buys dev a seven-fold larger transient for no gain.
  Rejected as complexity without effect.

The agreement asked for: yes, the report is accounting that is simply
correct, and would be worth keeping on its own. A buffer-built body's copy
goes through the very counter V8 keeps for the `ArrayBuffer` it was copied
from (`ExternalMemoryAccounter` in `array-buffer-sweeper.cc` calls the same
`UpdateExternalMemory`), so the copy counts as its source already does; but
on its own, in a workerd, that count only hastens the next page expansion's
collection, and it is the collection that carries both the benefit and the
cost.

The 64 MiB figure is V8's own policy for an embedder's memory, applied here
because V8 cannot apply it without incremental marking; the throughput an
isolate loses to it with a large live heap is what V8 with incremental
marking on would spend as well, as marking work if not as pause. What is
workerd's own, and the lever if that cost needs reducing, is the choice of
`kCritical`: two collections where one would do, and V8's compilation cache,
concurrent optimization jobs and pooled pages dropped with them -- the pooled
pages of every isolate in the group, which in the shared-heap build is the
process. A plain `Heap::CollectAllGarbage` is not reachable through the
public API without `--expose-gc`; a V8 patch exposing one, in the manner of
`patches/v8/0030`, would halve the cost measured here and leave the caches
alone, and is the next step if a workload like `bigheapbody` turns out to be
common. A soft limit that grows with the live heap is the other lever, and a
policy change rather than a correction, so it is left to whoever has the
production numbers to set it against.

### Found on the way

**`--trace-gc` faults the next isolate under heap arenas, and so would
anything else that first writes to `stdout` from inside a turn.** The C
library allocates a stream's buffer on the first write to it; V8's trace line
was the first thing written to `stdout` in these runs, from inside `back`'s
turn, so the buffer went into `back`'s arena and the first scavenge `front`
traced faulted on it (`cr2` in `back`'s slot, from `front`'s turn, during the
warm-up of every run). Running under `stdbuf -o0` confirmed it: no fault, and
the collection on its own never faulted. `jsg/setup.c++` hands `stdout` a
static buffer with `setvbuf()` before V8 is initialized, which is the same cure
for the same shape of problem, and the run that found it passes with the flag
and without `stdbuf` on the committed build.

### Caveats

- **The production enforcer was not measured, only its limit.** The limited
  arms give the isolate the 128 MiB through `--max-old-space-size`, which is
  the one field a `ResourceConstraints` limit would set; whatever else
  Cloudflare's enforcer does (a near-heap-limit callback, its own memory
  accounting) is not in this tree and could change the off arm's fate, though
  not V8's timing.
- **The large heap was 42.5 MB, not 128.** The per-response cost at the
  production limit is extrapolated from the pause's proportionality to live
  size (0.29 ms at 0.2 MB, 16 ms at 42.5 MB), not measured.
- **Six seconds per window.** Long enough for the off arm to show its growth
  and the on arm its steady state; not long enough for the off arm's
  expansion-timed collections to appear in `bigbody` at all, so the off
  rows carry the fresh-page cost and none of the eventual collection's.
- **One connection.** The cost is a mutator-side pause, so concurrency would
  change the throughput figures without changing the per-collection cost.
- **The second collection per trigger was counted, not explained.**
  `CollectGarbageOnMemoryPressure` estimates potential garbage from
  committed-minus-live plus `external_memory()`; which of the two keeps the
  estimate over 8 MB after the first collection was not traced.
- **The arena cell's bimodality did not show.** Six launches of `bigbody` on
  are within 7% of one another; either every launch landed in one mode or
  the body path here, which does not cross a service binding, does not
  exercise it.
- **Every count the trace gives is short by up to one buffer**, as the method
  says. Where an arm of this section survives in the tree, the section after
  it re-measured the same cells with the tail arriving, and those counts are
  the ones to quote.

## Keeping the shared arena populated

### The question

The section before last found the shipping configuration bimodal per launch,
by up to 1.8x on every route at once, and traced the slow mode to jemalloc
purging arena 0 on every scope switch when no other thread was bound to it.
This section settles how to stop that: what jemalloc's own source allows,
which of three candidate fixes to take and why, and whether the configuration
with the fix lands in the former fast mode or somewhere else.

### What jemalloc does, read from the source

The build's jemalloc is the unpatched 5.3.0 module from the Bazel Central
Registry (`build/deps/deps.jsonc`). Writing `thread.arena` goes through
`thread_arena_ctl` (`src/ctl.c`), which first binds the thread to an
automatic arena if it has none (`arena_choose`) and then, for any change of
index, calls `arena_migrate` (`src/jemalloc.c`):

    arena_nthreads_dec(oldarena, false);
    arena_nthreads_inc(newarena, false);
    tsd_arena_set(tsd, newarena);
    if (arena_nthreads_get(oldarena, false) == 0) {
        /* Purge if the old arena has no associated threads anymore. */
        arena_decay(tsd_tsdn(tsd), oldarena, false, /* all */ true);
    }

Nothing conditions the purge but the thread count. `arena_decay(all)` goes to
`arena_decay_impl`, whose `all` branch takes the decay mutex and calls
`pac_decay_all` without reading the arena's decay time; the decay settings are
consulted only on the other branch (`pac_maybe_decay_purge`), which is why
`dirty_decay_ms:-1` changed nothing in the experiment above. `pac_decay_all`
is `pac_decay_to_limit` with a limit of zero pages over every dirty extent,
and `pac_decay_stashed` with `fully_decay` set skips the muzzy step and hands
each extent to `extent_dalloc_wrapper` (`src/extent.c`): with the default
hooks and `retain` on (the default on 64-bit Linux) deallocation is declared
to fail, decommit fails on an overcommitting Linux, and so the extent is
purged with `pages_purge_forced`, which is `madvise(MADV_DONTNEED)`, and
recorded in the retained pool -- the stack the experiment above caught. For
an isolate's arena the same path runs with `HeapArena`'s hooks, which refuse
every step, so the extents move to retained with their pages untouched; that
purge is bookkeeping and stays.

There is no control that keeps the purge from a thread that leaves. The
`percpu_arena` mode refuses `thread.arena` for the automatic range outright
(`EPERM`), and so cannot be combined with the scopes at all. Thread death
takes a different path, `arena_cleanup` to `arena_unbind`, which only
decrements the count: a thread that dies bound to an arena does not purge it;
only a thread that migrates does. And `arena_choose_hard` is as the previous
section inferred: among the initialized automatic arenas it takes the lowest
index with the fewest threads, and uses that only if its count is zero,
otherwise it initializes the next uninitialized arena -- so a thread's first
allocation lands in arena 0 exactly when arena 0 is empty at that moment,
which it is while the event loop is inside a turn.

### The choice

Three ways to end the dependence on that coin were weighed.

**Keep a thread bound to the shared arena, taken.** `HeapArena`'s
constructor, where the first arena is created and so the first scope switch
becomes possible, starts a thread once that binds itself to arena 0 with
`thread.arena`, blocks every signal and pauses forever; the constructor waits
for the binding before it returns, so no scope switch precedes it
(`keepSharedHeapArenaPopulated()` in `jsg/heap-arena.c++`). Arena 0's thread
count can then never reach zero, and `arena_migrate` never purges it. This
holds the condition that distinguished the fast launches rather than removing
the dependency on it. What makes that acceptable: the thread cannot exit (it
runs nothing after binding, and with its signals blocked nothing but the
process's death ends its `pause()`), it is bound before any scope can switch,
and a check in `guest-arena-test` fails the moment either stops being true.
What it does not cover, and what would bring the purge back: a thread whose
home arena is another automatic arena entering a `HeapArenaScope`, since that
thread leaves *its* home arena and the keeper holds only arena 0. In workerd
the event loop is the main thread, which jemalloc binds to arena 0 before
any other thread exists, and no other thread enters a scope; a server with
several event-loop threads would need a keeper per home arena, or every event
loop moved onto arena 0, and that is the first thing to revisit if the scopes
ever run on more than one thread. A change in jemalloc's binding rule would
move the main thread's home the same way.

**Stop migrating the thread, rejected as too large for what it buys.**
Keeping a thread-local arena index and allocating with
`mallocx(..., MALLOCX_ARENA(i))` would remove the dependency outright: no
thread would ever leave an arena. But nothing in this tree routes allocation
through a shim, so it would mean interposing `malloc`, `calloc`, `realloc`,
`posix_memalign`, `aligned_alloc`, `memalign`, `free` and
`malloc_usable_size` (the C++ `operator new` family reaches `malloc`, so it
comes along) and keeping jemalloc's own symbols reachable under a prefix; and
what the shim does not see -- the C library's own allocations from inside a
turn, which call jemalloc's `malloc` directly and use the thread's bound
arena -- would land in the shared heap where today a scope puts it in the
isolate's arena. Placement by site survives only for the sites the shim
covers, so the design would have to re-audit what the C library allocates
during a turn. That is a larger change with a correctness tail, for a cost
(one `mallctlbymib` per scope, under a microsecond) that the keeper leaves
as it is.

**jemalloc's own controls, none found that applies.** `narenas:1` would make
every thread's home arena 0, so that V8's worker threads keep it populated
from their first allocation on; that also holds the condition rather than
removing it, with the additional cost of every thread's allocations contending
on one arena's bin locks with the thread cache off, and was not measured.
Patching `arena_migrate` out of purging (a `single_version_override` of the
registry module with a patch, a mechanism the tree does not use for its
`bazel_dep`s yet) would remove the dependency for every arena and every
thread at once, in three lines; it was not taken because it adds a patched
third-party module for a behaviour the keeper neutralizes exactly in the one
configuration that exists, and it stays the step to take if the keeper's
limitation above ever bites.

### Method

One jemalloc binary, `-c opt --platform_suffix=_jemalloc` from the tree as
committed with the keeper, plus two measurement-only knobs that were reverted
before committing and are not part of the tree:

- `WORKERD_GK_SUPPRESS_SHARED_HEAP_KEEPER` leaves the keeper out, so the same
  binary serves the before arm. Its one log line is checked in every
  launch's `config_lines`.
- `WORKERD_GK_STATS_PERIOD` prints arena 0's `nthreads`, `dirty_npurge`,
  `dirty_purged`, `dirty_nmadvise` and `pdirty` as a `[gkstats]` line every
  half second from a host-only thread that binds itself to another automatic
  arena before the first isolate exists, so that it cannot be the thread
  that populates arena 0 by chance. `apportion.py` records the last line
  before and after each benchmark window.

The same workers, routes and CPUs as the two sections above (server
`12-17,36-41`, client `0-5,24-29`, `GK_RUNNER=resident GK_RUNNER_PIN=other`),
one connection, p50, six seconds per benchmark, perf off, eight rounds with
both arms launched afresh in every round and interleaved
(`results/keeper-ab.json`). The VMSCAPE mitigation read `Mitigation: IBPB
before exit to userspace` and `halt_poll_ns` 200000 before and after.

The absolute figures here are not those of the section before last: commit
`c407a287c` (the soft-limit collection, the section above) sits between, and
with it `back`'s region holds at about 100 MiB instead of filling at 6.5 MB
per response, so the 4 MiB response is faster in both modes than it was
measured then (1.8 ms against 2.9 in the fast mode, 4.3 against 5.1 to 5.7 in
the slow). What is compared is the two arms of this run against each other.

### 1. Every launch, both arms

p50 in microseconds, rounds in order; arena 0's `dirty_npurge` at the end of
the 4 MiB-response window (cumulative since launch) and over that window;
pages purged since launch; `pdirty` at the end of the window; and over the
same window, KVM `pf_taken` per request and the process's CPU per request.

Keeper on (the configuration as committed):

| rnd | no body | 64 KiB | 4 MiB down | 4 MiB up | pumped | npurge | in window | purged pages | pdirty | pf/req | cpu us/req |
| --- | ------- | ------ | ---------- | -------- | ------ | ------ | --------- | ------------ | ------ | ------ | ---------- |
| 0   | 110.5   | 154.8  | 1805       | 1780     | 1815   | 85     | 67        | 43,448       | 1690   | 641    | 2958       |
| 1   | 110.3   | 154.5  | 1798       | 1798     | 1823   | 98     | 72        | 51,839       | 2713   | 647    | 2946       |
| 2   | 109.7   | 155.1  | 1807       | 1808     | 1851   | 102    | 76        | 50,649       | 1690   | 642    | 2959       |
| 3   | 109.2   | 156.8  | 1831       | 1805     | 1840   | 102    | 72        | 47,819       | 1690   | 645    | 3000       |
| 4   | 109.2   | 156.2  | 1799       | 1815     | 1832   | 104    | 81        | 52,288       | 1690   | 645    | 2969       |
| 5   | 110.7   | 157.0  | 1802       | 1805     | 1830   | 105    | 78        | 52,979       | 1690   | 645    | 2947       |
| 6   | 108.8   | 155.1  | 1799       | 1822     | 1859   | 98     | 73        | 49,897       | 1690   | 646    | 2959       |
| 7   | 108.7   | 156.8  | 1814       | 1802     | 1831   | 91     | 58        | 42,569       | 1690   | 642    | 2956       |

Keeper suppressed, the same binary:

| rnd | no body   | 64 KiB    | 4 MiB down | 4 MiB up | pumped   | npurge  | in window | purged pages | pdirty | pf/req | cpu us/req |
| --- | --------- | --------- | ---------- | -------- | -------- | ------- | --------- | ------------ | ------ | ------ | ---------- |
| 0   | 127.8     | 194.0     | 4276       | 4305     | 4373     | 259,565 | 9757      | 4,807,551    | 0      | 1996   | 6214       |
| 1   | **107.8** | **154.9** | **1814**   | **1783** | **1835** | 17,017  | 76        | 150,258      | 1793   | 653    | 2986       |
| 2   | 127.7     | 192.8     | 4275       | 4306     | 4355     | 260,100 | 9762      | 4,819,862    | 0      | 1997   | 6206       |
| 3   | 126.4     | 194.0     | 4300       | 4316     | 4372     | 260,569 | 9704      | 4,790,131    | 0      | 1998   | 6257       |
| 4   | 127.7     | 192.2     | 4268       | 4307     | 4367     | 301,571 | 9752      | 4,812,194    | 0      | 1997   | 6205       |
| 5   | **109.3** | **154.5** | **1799**   | **1823** | **1868** | 28,039  | 73        | 164,087      | 1690   | 645    | 2965       |
| 6   | **110.0** | **155.9** | **1791**   | **1782** | **1828** | 13,579  | 84        | 147,752      | 1690   | 653    | 2930       |
| 7   | 127.0     | 192.1     | 4295       | 4295     | 4352     | 316,297 | 9681      | 4,809,143    | 0      | 1995   | 6238       |

The `nthreads` sample at the end of the 4 MiB window was 1 in every launch
with the keeper (the keeper alone, the owner being inside a turn when the
sample was taken), 0 in the five slow launches without it, and 1 or 2 in the
three fast ones.

### 2. What the tables say

**The mode is gone.** Eight launches of eight with the keeper are within
108.7 to 110.7 us with no body, 154.5 to 157.0 at 64 KiB, 1798 to 1831 on the
4 MiB response, 1780 to 1822 on the 4 MiB request and 1815 to 1859 pumped --
spreads of 2 to 2.5% -- where the same binary with the keeper suppressed
alternated between that and 126.4 to 127.8, 192 to 194, 4268 to 4300, 4295 to
4316 and 4352 to 4373: 2.4 times slower on a 4 MiB body in either direction
and 17 us slower with no body, in five launches of eight (the earlier
section's one in four fast was three in eight here).

**Arena 0's purge count no longer tracks the scope switches.** With the
keeper, arena 0 was purged 85 to 105 times in a whole launch, 58 to 81 of them
in the six seconds of the 4 MiB window, with 1690 pages dirty at the end of
it: that is jemalloc's ordinary decay, the same 73 to 84 per window that the
three fast launches without the keeper show. In the five slow launches it was
purged about 9,700 times per window, roughly seven times per request, which
is the number of scope switches a request makes, and 4.8 million pages in all
with nothing left dirty. The sample's `nthreads` confirms the mechanism from
the other side: with the keeper the count never read zero.

**It is the fast mode, not a third behaviour.** Every column of the keeper
arm lies within, or within 1% of, the three fast launches of the suppressed
arm: 641 to 647 nested faults per 4 MiB response against 645 to 653 (the slow
launches' 1995 to 1998), 2946 to 3000 us of process CPU per response against
2930 to 2986 (the slow launches' 6205 to 6257), and the p50s above. Nothing
else came to dominate once the purge stopped; what remains at 4 MiB is the
guest kernel's own per-byte cost, apportioned above (J1 against J0), and what
remains of placement is the fast mode's few microseconds on a bodiless
request.

### 3. The committed binary

Eight launches of the shipping configuration on the jemalloc binary built from
the tree as committed, knobs and all other differences absent
(`results/keeper-committed.json`), after the probe sweep on the same machine;
no `[gkstats]` line exists in this binary, so arena 0's counters are not
sampled, and the nested faults and CPU per request over the 4 MiB window are
what says which mode a launch is in:

| rnd | no body | 64 KiB | 4 MiB down | 4 MiB up | pumped | pf/req | cpu us/req |
| --- | ------- | ------ | ---------- | -------- | ------ | ------ | ---------- |
| 0   | 108.7   | 154.1  | 1796       | 1792     | 1831   | 648    | 2964       |
| 1   | 106.5   | 153.7  | 1797       | 1809     | 1824   | 643    | 2952       |
| 2   | 106.9   | 153.0  | 1813       | 1791     | 1833   | 650    | 2978       |
| 3   | 108.0   | 153.1  | 1812       | 1808     | 1853   | 644    | 2945       |
| 4   | 108.8   | 153.3  | 1801       | 1790     | 1832   | 683    | 3040       |
| 5   | 108.1   | 155.2  | 1789       | 1787     | 1833   | 643    | 2953       |
| 6   | 108.2   | 154.4  | 1800       | 1827     | 1871   | 648    | 2953       |
| 7   | 107.9   | 155.3  | 1790       | 1775     | 1819   | 646    | 2925       |

Every launch is in the fast mode, within the keeper arm's spreads above (a
microsecond or two faster with no body, which is within what the knob
binary's sampling thread could account for and was not pursued).

### Caveats

- **The keeper covers arena 0 and the main thread.** Its reasoning depends on
  jemalloc binding the first thread to allocate -- the main thread, which is
  workerd's event loop -- to arena 0, and on no other thread entering a
  scope. Both are true of `workerd serve` today and neither is enforced; the
  unit test checks the keeper's presence and effect for the main thread
  only.
- **Six-second windows, one connection, one machine**, as in the sections
  above; the mode was a per-launch property and eight launches of each arm
  are enough to see it gone, not to put a confidence interval on the fast
  mode's figures.
- **The two knobs are not committed**; the before arm exists only in
  `results/keeper-ab.json`, and the binary that served both arms differs from
  the committed one by the knobs. Section 3 below is the committed binary.
- **`bench/bench.capnp`'s `cdn` pair faults under heap arenas**, found on the
  way and not chased: 200 requests to its front worker, which forwards the
  request object over a service binding and returns the origin's response,
  condemn the `cdn-origin` isolate ("JS turn faulted inside the guest") at an
  address 160 KiB above a 512 GiB slot base, on the jemalloc build with the
  guest kernel on and not on the plain build with it on. The `hello` and
  `alloc` workers of the same config do not fault. It reproduces with the
  keeper suppressed, so it is not the keeper's, and is left for whoever next
  uses that config.

## What the socket relays cost

### The question

A `connect()` tunnel through an in-process service binding is a `kj` two-way
pipe and a WebSocket through one is a `kj` WebSocket pipe; where per-isolate
heap arenas are active, `IoStreamRelay` and `WebSocketRelay`
(`io/worker-interface.c++`) make every read and every receive from the event
loop, as `ReadRelay` does for bodies. "What the body relay costs" above put the
body relay at about a microsecond per crossing plus 8 to 14 us per MiB, and
its unit is a `tryRead()` call, of which a body pays few because the reader asks
in 128 to 512 KiB buffers. A socket's unit is whatever the worker's JS reads
and a WebSocket's is a message, so a chatty protocol pays the relay's fixed
cost per read or per message, and the body figures do not transfer. This is
the number for small messages.

### Method

`socketrelay.py`, with `bodyrelay.py`'s arms and knob: N is the jemalloc
binary without the guest kernel (no arenas, so the bare pipes are safe), G the
plain binary under the guest kernel (`GK_RUNNER=resident GK_RUNNER_PIN=other`,
no arenas), each a paired comparison of the relays forced off and on through
the temporary `WORKERD_GK_FORCE_BODY_RELAY` knob in the three relay predicates,
interleaved launch by launch; A is the shipping jemalloc binary under the guest
kernel with arenas, where the relays are on by themselves. One connection, four
launches per arm, four seconds per case, the server on `12-17,36-41` and the
client on `0-5,24-29`, the VMSCAPE mitigation reading `Mitigation: IBPB before
exit to userspace` and `halt_poll_ns` 200000 before and after each run
(`results/socket-{native,gk,arena}.json`).

A front worker connects to a back worker over its binding, or opens a
WebSocket to it, and inside one request does n round trips of a four-byte
message, answering the count: `/tunnel/<n>` writes a line and reads the echo n
times, `/ws/<n>` sends a message and awaits the echo n times, `/ws-burst/<n>`
sends all n before awaiting any echo, so that the relay's turns overlap the
sender's. There is no tunnel burst: a `kj` pipe has no buffer, so a caller that
writes n lines before reading an echo parks the callee's second echo, which
stops the callee reading, which parks the caller -- on the bare pipe without
the guest kernel just the same (the route hung at 10 s on the knob binary with
the relays off), so it is not the relay's. The WebSocket routes await the
callee's close reply before answering; the one that does not is a finding of
its own, below.

### 1. Per launch, relays off and on

p50 per launch in microseconds, the four launches of each arm in order, and
the paired difference launch by launch.

| N, one connection | off | on | on minus off, paired |
| --- | --- | --- | --- |
| tunnel, 1 round trip | 203.5 206.8 205.8 209.7 | 209.8 206.0 211.2 212.0 | +6.4 -0.8 +5.4 +2.3 |
| tunnel, 10 | 479.7 478.6 482.7 482.0 | 484.2 480.0 485.3 480.7 | +4.4 +1.3 +2.6 -1.3 |
| tunnel, 100 | 3151 3149 3126 3115 | 3116 3120 3114 3128 | -35 -29 -12 +14 |
| tunnel, 1000 | 29558 29298 29384 28912 | 29554 29343 29458 29529 | -4 +45 +74 +618 |
| ws, 1 | 153.8 152.9 154.0 159.9 | 165.5 161.0 159.1 158.8 | +11.7 +8.1 +5.0 -1.1 |
| ws, 10 | 267.6 262.5 268.5 272.7 | 283.7 279.0 275.6 275.9 | +16.1 +16.6 +7.1 +3.2 |
| ws, 100 | 1342 1321 1333 1334 | 1391 1372 1359 1376 | +49 +52 +26 +42 |
| ws, 1000 | 11850 11675 11818 11719 | 12303 12114 12021 12151 | +454 +440 +203 +432 |
| ws burst, 100 | 1016 990 1004 1019 | 1052 1030 1035 1023 | +36 +40 +31 +4 |
| ws burst, 1000 | 8425 8344 8357 8418 | 8760 8624 8629 8586 | +335 +280 +272 +168 |

| G, one connection | off | on | on minus off, paired |
| --- | --- | --- | --- |
| tunnel, 1 round trip | 396.8 406.9 403.2 393.9 | 400.3 407.4 404.2 393.6 | +3.5 +0.5 +1.0 -0.3 |
| tunnel, 10 | 934.3 945.6 943.2 934.8 | 951.6 951.7 940.4 936.8 | +17.3 +6.1 -2.8 +2.0 |
| tunnel, 100 | 6491 6438 6556 6449 | 6539 6593 6483 6412 | +48 +155 -73 -37 |
| tunnel, 1000 | 61298 62406 61145 62355 | 62111 61849 61764 61390 | +813 -556 +619 -965 |
| ws, 1 | 178.7 177.4 178.1 179.3 | 179.7 183.6 182.4 182.9 | +1.0 +6.2 +4.3 +3.6 |
| ws, 10 | 470.7 469.1 470.5 474.8 | 473.9 477.9 476.3 477.8 | +3.2 +8.8 +5.8 +3.0 |
| ws, 100 | 3412 3464 3467 3473 | 3493 3517 3486 3513 | +81 +53 +19 +39 |
| ws, 1000 | 33089 33078 33160 33255 | 33558 33544 33309 33599 | +469 +467 +149 +344 |
| ws burst, 100 | 2522 2526 2528 2544 | 2586 2586 2584 2594 | +64 +60 +56 +50 |
| ws burst, 1000 | 24179 24376 24419 24273 | 24661 24699 24614 24810 | +482 +323 +195 +536 |

No launch of any arm had an error.

**A WebSocket message costs the relay 0.2 to 0.5 us, a tunnel round trip
nothing the launches can tell apart.** A thousand WebSocket round trips cost
+203 to +454 us in N (1.7 to 3.9 per cent of 11.8 ms) and +149 to +469 in G
(0.4 to 1.4 per cent of 33 ms), with two relayed receives per round trip, so
one relayed receive is 0.1 to 0.25 us and ten thousand small messages are 2 to
5 ms on top of 120 to 330 ms. The burst, where the relay's turns overlap the
sender's, costs the same per message or less. The tunnel's thousand round
trips are -4 to +74 us in N with one launch at +618 (0.2 per cent of 29 ms)
and within +-1 ms either way in G, where 61 ms is the base: a tunnel round trip
already costs 29 us without the guest kernel and 61 under it, in the JS stream
machinery on both sides and the two guest turns, against which an extra
event-loop turn per read does not show. The reason a message costs so much
less than the body relay's "about a microsecond per crossing" is that that
figure is a whole request's worth of relay -- the staging buffer, the extra
turn, the lost synchronous fast path on a reader that would otherwise have had
one -- where a socket read or a WebSocket receive was already asynchronous and
already a turn of its own.

**There is no safe synchronous fast path to add.** `tryReadSync()` through the
relay would run in the reader's turn against the pipe's state object, which is
the writer's `BlockedWrite` whenever the writer arrived first, and the type of
that object is known only by dereferencing it; even EOF, the one answer that
needs none of the writer's bytes, is a `ShutdownedWrite` the writer's turn
allocated. A read-ahead the loop fills before the reader asks would serve a
synchronous read from host memory, at the cost of one chunk of flow control; at
0.1 to 0.25 us a receive there is nothing for it to buy.

### 2. The shipping configuration

The shipping jemalloc binary under the guest kernel with arenas, the relays on
by themselves, four launches, p50 per launch in microseconds and the server's
CPU time per request from the launch's median:

| A, one connection | p50 per launch | CPU us per request |
| --- | --- | --- |
| tunnel, 1 round trip | 442 435 463 448 | 1011 958 1103 1077 |
| tunnel, 10 | 1082 1069 1107 1081 | 4085 4479 5883 4912 |
| tunnel, 100 | 7662 7493 7799 7670 | 43721 32085 38326 31747 |
| tunnel, 1000 | 73190 72518 74185 72759 | 400909 375833 384091 297931 |
| ws, 1 | 235.7 232.6 233.8 231.4 | 573 588 592 598 |
| ws, 10 | 588.5 585.7 595.1 586.7 | 1382 1407 1409 1344 |
| ws, 100 | 4186 4169 4226 4137 | 9317 9320 9441 9209 |
| ws, 1000 | 39625 39558 39993 39539 | 87677 87000 88081 87778 |
| ws burst, 100 | 3116 3092 3142 3107 | 7088 7096 7258 7039 |
| ws burst, 1000 | 29420 29148 29564 29316 | 65672 65333 66591 66165 |

No launch had an error, and no launch faulted or condemned an isolate. A
WebSocket round trip across a binding costs 40 us here against 33 in G and 12
in N, a tunnel round trip 73 against 61 and 29; the step from G to A is the
arenas (jemalloc with its thread cache off, the per-isolate placement, the
relays), and the relays' share of it is what section 1 bounds at 0.1 to 0.25
us a receive. The tunnel's CPU time per request is several times its latency
at every n: the socket's two JS stream machineries spin the runner's vCPUs
while the request waits on the loop, which the body relay's measurement saw
as well and which is not the relay's.

### 3. Found on the way: a close that is not awaited

The first run of arm A condemned the back worker on its first WebSocket
route, with the relays in place and the message traffic intact: the route
answered its count and then the callee's turn faulted writing into the
caller's arena, at `kj::Event::disarm()` inlined into
`kj::_::CoroutineBase::~CoroutineBase()` (`mov %rdx,(%rcx)`: `*prev = next`),
destroying the adapter pipe's `close()` coroutine under
`LegacyWebSocketAdapter::pump (.destroy)`. The route's caller sent `close()`
and returned at once, without awaiting the callee's close reply, so its
request ended while the callee's close was still queued on the event loop,
armed by the loop when the caller's relayed receive took it; the callee's turn
then cancelled its pump, destroyed that queued event, and unlinking it wrote
the link field of its queue neighbour, an event of the caller's. That is the
event-disarm hazard README.md's heap-arena section records as not covered --
a turn that destroys one of its own events queued before the turn began --
reached by a chatty close. It reproduces six times in six on `/ws-leave/100`
(three under the resident runner, three under the default) and once on
`/ws-leave/1000`, and the same routes awaiting the reply are clean five times
in five; `GK_WS_LEAVE=1 socketrelay.py` keeps the reproducer. A step added to
`binding-crossing-test.sh` with the same shape -- a hundred echoed messages,
then `close()` and return -- did not reproduce it: that step awaited each
echo in turn, which drains the queue so nothing of the callee's is left
armed at teardown. Bursting the sends -- all of them before any reply, then
`close()` and return -- reproduces it from that script's own configuration,
compatibility flags and `WorkerEntrypoint` callee, so none of those bear on
it; the serial echo was the difference. No relay can close it: whatever
node a turn destroys, its queue neighbours are whoever armed next to it on
the loop. It needs the loop-level treatment the README leaves open, which is
a design decision.

It is worse than a condemnation. The turn is abandoned inside the destructor,
with the event half unlinked and the coroutine frame half destroyed but still
on the loop's queue, and when the loop reaches it the call through its vtable
aborts the process: `libc++abi: Pure virtual function called!`, signal 6,
exit status 1, after `IoContext` has logged the condemnation twice. Six runs
in six. The first run of arm A saw this as a connection reset in the harness.
A caller that closes a binding's WebSocket and does not await the reply
therefore takes the whole workerd down under heap arenas today.

## Collecting once for reported external memory

### The question

"Whether the soft-limit collection belongs in every build" above settled that
the collection stays, and named the lever if its cost ever needed reducing:
`ExternalMemoryTarget::applyLocked` (`jsg/jsg.c++`) asked for it with
`MemoryPressureNotification(kCritical)`, which is the one level V8 acts on
without incremental marking but also its near-OOM path, and a V8 patch
exposing a plain collection "would halve the cost measured here and leave the
caches alone". That is what this section does and measures. The doubt worth
settling first was whether the plain collection is enough: the second
collection `kCritical` runs was counted there, not explained, and if it were
the one that frees the reported memory then a single collection would not
bound the heap arena region and the trade would be a different one. It is not;
the region is held to the same 100 MiB, to the megabyte, by one collection.

### What kCritical does in this V8, read from the source

`Isolate::MemoryPressureNotification(kCritical)` (`src/api/api.cc`) decides
whether the calling thread holds the isolate and hands off to
`Heap::MemoryPressureNotification` (`src/heap/heap.cc`), which stores the level
and, because the stored level is `kNone` whenever nobody is inside a
notification, calls `CheckMemoryPressure()` on the spot. That does three
things, in order:

1. `FreeCachesOnMemoryPressure(isolate, kMemoryPressure)`:
   `AbortConcurrentOptimization(kDontBlock)`, `ClearSerializerData()`,
   `compilation_cache()->Clear()`,
   `IsolateGroup::current()->memory_pool()->ReleaseAllImmediately()` and,
   because `--flush-liftoff-code` defaults on,
   `wasm::GetWasmEngine()->FlushLiftoffCode()`. `ReleaseAllImmediately`
   (`src/heap/memory-pool.cc`) releases the local page and zone pools of
   *every* isolate in the group and the shared pools besides, which in a
   shared-heap workerd is the process; the per-isolate form,
   `ReleaseImmediately(isolate)`, is what the reason `kLastResort` or
   `kMemoryPressure` turns into the group-wide one.
2. It resets the stored level to `kNone` -- so that a collection's finalizers
   cannot recurse through it -- and calls `CollectGarbageOnMemoryPressure()`,
   which collects: `CollectAllGarbage(GCFlag::kReduceMemoryFootprint,
   kMemoryPressure, kGCCallbackFlagCollectAllAvailableGarbage)` followed by
   `CompleteArrayBufferSweeping()`.
3. It estimates `potential_garbage = (CommittedMemory() - SizeOfObjects()) +
   external_memory()` and, if that is at least 8 MB *and* at least a tenth of
   `CommittedMemory()`, collects a second time with the same flags (or, had the
   first taken over 50 ms, would have started incremental marking, which
   workerd has off).

So the second collection is not a consequence of the first having failed to
free anything, and it is not driven by the reported memory alone. Of the two
terms in the estimate, `CommittedMemory() - SizeOfObjects()` is a nearly empty
but committed heap -- for `back` a 0.2 MB live heap in a megabyte of old space
plus a committed new space -- and `external_memory()` is what remains reported
after the collection, which is the part that is still live. Neither is
something a further collection can reduce, so the estimate clears the
threshold on every trigger and the second collection always runs. The trace
shows it finding nothing: `0.4 (0.8) -> 0.2 (0.5) MB` for the first of a pair
and `0.2 (0.5) -> 0.2 (0.5) MB` for the second.

`GCFlag::kReduceMemoryFootprint` is a cost of its own inside the collection,
separate from the caches: the mark-compact releases this isolate's pooled
chunks and its finished wasm stacks (`heap.cc`), the sweeper discards unused
system pages page by page (`sweeper.cc`), and compaction switches to the
fragmentation target and evacuation budget meant for shrinking. None of that
is needed to free memory the embedder holds in `kj`, which a major collection
frees through its weak callbacks.

### The patch

`patches/v8/0052` adds `Isolate::CollectGarbageForExternalMemory()`:
`CollectAllGarbage(GCFlag::kNoFlags,
GarbageCollectionReason::kExternalMemoryPressure)`, one full mark-compact and
nothing else -- no cache or pool drops, no second collection, and not
memory-reducing. `applyLocked` calls it in place of the pressure notification.
The reason is V8's own `kExternalMemoryPressure`, which the trace prints as
"external memory pressure" so the collections this asks for are told apart
from any other in a `--trace-gc` log.

### Method

The arms are `critical` (the pressure notification, as it was), `plain` (the
patch, as committed) and `off` (the report left in place, no collection),
selected per launch from one binary by a never-committed
`WORKERD_GK_MEASURE_EXTERNAL_GC` knob in `applyLocked` (`measure/extgc/cost.sh`
describes it), so that no build difference can enter a delta. Otherwise the
method is the section above's: the heap-region workload of 3,050 responses of
4 MiB, 12 GB after the warm-up, sampled through the `__gk_heap_region` probe
after each burst of 250 (`measure/extgc/region.sh`); and the `cost-*.js`
workers under oha at one connection (eight for the second `hello` row), six
seconds after two of warm-up, workerd on CPUs 12-17,36-41 and oha on
0-5,24-29, the arms interleaved round by round with a fresh launch each, six
launches per arm. The production-like cell is the default build (tcmalloc,
guest kernel off) with `--max-old-space-size=128`; the arena cell is the
jemalloc build with the guest kernel on and no limit. GC counts and pauses are
from one further launch per cell with `--trace-gc` and one worker per launch
(`measure/extgc/gcsum.py`).

A second never-committed knob, `WORKERD_GK_MEASURE_LINEBUF`, line-buffered
`stdout` for the traced launches. The binary measured here gave `stdout` a
fully buffered `BUFSIZ` buffer and nothing flushed it, so without the knob the
last 8 KiB of a `--trace-gc` log -- 33 lines of it -- did not reach the file;
the counts in the section above, measured without the knob, are short by up to
that much. `setup.c++` line-buffers `stdout` in every build now, so the knob
has no successor; see "A `--trace-gc` log that reaches the file".

The VMSCAPE mitigation read `Mitigation: IBPB before exit to userspace` and
`kvm.halt_poll_ns` 200000 before and after every run.

### 1. The region, and that one collection bounds it

`back`'s heap arena region in MiB, and the external memory reported for it,
after the warm-up and each of the twelve bursts (`extgc3-region-*.txt`):

| arm              | field  | warm | 1   | 2    | 3    | 4    | 5    | 6    | 7    | 8    | 9    | 10   | 11   | 12   |
| ---------------- | ------ | ---- | --- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| critical         | region | 100  | 100 | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  |
| critical         | ext    | 72   | 56  | 40   | 24   | 8    | 60   | 44   | 28   | 12   | 64   | 48   | 32   | 16   |
| plain            | region | 100  | 100 | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  |
| plain            | ext    | 72   | 56  | 40   | 24   | 8    | 60   | 44   | 28   | 12   | 64   | 48   | 32   | 16   |
| plain, 128 MiB   | region | 100  | 100 | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  | 100  |
| off              | region | 272  | 968 | 1800 | 1800 | 2440 | 3208 | 3208 | 3208 | 3528 | 4168 | 4168 | 4168 | 4168 |

The `critical` and `plain` arms are the same run to the megabyte, in the region
and in the sawtooth of reported memory between 8 and 72 MiB that the 64 MiB
soft limit draws; the arm given `--max-old-space-size=128` is the same again.
The `off` arm is the growth the collection exists to stop. What differs is the
collecting: over the same 12 GB, `back` runs

| arm      | mark-compacts | pause total | mean    | max     | scavenges |
| -------- | ------------- | ----------- | ------- | ------- | --------- |
| critical | 360           | 276.2 ms    | 0.77 ms | 3.64 ms | 192       |
| plain    | 180           | 149.2 ms    | 0.83 ms | 3.70 ms | 203       |
| off      | 4             | 8.0 ms      | 2.01 ms | 2.44 ms | 204       |

Exactly two collections per trigger against exactly one, for the same bound.
The mean rises because what the pair's second collection does is nothing, so
it averages down; the work removed is a whole collection, not a cheap tail.

### 2. What it costs, per launch

Requests per second, six launches of each arm in round order. Default build,
`--max-old-space-size=128` (`extgc3-cost-default-limit128.txt`):

| workload     | critical                      | plain                         | off                           |
| ------------ | ----------------------------- | ----------------------------- | ----------------------------- |
| hello, c=1   | 22926 23099 22836 22148 22338 22963 | 22837 22711 22795 23009 22719 22876 | 23050 22914 22609 23239 23329 23271 |
| hello, c=8   | 29003 28754 29014 29218 28686 29055 | 28987 28871 28943 28668 28966 28725 | 28833 28696 29260 28906 28712 28786 |
| bigheap, c=1 | 22016 21979 22045 21444 21521 21718 | 21849 21922 21583 21875 21580 21975 | 22086 21852 21806 22209 22138 22199 |
| bigbody, c=1 | 1253 1260 1269 1272 1252 1277 | 1291 1294 1299 1291 1283 1271 | 1000 1018 992 1013 1016 1007  |
| bigheapbody  | 353 339 347 348 351 348       | 562 560 569 584 565 569       | 954 889 890 920 947 922       |

jemalloc build, guest kernel on, no limit
(`extgc3-cost-jemalloc-gk-nolimit.txt`):

| workload     | critical                      | plain                         | off                           |
| ------------ | ----------------------------- | ----------------------------- | ----------------------------- |
| hello, c=1   | 5932 5927 5824 5859 5737 5940 | 5925 5811 5998 5839 5923 5859 | 5982 5840 6016 5909 5880 5769 |
| hello, c=8   | 7955 8022 8308 8239 7255 8013 | 8122 7743 7954 7535 8976 8165 | 7764 7897 7326 7847 7581 7920 |
| bigheap, c=1 | 6072 5977 5954 5929 6040 6076 | 6074 6047 5848 5906 6000 5982 | 6057 6088 5944 5999 5940 5825 |
| bigbody, c=1 | 1053 1097 1047 1043 1054 1040 | 1087 1095 1094 1086 1093 1083 | 920 886 916 886 899 916       |
| bigheapbody  | 419 411 413 415 409 424       | 577 590 596 615 587 598       | 515 658 869 770 770 644       |

p99 in microseconds, same launches, the two rows where the collection fires:

| cell, workload          | critical                            | plain                               | off                           |
| ----------------------- | ----------------------------------- | ----------------------------------- | ----------------------------- |
| default, bigbody        | 1547 1434 1420 1431 1432 1406       | 1197 1227 1191 1225 1216 1246       | 1508 1502 1512 1496 1511 1522 |
| default, bigheapbody    | 39640 40363 40497 40183 39788 39606 | 20646 20519 20063 17889 19824 20752 | 1390 1509 1498 1500 1395 1520 |
| jemalloc, bigbody       | 2114 2069 2129 2142 2116 2175       | 1545 1521 1524 1529 1524 1558       | 2247 2358 2247 2377 2338 2259 |
| jemalloc, bigheapbody   | 30674 30437 30555 30349 30729 27658 | 18626 17908 17790 17610 18213 17717 | 4834 3422 2486 2601 2632 2569 |

Resident memory at the end of each six-second window, GB: `bigbody` 1.0-1.2 /
1.0 / 5.9-6.6 in the default build and 0.9-1.0 / 0.9-1.0 / 7.2-7.3 under the
arenas; `bigheapbody` 1.1-1.3 / 1.1 / 9.6-15.7 and 1.0-1.1 / 1.1-1.2 /
14.8-18.6, critical, plain and off in that order.

Collections in the traced launch, one worker per launch
(`extgc3-cost-trace1-*.txt`):

| cell, workload        | arm      | mark-compacts | pause total | mean     | max      |
| --------------------- | -------- | ------------- | ----------- | -------- | -------- |
| default, bigbody      | critical | 1176          | 333.6 ms    | 0.28 ms  | 1.32 ms  |
| default, bigbody      | plain    | 588           | 203.8 ms    | 0.35 ms  | 0.52 ms  |
| default, bigheapbody  | critical | 328           | 5537.0 ms   | 16.88 ms | 38.34 ms |
| default, bigheapbody  | plain    | 269           | 4135.5 ms   | 15.37 ms | 20.54 ms |
| jemalloc, bigbody     | critical | 967           | 494.6 ms    | 0.51 ms  | 3.72 ms  |
| jemalloc, bigbody     | plain    | 503           | 255.3 ms    | 0.51 ms  | 3.80 ms  |
| jemalloc, bigheapbody | critical | 390           | 4692.3 ms   | 12.03 ms | 42.94 ms |
| jemalloc, bigheapbody | plain    | 280           | 3372.5 ms   | 12.04 ms | 26.17 ms |

The `off` arm's traced launches collect 0, 0, 7 and 2 times in the same
windows. The counts are not in the ratio 2:1 wherever the throughput differs,
because the arm that serves more responses in the same six seconds crosses the
soft limit more often; per trigger it is two collections against one
everywhere.

What the rows say:

- **The three workloads that paid nothing still pay nothing.** `hello` at one
  and eight connections and `bigheap` never cross the soft limit; their six
  launches per arm overlap entirely in both cells, as they did before.
- **Where the collection was already a gain, it is a slightly larger one.**
  `bigbody` goes from 1253-1277 to 1271-1299 requests per second in the
  default build and from 1040-1097 to 1083-1095 under the arenas -- 2 and 3%,
  at the edge of the spreads -- and its p99 from 1406-1547 to 1191-1246 us and
  from 2069-2175 to 1521-1558 us, which is not. Half the collections at the
  same per-collection cost: 1176 to 588 and 967 to 503 mark-compacts.
- **Where it was the cost, about three fifths of the cost goes.** With 42.5 MB
  live, `bigheapbody` goes from 339-353 to 560-584 requests per second in the
  default build, a factor of 1.63, and from 409-424 to 577-615 under the
  arenas, a factor of 1.43. Measured against the `off` arm, which is what the
  collection is being charged for, the penalty falls from 2.65x to 1.62x and
  from 1.70x to 1.19x. p99 halves, 40 ms to 20 and 30 ms to 18, and the
  longest pause in the trace with it, 38.3 ms to 20.5 and 42.9 to 26.2. The
  per-collection mean barely moves (16.88 to 15.37 ms, 12.03 to 12.04), so
  what the pair's second collection costs is a whole collection's work for
  nothing.
- **The bound is untouched.** `heap-region-test.sh` passes on the committed
  jemalloc build with `back`'s region growing 0 MiB over 12 GB served and
  `front`'s 13 MiB, the same figures as the binary before the change.

### The verdict

Landed. The collection stays unconditional in every build with the same 64 MiB
soft limit; what changes is that it is one plain mark-compact instead of V8's
near-OOM sequence. Nothing measured here is worse, the region is held to the
same 100 MiB to the megabyte, and the worst case this was aimed at -- a large
live heap and large bodies together -- keeps 1.63x of the 2.65x it was paying.

This changes when and how much garbage is collected in every build, not only
the arena one, exactly as the report and the collection themselves did; GC
timing is not part of the API surface a worker can observe, and the change only
removes work, so it takes no compatibility flag. What it does not remove is the
rest of the cost: a mark-compact of the whole live heap per 64 MiB of bodies is
still proportional to the heap, so the policy lever the section above named --
a soft limit that grows with the live heap -- is where any further reduction
has to come from, and is still left to whoever has the production numbers.

### Found on the way

**V8 drops the same caches once per isolate at startup, for `kModerate`.**
`worker.c++` tells a new isolate's heap that there is moderate memory pressure,
on the reasoning that a machine serving thousands of tenants always has some.
`CheckMemoryPressure` tests `HighMemoryPressure()`, which is true of any level
but `kNone`, before it looks at which level it is, so `kModerate` runs
`FreeCachesOnMemoryPressure` in full -- compilation cache, serializer data,
concurrent optimization jobs, the group's page and zone pools, Liftoff code --
and only then finds that there is nothing for `kModerate` to do without
incremental marking. It is once per isolate rather than once per 64 MiB of
bodies, so it is not what this section is about, but an isolate created while
others are serving empties their page pools as it starts.

### Caveats

- **Whether a plain collection is cheaper than `kCritical`'s first one was not
  separated.** A `critical` trigger's pair costs 33.8 ms at 42.5 MB live
  against 15.37 ms for one plain collection, so the second collection is
  roughly as expensive as the first; how much of the remaining 3 ms is
  `kReduceMemoryFootprint`'s page discarding and compaction budget rather than
  measurement spread is not known, because the trace does not label the two
  collections of a pair.
- **The pooled pages a group-wide drop would cost were not measured.** Every
  trace line in these runs reads `pooled: 0.0 MB`: one worker per isolate
  serving one shape of request keeps no pooled pages, so
  `ReleaseAllImmediately` had nothing to release and the process-wide drop that
  `kCritical` performs cost nothing here. The saving it represents is an
  argument from the source, not a figure.
- **The large heap was still 42.5 MB, not 128.** As before, what a worker at
  the production limit would pay is extrapolated from the pause's
  proportionality to live size, not measured.
- **Six seconds and one connection**, as before: the `off` arm's deferred
  collections do not appear within the window, and the cost is a mutator-side
  pause that concurrency would spread without changing.
- **The `off` arm of `bigheapbody` under the arenas is spread 515 to 869**
  requests per second across its six launches, the widest spread in either
  table. Nothing here depends on its mean, but a three-launch median of it
  would have been worth little.

## A `--trace-gc` log that reaches the file

### The question

Three sections above read GC counts out of `--trace-gc` logs, and the last of
them needed a knob that was never committed to get them whole. Without it the
end of every traced run was missing, which is how a three-burst run that was
plainly collecting came to show no mark-compact at all. What is lost, why, and
what the counts already measured are worth.

### What loses it, read from the source

Two things have to hold at once. The first is that `stdout` is fully buffered,
which a pipe or a file is by the C library's default; the arena builds also made
it so explicitly, because `setup.c++` has to hand stdio a buffer before a turn
can allocate one inside an isolate's arena (the section above). The second is
that nothing flushes it. `kj::TopLevelProcessContext::exit()` (`kj/main.c++`)
calls `_exit()`, so a workerd that drains on SIGTERM and returns from its main
function never runs stdio's exit-time flush either; nor does a process that dies
on a fault, which several of the probes here arrange on purpose. A flush on the
signal path would therefore cover one of the three ways a traced run ends.

Line buffering covers all three, and what it costs is a `write(2)` per line on a
stream that carries nothing in volume: a worker's own output reaches fd 1
through the JSON logger, which writes to the descriptor directly, and the rest
of workerd's logging is `KJ_LOG` to stderr, so V8's trace flags are all that
stdio writes to `stdout` at all. `setup.c++` line-buffers it, for every build
rather than only the arena ones, and still hands stdio the buffer to use.

### Method

The heap-region `bigbody` worker under the `--trace-gc` config
(`measure/extgc/cost-bigbody.js`, `measure/extgc/cost-nolimit-trace.capnp`), a
fixed 4,000 responses at one connection so that every launch collects the same
number of times, then SIGTERM and, if that has not ended it in ten seconds,
SIGKILL. `stdout` and `stderr` to separate files, so that the `stdout` file is
exactly what stdio wrote. Three launches per cell, workerd on CPUs 12-17,36-41
and oha on 0-5,24-29. Two binaries built from this tree, before the change and
after, each in the default (tcmalloc) and the jemalloc-plus-guest-kernel
configuration. The VMSCAPE mitigation read `Mitigation: IBPB before exit to
userspace` and `kvm.halt_poll_ns` 200000 before and after.

### 1. The tail, per launch

Identical to the byte across the three launches of each cell:

| binary | build         | `stdout` bytes    | newlines | `Mark-Compact` lines | the file ends in |
| ------ | ------------- | ----------------- | -------- | -------------------- | ---------------- |
| before | default       | 45056 = 11 x 4096 | 223      | 223                  | `[2424578:0`     |
| before | jemalloc + gk | 40960 = 5 x 8192  | 202      | 203                  | `...) extern`    |
| after  | default       | 47470             | 235      | 235                  | a complete line  |
| after  | jemalloc + gk | 47470             | 235      | 235                  | a complete line  |

Both `before` files end at an exact multiple of the stdio buffer in use and in
the middle of a line, which is the signature of a buffer nobody emptied: the C
library sizes a buffer of its own from the file's block size, 4096 here, and the
`setvbuf()` call gave the arena builds `BUFSIZ`, 8192. What is missing is
whatever was in the buffer when the process ended -- 2,414 bytes and 12 lines in
the default build, 6,510 bytes and 33 lines under the arenas. All six launches
ended on the SIGTERM, so nothing killed the process before it could have
flushed; there is simply no flush. The `after` files carry all 235 collections
the workload performs, in both builds and in every launch.

**The buffer workerd supplies is not what loses the tail; it decides how much is
lost.** The default build, which never called `setvbuf()`, loses 4 KiB of trace
to the C library's own buffer. The arena builds' `BUFSIZ` buffer doubled that.
So a traced run was being cut before the arenas existed, and building without
the guest kernel is not a way around it.

### 2. The arena fault did not come back

The traced launches above ran under the guest kernel with the trace lines
written from inside turns, which is the shape that found the fault: no
unmappable fault, no condemned isolate and no failed request in any of them. The
probes pass on both binaries built for this change -- 23 of 23 on the default
build and 22 of 23 on the jemalloc one, whose failure is
`memory-cache-fault-test.sh` at step 2, the one this branch leaves deliberately
red, unchanged -- among them `fault-leak-test.sh` and `atexit-arena-test.sh`,
the two that read thread-wide state a faulted turn leaves behind.

### Where the earlier counts stand

- **"Collecting once for reported external memory" is exact.** Its traced
  launches used the knob, and its counts are what they say.
- **"Whether the soft-limit collection belongs in every build" is low by up to
  one buffer** -- 33 lines in the cell measured here -- in every count its trace
  gives: the four mark-compacts of the `off` arm, the 359 and 360 of the `on`
  arm, the 1,161 of `bigbody` and the 330 and 402 of `bigheapbody`. Its method
  and its caveats now say so. Where an arm of it survives in the tree, the
  section after it re-measured the same cells with the tail arriving, and those
  are the counts to quote.
- **One figure the loss broke rather than shortened** is `bigbody`'s `off` arm,
  reported as collecting "none at all in 6,050 responses". That whole trace fits
  inside one buffer, so the figure was the buffer and not the run: re-measured,
  the `off` arm's traced launches collect 0, 0, 7 and 2 times.

### The verdict

Landed. `stdout` is line-buffered in every build, where a non-tty `stdout` was
fully buffered before, and stdio is still handed the buffer it uses, which is
what keeps it from allocating one in an isolate's arena. This changes the
timing of diagnostic writes in builds without the guest kernel as well as with
it. Nothing a worker can observe depends on it -- worker output does not pass
through stdio -- so it takes no compatibility flag.

### Caveats

- **A line is only as whole as one `write(2)` makes it.** Line buffering is
  enough for two isolates' trace lines not to interleave with each other, but
  the JSON logger writes to fd 1 outside stdio and nothing orders it against
  them.
- **A line in progress when the process dies is still lost.** What is bounded
  now is one line instead of one buffer, and SIGKILL is no worse than that.
- **The cost of a write per line was not measured.** Nothing in this tree
  writes to `stdout` through stdio in volume, which is a grep and not a figure;
  a V8 flag that traced per allocation would pay a syscall a line.
- **Only the SIGTERM path was exercised.** That a crash loses the buffer too
  follows from `_exit()` having no flush, and was not measured.
