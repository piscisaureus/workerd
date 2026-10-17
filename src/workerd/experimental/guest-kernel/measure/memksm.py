#!/usr/bin/env python3
# The process-per-tenant arrangement measured with KSM merging its tenants'
# identical pages, which stands in for the sharing a pre-forked design would
# get from copy-on-write. KSM merges pages regardless of how they came to be
# identical, so what it leaves is a floor rather than a prediction.
#
# Needs KSM switched on by root; the caller does that (sweep-ksm.sh).
#
# Usage: memksm.py <out.json> [counts...]

import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

SHIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ksmshim.so")
WORK = os.path.join(H.RUN_DIR, "procs-ksm")


def ksm_stat(name):
    with open("/sys/kernel/mm/ksm/" + name) as f:
        return int(f.read().strip())


def wait_converged(timeout=240.0, quiet_rounds=3):
    """Waits until KSM has scanned everything and the shared count holds still."""
    start_scans = ksm_stat("full_scans")
    deadline = time.time() + timeout
    last, stable = -1, 0
    while time.time() < deadline:
        time.sleep(2.0)
        cur = ksm_stat("pages_sharing")
        if ksm_stat("full_scans") >= start_scans + 2 and abs(cur - last) < max(64, cur // 500):
            stable += 1
            if stable >= quiet_rounds:
                return True
        else:
            stable = 0
        last = cur
    return False


def run_one(n):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    ports = [H.PORT_BASE + i for i in range(n)]
    before = H.sys_used_kb()
    procs = []
    try:
        cfgs = H.write_single_configs(WORK, n, H.PORT_BASE)
        for i, cfg in enumerate(cfgs):
            procs.append(
                H.launch(cfg, gk=False, extra_env={"LD_PRELOAD": SHIM},
                         log=os.path.join(WORK, "p%d.log" % i))
            )
        H.warm_all(ports)
        time.sleep(4)
        pre = H.collect([p.pid for p in procs])
        converged = wait_converged()
        post = H.collect([p.pid for p in procs])
        post["n"] = n
        post["procs"] = n
        post["converged"] = converged
        post["pss_before_ksm"] = pre["Pss"]
        post["uss_before_ksm"] = pre["Uss"]
        post["ksm_pages_sharing"] = ksm_stat("pages_sharing")
        post["ksm_pages_shared"] = ksm_stat("pages_shared")
        post["sys_used_delta"] = H.sys_used_kb() - before
        return post
    finally:
        H.kill_all(procs)
        time.sleep(2.0)


def main():
    out = sys.argv[1]
    counts = [int(x) for x in sys.argv[2:]] or [1, 2, 4, 8, 16, 32, 64]
    rows = []
    for n in counts:
        r = run_one(n)
        rows.append(r)
        print("procs-ksm n=%-4d pss %9d (was %9d)  uss %9d (was %9d) converged=%s"
              % (n, r["Pss"], r["pss_before_ksm"], r["Uss"], r["uss_before_ksm"],
                 r["converged"]), flush=True)
    res = {"mode": "procs-ksm", "rows": rows, "fits": {}}
    xs = [r["n"] for r in rows]
    for key in ("Uss", "Pss", "Rss", "pss_before_ksm", "sys_used_delta"):
        s, i, r2 = H.ols(xs, [r[key] for r in rows])
        res["fits"][key] = {"slope_kb": s, "intercept_kb": i, "r2": r2}
        print("  fit %-16s slope %9.1f kB/tenant  intercept %10.1f kB  R2 %.4f"
              % (key, s, i, r2))
    H.jdump(out, res)


main()
