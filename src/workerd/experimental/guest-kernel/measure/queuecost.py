#!/usr/bin/env python3
# What the run queue's links cost a served request.
#
# queue-cost.c++ prices the queue operations on their own; this asks what they
# are worth where it matters, on workloads that make the loop turn rather than
# ones that move bytes: a small request at one connection and at eight, a
# request that chains n timers so the loop arms n events and the turns consume
# them one at a time, a request that arms n at once so that n links are held
# simultaneously, and a request that makes n subrequests over a binding so the
# promise graph of a crossing runs n times.
#
# An arm here is a workerd binary rather than a build knob, because what is
# under test is in kj. The arms are launched alternately, so whatever drifts
# over a sequence of launches drifts across both of them, and every launch is
# reported on its own: a median over launches hides the spread that matters on
# this box (see findings.md).
#
# Usage: queuecost.py <out.json> <name>=<workerd> [<name>=<workerd> ...]
#   GK=1 runs with the guest kernel, which is the configuration the links are
#   for; GKM_DUR seconds per case (default 8), GKM_ROUNDS launches per arm
#   (default 5), SERVER_CPUS and CLIENT_CPUS taskset the server and the client.

import os
import shutil
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bodyrelay as B
import harness as H

WORK = Path(H.RUN_DIR) / "queuecost"
DUR = float(os.environ.get("GKM_DUR", "8"))
ROUNDS = int(os.environ.get("GKM_ROUNDS", "5"))

FRONT = r"""
export default {
  async fetch(req, env) {
    const p = new URL(req.url).pathname.split('/');
    const n = +p[2];
    if (p[1] === 'chain') {
      // n timers awaited one after another: each is an event the loop arms on
      // the turn's behalf and a later turn consumes, which is the queue step
      // this measures, with one link held at a time.
      for (let i = 0; i < n; i++) await new Promise((r) => setTimeout(r, 0));
      return new Response(String(n));
    }
    if (p[1] === 'par') {
      // The same n timers, all outstanding at once, so the loop holds n links.
      const all = [];
      for (let i = 0; i < n; i++) all.push(new Promise((r) => setTimeout(r, 0)));
      await Promise.all(all);
      return new Response(String(n));
    }
    if (p[1] === 'sub') {
      // n subrequests over a binding, each of which runs the whole promise
      // graph of a crossing: many more queue steps per request than a timer.
      let got = 0;
      for (let i = 0; i < n; i++) {
        got += (await (await env.BACK.fetch('http://back/')).text()).length;
      }
      return new Response(String(got));
    }
    return new Response('ok');
  },
};
"""

BACK = r"""
export default { async fetch() { return new Response('ok'); } };
"""

# label, path, connections
CASES = (
    ("small c1", "/", 1),
    ("small c8", "/", 8),
    ("chain/50 c1", "/chain/50", 1),
    ("chain/50 c8", "/chain/50", 8),
    ("par/200 c1", "/par/200", 1),
    ("sub/20 c8", "/sub/20", 8),
)


def write_config(d, port):
    d.mkdir(parents=True, exist_ok=True)
    for name, src in (("front.js", FRONT), ("back.js", BACK)):
        (d / name).write_text(src)
    cfg = d / "c.capnp"
    cfg.write_text(
        'using Workerd = import "/workerd/workerd.capnp";\n'
        "const config :Workerd.Config = (\n"
        "  services = [\n"
        '    (name = "front", worker = (modules = [(name = "worker", esModule = embed "front.js")],\n'
        '      bindings = [(name = "BACK", service = "back")],\n'
        f'      compatibilityDate = "{H.COMPAT_DATE}")),\n'
        '    (name = "back", worker = (modules = [(name = "worker", esModule = embed "back.js")],\n'
        f'      compatibilityDate = "{H.COMPAT_DATE}"))\n'
        "  ],\n"
        f'  sockets = [(name = "sf", address = "127.0.0.1:{port}", http = (), service = "front")]\n);\n'
    )
    return str(cfg)


