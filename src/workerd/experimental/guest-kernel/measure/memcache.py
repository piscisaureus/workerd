#!/usr/bin/env python3
# What moving the shared memory cache behind hostcalls costs a cache-heavy
# worker.
#
# A MemoryCache hit used to be a lock and a refcount on a process-heap object,
# taken inside the turn; the cache is now a host object (api/memory-cache.h),
# and a hit is one hostcall -- a VM exit under the guest kernel, a direct call
# without it -- that copies the value into the caller's own memory, through the
# frame when it fits and into a buffer the turn names otherwise. A miss is a
# hostcall to lead, the fallback, and a hostcall to publish. This measures the
# per-operation cost of that on one worker, before and after, in each
# configuration the runtime ships in: N is the jemalloc binary without the
# guest kernel, G the plain binary under it (GK_RUNNER=resident
# GK_RUNNER_PIN=other, no arenas), A the jemalloc binary under it with
# per-isolate heap arenas. A single worker is enough for the cost: the
# before-binary serves one worker correctly in every arm, since nothing
# crosses an arena, and the hit path is the same code whatever the worker count.
#
# Routes, each answering a count so the client's traffic is the same at every
# n: /hit/<n> reads one cached 32-byte value n times; /hit-large/<n> reads one
# cached 64 KiB value n times (larger than the hostcall frame, so the value is
# fetched into a buffer the turn names, or a frame at a time where the buffer
# is not in an arena); /miss/<n> reads n fresh keys with a synchronous fallback,
# so each read leads, runs the fallback and publishes; /actor-id/<n> mints a
# Durable Object id from a name n times, the control: a hostcall that already
# ships, one exit per call, with no cache in it.
#
# Usage: memcache.py <out.json> <arm> [<arm> ...]
#   an arm is NAME:BIN=<workerd>,GK=0|1[,KEY=VAL...]; the rest of the pairs go
#   into the environment. GKM_DUR seconds per case (default 4), GKM_ROUNDS
#   launches per arm (default 4); SERVER_CPUS and CLIENT_CPUS taskset the
#   server and the client. Arms are interleaved launch by launch, so a paired
#   difference compares launches made under the same conditions.

import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bodyrelay as B
import harness as H

WORK = Path(H.RUN_DIR) / "memcache"
DUR = float(os.environ.get("GKM_DUR", "4"))
ROUNDS = int(os.environ.get("GKM_ROUNDS", "4"))
SERVER_CPUS = os.environ.get("SERVER_CPUS", "12-17,36-41")
CLIENT_CPUS = os.environ.get("CLIENT_CPUS", "0-5,24-29")

WORKER = r"""
let seq = 0;
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const [, route, count] = url.pathname.split('/');
    const n = Number(count ?? '1');
    if (route === 'hit') {
      for (let i = 0; i < n; i++) {
        await env.CACHE.read('small', () => ({ value: 'x'.repeat(32) }));
      }
      return new Response(String(n));
    }
    if (route === 'hit-large') {
      for (let i = 0; i < n; i++) {
        const v = await env.CACHE.read('large', () => ({ value: 'y'.repeat(65536) }));
        if (v.length !== 65536) return new Response('bad', { status: 500 });
      }
      return new Response(String(n));
    }
    if (route === 'miss') {
      for (let i = 0; i < n; i++) {
        const k = 'k' + (seq++);
        await env.CACHE.read(k, () => ({ value: 'z'.repeat(32) }));
      }
      return new Response(String(n));
    }
    if (route === 'actor-id') {
      // The control: a hostcall that ships already (the actor-id namespace key,
      // server/actor-id-impl.h), one exit per call, on the same path.
      for (let i = 0; i < n; i++) env.NS.idFromName('name');
      return new Response(String(n));
    }
    return new Response('?', { status: 404 });
  },
};
export class Dummy {
  fetch() { return new Response('dummy'); }
}
"""

CASES = (
    ("hit 1", "/hit/1", "1"),
    ("hit 100", "/hit/100", "100"),
    ("hit-large 1", "/hit-large/1", "1"),
    ("hit-large 100", "/hit-large/100", "100"),
    ("miss 1", "/miss/1", "1"),
    ("miss 100", "/miss/100", "100"),
    ("actor-id 1", "/actor-id/1", "1"),
    ("actor-id 100", "/actor-id/100", "100"),
)


