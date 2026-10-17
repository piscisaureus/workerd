#!/usr/bin/env python3
# The largest number of tenants one workerd will hold, by bisection. Under the
# guest kernel each isolate takes a private page-table slot, so there is a
# ceiling; without it there is none to find short of memory.
#
# Usage: ceiling.py <shared|gk> <lo> <hi>
import os, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

def works(mode, n):
    d = os.path.join(H.RUN_DIR, "ceiling")
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    cfg = H.write_multi_config(d, n, H.PORT_BASE)
    p = H.launch(cfg, gk=(mode == "gk"), log=os.path.join(d, "wd.log"))
    try:
        H.warm_all([H.PORT_BASE + i for i in range(n)], timeout=300)
        return True, ""
    except Exception as e:
        tail = ""
        try:
            tail = open(os.path.join(d, "wd.log")).read()[-300:]
        except OSError:
            pass
        return False, "%s | %s" % (e, tail.replace("\n", " ")[:200])
    finally:
        H.kill_all([p]); time.sleep(1)

mode, lo, hi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ok, msg = works(mode, lo)
print("lo n=%d -> %s %s" % (lo, ok, msg), flush=True)
if not ok: sys.exit(1)
while hi - lo > 1:
    mid = (lo + hi) // 2
    ok, msg = works(mode, mid)
    print("n=%-5d %s %s" % (mid, "ok" if ok else "FAIL", msg), flush=True)
    if ok: lo = mid
    else: hi = mid
print("CEILING %s = %d tenants in one process" % (mode, lo))
