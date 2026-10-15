#!/usr/bin/env python3
# The memory sweep again, with tenants that do not resemble each other.
#
# The main sweep gives every tenant the same few lines of JavaScript, which is
# the most favourable case for any scheme that shares identical pages between
# processes: almost every page of two such tenants matches. Real tenants run
# different code. This runs the same arrangements over tenants of about fifty
# kilobytes of generated source each, no two alike, and reports the marginal
# cost between two tenant counts so the two tenant shapes can be compared.
#
# Usage: memdiverse.py <out.json> [low] [high]

import os
import random
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

SHIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ksmshim.so")
WORK = os.path.join(H.RUN_DIR, "diverse")
WORDS = [
    "alpha", "bravo", "cedar", "delta", "ember", "flint", "grove", "harbor",
    "indigo", "juniper", "kestrel", "lumen", "marrow", "nimbus", "onyx",
    "pewter", "quarry", "raven", "sable", "thicket", "umber", "vellum",
    "willow", "xenon", "yarrow", "zephyr",
]


def diverse_module(i):
    """About fifty kilobytes of JavaScript that no other tenant shares."""
    r = random.Random(0x5EED ^ i)
    out = []
    names = []
    for f in range(60):
        name = "%s_%s_%d" % (r.choice(WORDS), r.choice(WORDS), r.randrange(1 << 20))
        names.append(name)
        table = ", ".join(str(r.randrange(1 << 28)) for _ in range(40))
        strs = ", ".join(
            '"%s-%s-%d"' % (r.choice(WORDS), r.choice(WORDS), r.randrange(1 << 20))
            for _ in range(20)
        )
        out.append(
            "const T_%s = [%s];\n"
            "const S_%s = [%s];\n"
            "function %s(x) {\n"
            "  let a = x ^ %d;\n"
            "  for (let i = 0; i < T_%s.length; i++) a = (a * 31 + T_%s[i]) >>> 0;\n"
            "  return a ^ S_%s[a %% S_%s.length].length;\n"
            "}\n" % (name, table, name, strs, name, r.randrange(1 << 28), name, name, name, name)
        )
    calls = " + ".join("%s(i)" % n for n in names[:20])
    out.append(
        "export default {\n"
        "  async fetch(req) {\n"
        "    const u = new URL(req.url);\n"
        "    if (u.pathname === '/work') {\n"
        "      let s = 0;\n"
        "      for (let i = 0; i < 8; i++) s = (s + %s) >>> 0;\n"
        "      return new Response(String(s));\n"
        "    }\n"
        "    return new Response('tenant-%d');\n"
        "  },\n"
        "};\n" % (calls, i)
    )
    return "".join(out)


H.tenant_module = diverse_module


def ksm(name):
    with open("/sys/kernel/mm/ksm/" + name) as f:
        return int(f.read().strip())


def wait_ksm(timeout=300.0):
    start = ksm("full_scans")
    deadline = time.time() + timeout
    last, stable = -1, 0
    while time.time() < deadline:
        time.sleep(2.0)
        cur = ksm("pages_sharing")
        if ksm("full_scans") >= start + 2 and abs(cur - last) < max(64, cur // 500):
            stable += 1
            if stable >= 3:
                return True
        else:
            stable = 0
        last = cur
    return False


def run_one(mode, n):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    ports = [H.PORT_BASE + i for i in range(n)]
    before = H.sys_used_kb()
    procs = []
    try:
        env = {"LD_PRELOAD": SHIM} if mode == "procs-ksm" else None
        if mode.startswith("procs"):
            for i, cfg in enumerate(H.write_single_configs(WORK, n, H.PORT_BASE)):
                procs.append(H.launch(cfg, gk=False, extra_env=env,
                                      log=os.path.join(WORK, "p%d.log" % i)))
        else:
            cfg = H.write_multi_config(WORK, n, H.PORT_BASE)
            procs.append(H.launch(cfg, gk=(mode == "gk"),
                                  log=os.path.join(WORK, "wd.log")))
        H.warm_all(ports, timeout=900)
        time.sleep(5)
        if mode == "procs-ksm":
            wait_ksm()
        r = H.collect([p.pid for p in procs])
        r["n"] = n
        r["sys_used_delta"] = H.sys_used_kb() - before
        return r
    finally:
        H.kill_all(procs)
        time.sleep(2)


def main():
    out = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    res = {"kind": "diverse", "lo": lo, "hi": hi, "rows": {}, "marginal_kb": {}}
    for mode in ("shared", "gk", "procs", "procs-ksm"):
        a = run_one(mode, lo)
        b = run_one(mode, hi)
        res["rows"][mode] = {"lo": a, "hi": b}
        m = {}
        for key in ("Uss", "Pss", "Rss", "sys_used_delta"):
            m[key] = (b[key] - a[key]) / (hi - lo)
        res["marginal_kb"][mode] = m
        print("%-10s marginal/tenant  uss %8.1f  pss %8.1f  sys %8.1f  kB"
              % (mode, m["Uss"], m["Pss"], m["sys_used_delta"]), flush=True)
    H.jdump(out, res)


main()
