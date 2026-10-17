#!/usr/bin/env python3
# CPU time per request, and the VM exits that go with it.
#
# Latency at one connection says how long a request takes; this says what it
# costs, which is what decides how many requests a core serves. The server's
# utime+stime, which covers its background threads as well as the event loop,
# is read either side of a load run and divided by the requests that run
# completed, so no rate has to be estimated. Several rounds, because the
# background compiler and collector do not do the same work every time.
#
# Usage: cputime.py <shared|gk|procs> <out.json> [rounds]

import os
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

WORK = os.path.join(H.RUN_DIR, "cputime")
BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")
DUR = os.environ.get("GKM_DUR", "15")
HZ = os.sysconf("SC_CLK_TCK")


def cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(") ", 1)[1].split()
    return int(parts[11]) + int(parts[12])


def count_exits(pid, seconds):
    """VM exits over a window, from perf, or None where it is refused."""
    try:
        r = subprocess.run(
            ["sudo", "-n", "perf", "stat", "-e", "kvm:kvm_exit", "-p", str(pid),
             "--", "sleep", str(seconds)],
            capture_output=True, text=True, timeout=seconds + 60,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in r.stderr.splitlines():
        if "kvm:kvm_exit" in line:
            tok = line.strip().split()[0].replace(",", "").replace(".", "")
            if tok.isdigit():
                return int(tok)
    return None


def one_round(pid, port, path, gk):
    t0 = cpu_ticks(pid)
    b = subprocess.Popen(
        [BENCH, "127.0.0.1", str(port), path, "4", DUR, "0"],
        stdout=subprocess.PIPE, text=True,
    )
    exits = None
    if gk:
        time.sleep(2.0)
        e0 = cpu_ticks(pid)
        exits = count_exits(pid, 8)
        e1 = cpu_ticks(pid)
    fields = b.communicate()[0].split()
    t1 = cpu_ticks(pid)
    total, rps = int(fields[0]), float(fields[5])
    out = {
        "cpu_us_per_req": (t1 - t0) / HZ * 1e6 / total,
        "rps": rps,
        "p50_us": float(fields[1]),
        "total": total,
    }
    if exits is not None:
        # The exit window's own requests, from the CPU it used at the rate the
        # whole run saw. Close enough to turn exits into exits per request.
        out["kvm_exits_per_req"] = exits / max(1.0, rps * 8.0)
        out["exit_window_cpu_frac"] = (e1 - e0) / HZ / 8.0
    return out


def main():
    mode, out = sys.argv[1], sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    with open(os.path.join(WORK, "t.js"), "w") as f:
        f.write(H.tenant_module(0))
    port = H.PORT_BASE
    cfg = os.path.join(WORK, "c.capnp")
    with open(cfg, "w") as f:
        f.write(
            'using Workerd = import "/workerd/workerd.capnp";\n'
            "const config :Workerd.Config = (\n"
            '  services = [(name = "w", worker = (modules = [(name = "worker", esModule = embed "t.js")],\n'
            '    compatibilityDate = "%s"))],\n'
            '  sockets = [(name = "s", address = "127.0.0.1:%d", http = (), service = "w")]\n);\n'
            % (H.COMPAT_DATE, port)
        )
    gk = mode == "gk"
    p = H.launch(cfg, gk=gk, log=os.path.join(WORK, "wd.log"))
    res = {"mode": mode, "rounds": rounds}
    try:
        H.warm_all([port])
        for path, label in (("/", "hello"), ("/work", "loop1k")):
            # One discarded round so the compiler's first pass over the handler
            # does not land in a measured one.
            one_round(p.pid, port, path, False)
            rs = [one_round(p.pid, port, path, gk) for _ in range(rounds)]
            cpu = [r["cpu_us_per_req"] for r in rs]
            exits = [r["kvm_exits_per_req"] for r in rs if "kvm_exits_per_req" in r]
            res[label] = {
                "rounds": rs,
                "cpu_us_per_req": statistics.median(cpu),
                "cpu_us_min": min(cpu),
                "cpu_us_max": max(cpu),
                "rps": statistics.median([r["rps"] for r in rs]),
                "p50_us": statistics.median([r["p50_us"] for r in rs]),
                "kvm_exits_per_req": statistics.median(exits) if exits else None,
            }
            e = res[label]["kvm_exits_per_req"]
            print("%-6s %-7s cpu %8.1f us/req (%.1f-%.1f)  rps %9.1f  p50 %8.1f us  "
                  "exits/req %s"
                  % (mode, label, res[label]["cpu_us_per_req"], min(cpu), max(cpu),
                     res[label]["rps"], res[label]["p50_us"],
                     ("%.1f" % e) if e else "-"), flush=True)
    finally:
        H.kill_all([p])
    H.jdump(out, res)


main()
