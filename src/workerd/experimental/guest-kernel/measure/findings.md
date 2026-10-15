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
code running slower inside the guest: a stall profile, which is what nested
paging does to a workload whose page walks now cost two dimensions.

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

**The decision this supports.** The guest kernel is worth continuing only for a
fleet whose tenants are nearly idle and whose binding constraint is memory
rather than CPU, and only if the per-turn stall cost can be brought down. It is
not worth continuing on the strength of the dispatch argument, which is the
argument the design is built around, because that argument measures backwards.
This does not reduce to "the guest kernel is better" or "processes win": the
guest kernel wins on memory, loses on dispatch and on CPU, and roughly ties on
cold start.

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

1. **Attribute the stall.** The IPC collapse points at nested paging. The
   parked `guest-kernel-huge-pages` branch is the direct test, and its own note
   -- that the benchmarked workloads were not TLB-bound -- was reached on
   different hardware and a build whose baseline was twenty times slower, which
   is exactly the condition that would hide a stall. Re-run it against an
   optimized build with `GK_HUGE_COLLAPSE`, measuring cycles, instructions and
   page-walk counters. If huge pages on both levels recover most of the 72%
   cycle increase, the CPU argument changes and with it the answer.
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
