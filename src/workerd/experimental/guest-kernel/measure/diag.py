#!/usr/bin/env python3
# Where the guest kernel's per-request cost goes.
#
# The same load against the same worker with the guest kernel off and on,
# counting cycles, instructions, minor faults and VM exits per request. If the
# cost were the crossings it would show as exits; if it were the guest's page
# walks it would show as cycles without instructions.
#
# Usage: diag.py <out.json>

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

WORK = os.path.join(H.RUN_DIR, "diag")
BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")
EVENTS = "cycles,instructions,kvm:kvm_exit"
WINDOW = 8


def faults(pid):
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().rsplit(") ", 1)[1].split()
    return int(parts[7]), int(parts[9])  # minflt, majflt


def perf(pid, seconds):
    try:
        r = subprocess.run(
            ["sudo", "-n", "perf", "stat", "-e", EVENTS, "-p", str(pid),
             "--", "sleep", str(seconds)],
            capture_output=True, text=True, timeout=seconds + 60,
        )
    except (OSError, subprocess.TimeoutExpired):
        return {}
    out = {}
    for line in r.stderr.splitlines():
        f = line.split()
        if len(f) >= 2:
            tok = f[0].replace(",", "").replace(".", "")
            if tok.isdigit() and f[1] in ("cycles", "instructions", "kvm:kvm_exit"):
                out[f[1]] = int(tok)
    return out


def run(gk, path="/"):
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
    p = H.launch(cfg, gk=gk, log=os.path.join(WORK, "wd.log"))
    try:
        H.warm_all([port])
        b = subprocess.Popen(
            [BENCH, "127.0.0.1", str(port), path, "4", str(WINDOW + 8), "0"],
            stdout=subprocess.PIPE, text=True,
        )
        time.sleep(4.0)
        f0 = faults(p.pid)
        c = perf(p.pid, WINDOW)
        f1 = faults(p.pid)
        rps = float(b.communicate()[0].split()[5])
        n = rps * WINDOW
        return {
            "gk": gk,
            "path": path,
            "rps": rps,
            "cycles_per_req": c.get("cycles", 0) / n,
            "instructions_per_req": c.get("instructions", 0) / n,
            "ipc": (c.get("instructions", 0) / c["cycles"]) if c.get("cycles") else None,
            "kvm_exits_per_req": c.get("kvm:kvm_exit", 0) / n,
            "minflt_per_req": (f1[0] - f0[0]) / n,
            "majflt_per_req": (f1[1] - f0[1]) / n,
        }
    finally:
        H.kill_all([p])
        time.sleep(1)


def main():
    out = sys.argv[1]
    rows = []
    for path in ("/", "/work"):
        for gk in (False, True):
            r = run(gk, path)
            rows.append(r)
            print("gk=%-5s %-6s cycles/req %10.0f  insn/req %10.0f  IPC %s  "
                  "exits/req %6.2f  minflt/req %6.2f"
                  % (r["gk"], r["path"], r["cycles_per_req"], r["instructions_per_req"],
                     ("%.2f" % r["ipc"]) if r["ipc"] else "-",
                     r["kvm_exits_per_req"], r["minflt_per_req"]), flush=True)
    H.jdump(out, {"rows": rows})


main()