def write_config(d, port):
    d.mkdir(parents=True, exist_ok=True)
    (d / "worker.js").write_text(WORKER)
    cfg = d / "c.capnp"
    cfg.write_text(
        'using Workerd = import "/workerd/workerd.capnp";\n'
        "const config :Workerd.Config = (\n"
        "  services = [\n"
        '    (name = "w", worker = (modules = [(name = "worker", esModule = embed "worker.js")],\n'
        '      bindings = [(name = "CACHE", memoryCache = (id = "bench",\n'
        "        limits = (maxKeys = 1000000, maxValueSize = 1048576, maxTotalValueSize = 268435456))),\n"
        '        (name = "NS", durableObjectNamespace = "Dummy")],\n'
        '      durableObjectNamespaces = [(className = "Dummy", uniqueKey = "bench-dummy")],\n'
        "      durableObjectStorage = (inMemory = void),\n"
        f'      compatibilityDate = "{H.COMPAT_DATE}"))\n'
        "  ],\n"
        f'  sockets = [(name = "s", address = "127.0.0.1:{port}", http = (), service = "w")]\n);\n'
    )
    return str(cfg)


def launch(binary, cfg, gk, env, log):
    e = dict(os.environ)
    if gk:
        e["WORKERD_EXPERIMENTAL_GUEST_KERNEL"] = "1"
    else:
        e.pop("WORKERD_EXPERIMENTAL_GUEST_KERNEL", None)
    e.update(env)
    argv = ["taskset", "-c", SERVER_CPUS, binary, "serve", "--experimental", cfg]
    return subprocess.Popen(
        argv,
        env=e,
        stdout=log.open("wb"),
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def run_arm(arm, rnd, port, cfg, rows):
    name, binary, gk, env = arm
    log = WORK / f"wd-{name}-{rnd}.log"
    p = launch(binary, cfg, gk, env, log)
    try:
        if not H.wait_port(port, time.time() + 120, p):
            raise RuntimeError(f"{name}: port never opened")
        for _label, path, want in CASES:
            B.check(port, path, want)
        B.bench(port, "/hit/100", 1, 2)  # let the JIT settle
        for label, path, _want in CASES:
            c0 = B.cpu_seconds(p.pid)
            r = B.bench(port, path, 1, DUR)
            r["cpu_us_per_req"] = (
                (B.cpu_seconds(p.pid) - c0) / r["total"] * 1e6 if r["total"] else None
            )
            rows.setdefault(name, {}).setdefault(label, []).append(r)
            print(
                f"r{rnd} {name:<8} {label:<14} p50 {r['p50_us']:9.1f} us  "
                f"p99 {r['p99_us']:9.1f}  rps {r['rps']:9.1f}  "
                f"cpu {r['cpu_us_per_req'] or -1:8.1f} us  err {r['errors']}",
                flush=True,
            )
        if p.poll() is not None:
            raise RuntimeError(f"{name}: workerd exited with {p.returncode}")
    finally:
        H.kill_all([p])
    text = log.read_bytes().decode("latin1")
    for bad in ("condemned", "guest fault", "FATAL"):
        if bad in text:
            raise RuntimeError(f"{name} round {rnd}: workerd log says {bad!r}")


def main():
    out = sys.argv[1]
    arms = []
    for spec in sys.argv[2:]:
        name, _, rest = spec.partition(":")
        kv = dict(pair.split("=", 1) for pair in rest.split(",") if pair)
        binary = kv.pop("BIN")
        gk = kv.pop("GK", "0") == "1"
        arms.append((name, binary, gk, kv))
    before = B.mitigations()
    print(
        f"arms {arms}; server cpus {SERVER_CPUS}; client cpus {CLIENT_CPUS}", flush=True
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
            run_arm(arm, rnd, port, cfg, rows)
    after = B.mitigations()
    print(
        f"vmscape after: {after['vmscape']}; halt_poll_ns after: {after['halt_poll_ns']}",
        flush=True,
    )
    summary = {}
    for name, cases in rows.items():
        for key, rs in cases.items():
            summary.setdefault(name, {})[key] = {
                "p50_us": statistics.median(r["p50_us"] for r in rs),
                "p50_us_launches": [r["p50_us"] for r in rs],
                "rps": statistics.median(r["rps"] for r in rs),
                "cpu_us_per_req": statistics.median(r["cpu_us_per_req"] for r in rs),
                "errors": sum(r["errors"] for r in rs),
            }
    H.jdump(
        out,
        {
            "arms": arms,
            "rounds": ROUNDS,
            "duration_s": DUR,
            "server_cpus": SERVER_CPUS,
            "client_cpus": CLIENT_CPUS,
            "mitigations": [before, after],
            "summary": summary,
            "rows": rows,
        },
    )
    names = [n for n, _, _, _ in arms]
    print()
    for key in [c[0] for c in CASES]:
        line = f"{key:<14}"
        for n in names:
            launches = " ".join(f"{r['p50_us']:.1f}" for r in rows[n][key])
            line += f"  {n}: {launches}"
        if len(names) == 2:
            pairs = [
                b["p50_us"] - a["p50_us"]
                for a, b in zip(rows[names[0]][key], rows[names[1]][key], strict=False)
            ]
            line += "  paired: " + " ".join(f"{d:+.1f}" for d in pairs)
        print(line)


if __name__ == "__main__":
    main()
