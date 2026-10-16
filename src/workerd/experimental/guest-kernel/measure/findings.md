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
parked loop walks, the NMI's window); a repeat of the one-connection hello
latency on the final build gave 27.1 to 27.2 us spinning and 30.2 to 30.4 us
sleeping, against 25.8 to 26.7 us with the guest kernel off and 139.5 us
with it on. A one-connection figure moves by 10 to 20 us with where the
scheduler puts the client relative to the server's two pinned CPUs (the
same sleeping arrangement measures 30 us in most launches and 49 us in
some; with the client pinned to a far CPU, 37 to 40 us in every launch),
which every arrangement is subject to alike and the interleaving averages
out.

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
- requests per second: the guest kernel costs **3.4 times** the CPU per
  request, and 59 times the aggregate rate when confined to one process.
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
between turns sleeping, and the crossover computed above moves
accordingly: it has to be recomputed with the sleeping arrangement's CPU
per request for a realistic worker mix, which this document does not
have. The dispatch argument the design is built around is therefore not
contradicted after all, once the turn stays inside KVM_RUN; what remains
open is the CPU per request of the sleeping arrangement, and the
realistic worker mix that decides both axes.

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
  underestimates for every arrangement; the comparisons are unaffected because
  the client load is proportional to the work served.
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
   KVM's 200 us halt-poll budget. `KVM_CAP_HALT_POLL` sets that budget per
   VM, from the process, so the trade between the poll's CPU and a
   scheduler wakeup's latency (the runner handoff's 9 us) can be made per
   deployment without touching the host; it was not measured here. The
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