def run_arm(arm, rnd, run, rows):
    name, workerd = arm
    gk, port, cfg = run
    H.WORKERD = workerd
    p = B.launch(cfg, gk, {}, str(WORK / f"wd-{name}-{rnd}.log"))
    try:
        if not H.wait_port(port, time.time() + 180, p):
            raise RuntimeError(f"{name}: port never opened")
        B.check(port, "/", "ok")
        B.check(port, "/chain/2", "2")
        B.check(port, "/par/2", "2")
        B.check(port, "/sub/2", "4")
        B.bench(port, "/", 4, 3)  # let the JIT and the connections settle
        B.bench(port, "/chain/50", 4, 3)
        for label, path, conns in CASES:
            c0, t0 = B.cpu_seconds(p.pid), time.time()
            r = B.bench(port, path, conns, DUR)
            dc, dt = B.cpu_seconds(p.pid) - c0, time.time() - t0
            r["cpu_us_per_req"] = dc / r["total"] * 1e6 if r["total"] else None
            r["cpus_busy"] = dc / dt
            rows.setdefault(name, {}).setdefault(label, []).append(r)
            print(
                f"r{rnd} {name:<5} {label:<12} p50 {r['p50_us']:9.1f} us  "
                f"p99 {r['p99_us']:9.1f}  rps {r['rps']:9.1f}  "
                f"cpu {r['cpu_us_per_req'] or -1:8.1f} us  err {r['errors']}",
                flush=True,
            )
        if p.poll() is not None:
            raise RuntimeError(f"{name}: workerd exited with {p.returncode}")
    finally:
        H.kill_all([p])


def main():
    out = sys.argv[1]
    arms = [spec.split("=", 1) for spec in sys.argv[2:]]
    gk = os.environ.get("GK", "0") == "1"
    before = B.mitigations()
    print(
        f"arms {arms}; gk {gk}; rounds {ROUNDS}; {DUR}s per case; "
        f"server cpus {B.SERVER_CPUS}; client cpus {B.CLIENT_CPUS}",
        flush=True,
    )
    print(
        f"vmscape: {before['vmscape']}; host halt_poll_ns: {before['halt_poll_ns']}",
        flush=True,
    )
    shutil.rmtree(WORK, ignore_errors=True)
    port = H.PORT_BASE
    cfg = write_config(WORK, port)
    rows = {}
    for rnd in range(ROUNDS):
        for arm in arms:
            run_arm(arm, rnd, (gk, port, cfg), rows)
    after = B.mitigations()
    print(
        f"vmscape after: {after['vmscape']}; halt_poll_ns after: {after['halt_poll_ns']}",
        flush=True,
    )

    summary = {}
    for name, cases in rows.items():
        for label, rs in cases.items():
            summary.setdefault(name, {})[label] = {
                "rps_launches": [r["rps"] for r in rs],
                "p50_us_launches": [r["p50_us"] for r in rs],
                "cpu_us_per_req_launches": [r["cpu_us_per_req"] for r in rs],
                "rps_median": statistics.median(r["rps"] for r in rs),
                "p50_us_median": statistics.median(r["p50_us"] for r in rs),
                "errors": sum(r["errors"] for r in rs),
            }
    H.jdump(
        out,
        {
            "arms": arms,
            "gk": gk,
            "rounds": ROUNDS,
            "duration_s": DUR,
            "server_cpus": B.SERVER_CPUS,
            "client_cpus": B.CLIENT_CPUS,
            "mitigations": [before, after],
            "summary": summary,
            "rows": rows,
        },
    )

    names = [n for n, _ in arms]
    base = names[0]
    print()
    print(f"{'case':<12}  {'arm':<5}  {'rps per launch':<58}  {'min':>9} {'max':>9}")
    for label, _, _ in CASES:
        for n in names:
            rps = summary[n][label]["rps_launches"]
            cells = " ".join(f"{v:9.0f}" for v in rps)
            print(f"{label:<12}  {n:<5}  {cells:<58}  {min(rps):9.0f} {max(rps):9.0f}")
    if len(names) > 1:
        other = names[1]
        print()
        print(
            f"{'case':<12}  {'paired rps delta per launch':<58}  "
            f"{'median':>8} {'as %':>7}"
        )
        for label, _, _ in CASES:
            a = summary[base][label]["rps_launches"]
            b = summary[other][label]["rps_launches"]
            d = [y - x for x, y in zip(a, b, strict=False)]
            cells = " ".join(f"{v:9.0f}" for v in d)
            pct = statistics.median(d) / statistics.median(a) * 100
            print(f"{label:<12}  {cells:<58}  {statistics.median(d):8.0f} {pct:6.2f}%")


if __name__ == "__main__":
    main()
