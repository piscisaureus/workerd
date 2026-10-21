#!/usr/bin/env python3
# Marginal memory per tenant, fitted across a range of tenant counts.
#
# Three arrangements: one workerd with every tenant and the guest kernel off
# ("shared"), the same with it on ("gk"), and one workerd per tenant ("procs").
# For each, every tenant count in the sweep is launched from scratch, warmed
# with one request per tenant, and measured; the marginal cost is the slope of
# a least-squares fit, so the large fixed cost lands in the intercept instead
# of being smeared over the tenants.
#
# Usage: memory.py <shared|gk|procs> <out.json> [counts...]

import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

SETTLE = float(os.environ.get("GKM_SETTLE", "4"))


def run_one(mode, n, workdir):
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    ports = [H.PORT_BASE + i for i in range(n)]
    before = H.sys_used_kb()
    before_pt = H.pagetables_kb()
    procs = []
    try:
        if mode == "procs":
            cfgs = H.write_single_configs(workdir, n, H.PORT_BASE)
            for i, cfg in enumerate(cfgs):
                procs.append(H.launch(cfg, gk=False, log=os.path.join(workdir, "p%d.log" % i)))
        else:
            cfg = H.write_multi_config(workdir, n, H.PORT_BASE)
            procs.append(H.launch(cfg, gk=(mode == "gk"), log=os.path.join(workdir, "wd.log")))
        H.warm_all(ports)
        time.sleep(SETTLE)
        pids = [p.pid for p in procs]
        tot = H.collect(pids)
        tot["n"] = n
        tot["procs"] = len(pids)
        tot["sys_used_delta"] = H.sys_used_kb() - before
        tot["pagetables_delta"] = H.pagetables_kb() - before_pt
        return tot
    finally:
        H.kill_all(procs)
        time.sleep(1.0)


def main():
    mode = sys.argv[1]
    out = sys.argv[2]
    counts = [int(x) for x in sys.argv[3:]] or [1, 2, 4, 8, 16, 32, 64, 128]
    rows = []
    for n in counts:
        t0 = time.time()
        r = run_one(mode, n, os.path.join(H.RUN_DIR, mode))
        r["launch_s"] = round(time.time() - t0, 2)
        rows.append(r)
        print(
            "%-6s n=%-4d uss=%9d pss=%9d rss=%9d pte=%8d sysdelta=%9d ptdelta=%8d (%.1fs)"
            % (
                mode,
                n,
                r["Uss"],
                r["Pss"],
                r["Rss"],
                r["VmPTE"],
                r["sys_used_delta"],
                r["pagetables_delta"],
                r["launch_s"],
            ),
            flush=True,
        )
    res = {"mode": mode, "rows": rows, "fits": {}}
    xs = [r["n"] for r in rows]
    for key in ("Uss", "Pss", "Rss", "VmPTE", "sys_used_delta", "pagetables_delta"):
        s, i, r2 = H.ols(xs, [r[key] for r in rows])
        res["fits"][key] = {"slope_kb": s, "intercept_kb": i, "r2": r2}
        print("  fit %-16s slope %9.1f kB/tenant  intercept %10.1f kB  R2 %.4f"
              % (key, s, i, r2))
    H.jdump(out, res)


main()
