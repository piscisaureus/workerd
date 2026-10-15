#!/usr/bin/env python3
# Aggregate request rate across many tenants kept busy at once.
#
# Density has a second half: a box holds tenants only if it can also serve
# them. One workerd runs every tenant on a single event loop, so a process
# holding n tenants serves them from one core, while n processes serve them
# from n. This loads every tenant at the same time and sums the rates.
#
# Usage: throughput.py <shared|gk|procs> <n> <out.json>

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

WORK = os.path.join(H.RUN_DIR, "throughput")
BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")
DUR = os.environ.get("GKM_DUR", "15")
CONNS = os.environ.get("GKM_CONNS", "2")
PATH = os.environ.get("GKM_PATH", "/")
HZ = os.sysconf("SC_CLK_TCK")


def cpu_ticks(pids):
    t = 0
    for pid in pids:
        try:
            with open("/proc/%d/stat" % pid) as f:
                parts = f.read().rsplit(") ", 1)[1].split()
            t += int(parts[11]) + int(parts[12])
        except OSError:
            pass
    return t


def main():
    mode, n, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    ports = [H.PORT_BASE + i for i in range(n)]
    procs = []
    try:
        if mode.startswith("procs"):
            # "procs-gk" is one process per tenant with the guest kernel on,
            # which says whether the guest kernel's cost scales across cores
            # or runs into something shared.
            for i, cfg in enumerate(H.write_single_configs(WORK, n, H.PORT_BASE)):
                procs.append(H.launch(cfg, gk=(mode == "procs-gk"),
                                      log=os.path.join(WORK, "p%d.log" % i)))
        else:
            cfg = H.write_multi_config(WORK, n, H.PORT_BASE)
            procs.append(H.launch(cfg, gk=(mode == "gk"), log=os.path.join(WORK, "wd.log")))
        H.warm_all(ports)
        time.sleep(2)
        pids = [p.pid for p in procs]
        clients = [
            subprocess.Popen(
                [BENCH, "127.0.0.1", str(p), PATH, CONNS, DUR, "2"],
                stdout=subprocess.PIPE, text=True,
            )
            for p in ports
        ]
        time.sleep(3.0)
        t0, w0 = cpu_ticks(pids), time.time()
        time.sleep(float(DUR) - 4.0)
        t1, w1 = cpu_ticks(pids), time.time()
        rows = [c.communicate()[0].split() for c in clients]
        rps = sum(float(r[5]) for r in rows)
        p50s = sorted(float(r[1]) for r in rows)
        server_cores = (t1 - t0) / HZ / (w1 - w0)
        res = {
            "mode": mode,
            "n": n,
            "conns_per_tenant": int(CONNS),
            "total_rps": rps,
            "rps_per_tenant": rps / n,
            "p50_us_median_tenant": p50s[len(p50s) // 2],
            "p50_us_worst_tenant": p50s[-1],
            "server_cores_busy": server_cores,
            "rps_per_core": rps / server_cores if server_cores else None,
        }
        print("%-6s n=%-4d total %9.1f rps  per tenant %8.1f  p50 med %8.1f us  "
              "server cores %5.2f  rps/core %9.1f"
              % (mode, n, rps, rps / n, res["p50_us_median_tenant"], server_cores,
                 res["rps_per_core"] or 0), flush=True)
        H.jdump(out, res)
    finally:
        H.kill_all(procs)


main()
