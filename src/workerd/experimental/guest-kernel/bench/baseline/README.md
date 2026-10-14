# Baseline measurements of stock workerd

What a request costs in unmodified workerd, measured before any guest-kernel
work existed. These numbers decided whether the design was worth building: a
forwarded syscall costs a VM exit (`../kvm-micro/m5_exitcost.c`), so the
viability question was how many syscalls and isolate locks a request actually
performs.

Everything here runs against a normal `workerd` binary with no guest-kernel
involvement, which is why it doubles as the control arm for the comparisons in
`../gk-bench`.

## Workers

`config.capnp` serves three workers, each on its own socket:

| Worker | Port | What it does |
| --- | --- | --- |
| `hello` | 18080 | returns a fixed string; the floor on per-request cost |
| `fetcher` | 18081 | two subrequests to `hello` through a service binding, bodies read in full |
| `cpu` | 18082 | about 1 ms of JSON and string work |

## Scripts

Each takes the path to a `workerd` binary and a request count, and drives
requests over one keep-alive connection (`client.py`).

| Script | Measures |
| --- | --- |
| `native.sh` | requests per second per worker, untraced |
| `cputime.sh` | CPU microseconds (user+sys, all threads) per request, from `/proc/<pid>/stat` |
| `run.sh` | the syscall mix per worker, via `strace -f -c`, after a warm-up run so JIT and caches are hot |
| `locks.sh` | `v8::Locker::Initialize` calls per request, and how many re-lock the isolate the thread locked last, via `bpftrace` (needs root) |

```
./native.sh  /path/to/workerd 5000
./cputime.sh /path/to/workerd 5000
./run.sh     /path/to/workerd 2000
./locks.sh   /path/to/workerd 2000
```

`syscall-cost.c` is standalone and takes no workerd: it times a trivial
syscall, a vDSO call and a `pkey_set` pair on the current machine, in both
nanoseconds and TSC cycles.

```
cc -O2 -o syscall-cost syscall-cost.c && ./syscall-cost
```

Both `run.sh` and `locks.sh` warm each worker with 500 requests before
measuring, because the first requests into a fresh isolate are dominated by
parsing and JIT rather than by steady-state work.
