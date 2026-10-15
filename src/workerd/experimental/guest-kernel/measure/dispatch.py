#!/usr/bin/env python3
# Cost of routing a request from one tenant to another.
#
# Each arrangement serves two endpoints. "solo" answers from the tenant the
# request arrived at; "front" hands the request to a second tenant and returns
# what that one answered. The difference between their latencies is the cost of
# the crossing, measured the same way in all three arrangements: in one process
# through a service binding with the guest kernel off, the same with it on, and
# between two workerd processes over loopback.
#
# Usage: dispatch.py <shared|gk|procs> <out.json>

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

DURATION = os.environ.get("GKM_DUR", "20")
WORK = os.path.join(H.RUN_DIR, "dispatch")

SOLO = """export default {
  async fetch(req) { return new Response("ok"); },
};
"""

FRONT = """export default {
  async fetch(req, env) { return env.BACK.fetch("http://back/"); },
};
"""

BACK = """export default {
  async fetch(req) { return new Response("ok"); },
};
"""


def write_configs(mode, d):
    os.makedirs(d, exist_ok=True)
    for name, src in (("solo.js", SOLO), ("front.js", FRONT), ("back.js", BACK)):
        with open(os.path.join(d, name), "w") as f:
            f.write(src)
    p_solo, p_front, p_back = H.PORT_BASE, H.PORT_BASE + 1, H.PORT_BASE + 2
    if mode.startswith("procs"):
        # A real process-per-tenant design would not route between its own
        # processes over TCP; the unix-socket variant is the floor for that
        # transport, the loopback one the ceiling.
        back_addr = ('unix:' + os.path.join(d, "back.sock")) if mode == "procs-uds" \
            else "127.0.0.1:%d" % p_back
        # The front tenant and the back tenant are separate processes; the
        # binding reaches the back one as an external service.
        a = os.path.join(d, "a.capnp")
        with open(a, "w") as f:
            f.write(
                'using Workerd = import "/workerd/workerd.capnp";\n'
                "const config :Workerd.Config = (\n"
                "  services = [\n"
                '    (name = "solo", worker = (modules = [(name = "worker", esModule = embed "solo.js")],\n'
                '      compatibilityDate = "%s")),\n'
                '    (name = "front", worker = (modules = [(name = "worker", esModule = embed "front.js")],\n'
                '      bindings = [(name = "BACK", service = "back")],\n'
                '      compatibilityDate = "%s")),\n'
                '    (name = "back", external = (address = "%s", http = ()))\n'
                "  ],\n"
                "  sockets = [\n"
                '    (name = "ss", address = "127.0.0.1:%d", http = (), service = "solo"),\n'
                '    (name = "sf", address = "127.0.0.1:%d", http = (), service = "front")\n'
                "  ]\n);\n" % (H.COMPAT_DATE, H.COMPAT_DATE, back_addr, p_solo, p_front)
            )
        b = os.path.join(d, "b.capnp")
        with open(b, "w") as f:
            f.write(
                'using Workerd = import "/workerd/workerd.capnp";\n'
                "const config :Workerd.Config = (\n"
                '  services = [(name = "back", worker = (modules = [(name = "worker", esModule = embed "back.js")],\n'
                '    compatibilityDate = "%s"))],\n'
                '  sockets = [(name = "sb", address = "%s", http = (), service = "back")]\n);\n'
                % (H.COMPAT_DATE, back_addr)
            )
        return [a, b], (p_solo, p_front)
    cfg = os.path.join(d, "one.capnp")
    with open(cfg, "w") as f:
        f.write(
            'using Workerd = import "/workerd/workerd.capnp";\n'
            "const config :Workerd.Config = (\n"
            "  services = [\n"
            '    (name = "solo", worker = (modules = [(name = "worker", esModule = embed "solo.js")],\n'
            '      compatibilityDate = "%s")),\n'
            '    (name = "front", worker = (modules = [(name = "worker", esModule = embed "front.js")],\n'
            '      bindings = [(name = "BACK", service = "back")],\n'
            '      compatibilityDate = "%s")),\n'
            '    (name = "back", worker = (modules = [(name = "worker", esModule = embed "back.js")],\n'
            '      compatibilityDate = "%s"))\n'
            "  ],\n"
            "  sockets = [\n"
            '    (name = "ss", address = "127.0.0.1:%d", http = (), service = "solo"),\n'
            '    (name = "sf", address = "127.0.0.1:%d", http = (), service = "front")\n'
            "  ]\n);\n"
            % (H.COMPAT_DATE, H.COMPAT_DATE, H.COMPAT_DATE, p_solo, p_front)
        )
    return [cfg], (p_solo, p_front)


BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")


def bench(port, conns, seconds=None):
    out = subprocess.run(
        [BENCH, "127.0.0.1", str(port), "/", str(conns), seconds or DURATION, "2"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return {
        "total": int(out[0]),
        "p50_us": float(out[1]),
        "p90_us": float(out[2]),
        "p99_us": float(out[3]),
        "mean_us": float(out[4]),
        "rps": float(out[5]),
        "errors": int(out[6]),
    }


def main():
    mode = sys.argv[1]
    out = sys.argv[2]
    shutil.rmtree(WORK, ignore_errors=True)
    cfgs, (p_solo, p_front) = write_configs(mode, WORK)
    gk = mode == "gk"
    procs = []
    try:
        # The back process comes up first so the front's external binding has
        # something to reach.
        for i, cfg in enumerate(reversed(cfgs)):
            procs.append(H.launch(cfg, gk=gk, log=os.path.join(WORK, "wd%d.log" % i)))
            time.sleep(0.3)
        H.warm_all([p_solo, p_front])
        res = {"mode": mode, "duration": DURATION}
        for conns in (1, 8):
            for name, port in (("solo", p_solo), ("front", p_front)):
                r = bench(port, conns)
                res["%s_c%d" % (name, conns)] = r
                print("%-6s c=%d %-6s p50 %8.1f us  p90 %8.1f  rps %9.1f  err %d"
                      % (mode, conns, name, r["p50_us"], r["p90_us"], r["rps"], r["errors"]),
                      flush=True)
            a = res["solo_c%d" % conns]
            b = res["front_c%d" % conns]
            res["crossing_p50_us_c%d" % conns] = b["p50_us"] - a["p50_us"]
            print("%-6s c=%d crossing p50 delta %8.1f us (%.1f%% of a front request)"
                  % (mode, conns, b["p50_us"] - a["p50_us"],
                     100.0 * (b["p50_us"] - a["p50_us"]) / b["p50_us"]), flush=True)
        H.jdump(out, res)
    finally:
        H.kill_all(procs)


main()
