#!/usr/bin/env python3
# What the body relay costs a request that crosses an in-process service
# binding.
#
# A request or response body crosses such a binding through a kj one-way pipe,
# and where per-isolate heap arenas are active `ReadRelay`
# (io/worker-interface.c++) moves every operation on that pipe onto the event
# loop: a read yields, reads into a host staging buffer of 64 KiB, and copies
# from there into the reader's buffer. This measures what that costs.
#
# The two arms differ only in the relay. `bodiesRelayed()` answers
# `jsg::heapArenasAvailable()`, so the shipping configuration cannot turn the
# relay off -- the bare pipe is what faults there. The comparison is therefore
# made in a configuration where heap arenas are off, so the bare pipe is safe,
# with the relay forced on by a temporary knob in `bodiesRelayed()`:
#
#   static const bool forced = ::getenv("WORKERD_GK_FORCE_BODY_RELAY") != nullptr;
#   return forced || jsg::heapArenasAvailable();
#
# which is not committed, because it is a configuration nobody should ship.
# One binary serves both arms, so no build difference can enter the delta.
# Only the relay-forced-on arms need that knob; the arm that measures the
# shipping configuration (jemalloc with the guest kernel on, where
# heapArenasAvailable() turns the relay on by itself) runs against the tree
# as committed.
#
# The front worker holds the size-dependent work inside the binding: it builds
# the request body itself and reads the response body whole, and answers the
# client with a number, so the client's traffic is the same at every body size
# and in both directions.
#
#   /down/<n>          back answers with n bytes; front reads them with
#                      arrayBuffer(). The response-body relay.
#   /downread/<n>      the same, read chunk by chunk through a reader.
#   /up/<n>            front sends n bytes from a buffer it holds; back reads
#                      them with arrayBuffer(). The request-body relay.
#   /upchunk/<n>/<c>   front sends n bytes as n/c chunks from a JS stream, so
#                      that a fixed body arrives in a varying number of
#                      chunks: this is what tells a per-chunk cost from a
#                      per-request one.
#   /upthru/<n>        front sends n bytes, back returns them unread as its
#                      response body, front reads that: both crossings are
#                      pumps rather than reads (GKM_CASES=pump).
#
# Usage: bodyrelay.py <out.json> <arm> [<arm> ...]
#   An arm is NAME or NAME:KEY=VAL,KEY=VAL -- a name and the extra environment
#   workerd gets (WORKERD_GK_FORCE_BODY_RELAY=1, GK_RUNNER=resident, ...). The
#   arms are interleaved round by round, each with its own launch, so that
#   nothing the machine does over the hour lands on one arm rather than the
#   other.
#   GK=1 runs with the guest kernel. GKM_DUR seconds per round (default 8),
#   GKM_ROUNDS rounds (default 3). WORKERD names the binary. SERVER_CPUS and
#   CLIENT_CPUS taskset the server and the client (defaults put them on
#   separate last-level caches of this machine).

import os
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")
WORK = os.path.join(H.RUN_DIR, "bodyrelay")
DUR = float(os.environ.get("GKM_DUR", "8"))
ROUNDS = int(os.environ.get("GKM_ROUNDS", "3"))
# Two cores of one CCD for the server (the owner and, under the guest kernel,
# its runner share a last-level cache that way, which "Where the runner sits"
# in findings.md shows is worth 22 us a request), another CCD for the client.
SERVER_CPUS = os.environ.get("SERVER_CPUS", "12-17,36-41")
CLIENT_CPUS = os.environ.get("CLIENT_CPUS", "0-5,24-29")

FRONT = r"""
const bufs = new Map();
function buf(n) {
  let b = bufs.get(n);
  if (b === undefined) { b = new Uint8Array(n); bufs.set(n, b); }
  return b;
}
export default {
  async fetch(req, env) {
    const p = new URL(req.url).pathname.split("/");
    if (p[1] === "down") {
      const r = await env.BACK.fetch("http://back/make/" + p[2]);
      const b = await r.arrayBuffer();
      return new Response(String(b.byteLength));
    }
    if (p[1] === "downread") {
      const r = await env.BACK.fetch("http://back/make/" + p[2]);
      const rd = r.body.getReader();
      let t = 0;
      for (;;) {
        const { done, value } = await rd.read();
        if (done) break;
        t += value.byteLength;
      }
      return new Response(String(t));
    }
    if (p[1] === "up") {
      const n = +p[2];
      const r = await env.BACK.fetch("http://back/sink",
          { method: "POST", body: n === 0 ? null : buf(n) });
      return new Response(await r.text());
    }
    if (p[1] === "upthru") {
      const n = +p[2];
      const r = await env.BACK.fetch("http://back/echo",
          { method: "POST", body: n === 0 ? null : buf(n) });
      const b = await r.arrayBuffer();
      return new Response(String(b.byteLength));
    }
    if (p[1] === "upchunk") {
      const n = +p[2], c = +p[3];
      let left = n;
      const s = new ReadableStream({
        pull(ctrl) {
          if (left <= 0) { ctrl.close(); return; }
          const k = Math.min(c, left);
          ctrl.enqueue(new Uint8Array(k));
          left -= k;
        },
      });
      const r = await env.BACK.fetch("http://back/sink", { method: "POST", body: s });
      return new Response(await r.text());
    }
    return new Response("?", { status: 404 });
  },
};
"""

BACK = r"""
const bufs = new Map();
function buf(n) {
  let b = bufs.get(n);
  if (b === undefined) { b = new Uint8Array(n); bufs.set(n, b); }
  return b;
}
export default {
  async fetch(req) {
    const p = new URL(req.url).pathname.split("/");
    if (p[1] === "make") {
      const n = +p[2];
      return new Response(n === 0 ? null : buf(n));
    }
    if (p[1] === "echo") {
      return new Response(req.body);
    }
    if (p[1] === "sink") {
      const b = await req.arrayBuffer();
      return new Response(String(b.byteLength));
    }
    return new Response("?", { status: 404 });
  },
};
"""

K = 1024
M = 1024 * 1024
# (label, path, expected answer)
GROUPS = {
    # The bodies a handler reads whole, with readAllBytes(), plus a JS reader
    # loop and a chunk-count sweep at a fixed total size.
    "main": (
        [
            ("down/%d" % n, "/down/%d" % n, str(n))
            for n in (0, K, 64 * K, 256 * K, 4 * M)
        ]
        + [("up/%d" % n, "/up/%d" % n, str(n)) for n in (0, K, 64 * K, 256 * K, 4 * M)]
        + [("downread/%d" % n, "/downread/%d" % n, str(n)) for n in (64 * K, M)]
        + [
            ("upchunk/1M/%d" % c, "/upchunk/%d/%d" % (M, c), str(M))
            for c in (K, 8 * K, 64 * K, 256 * K)
        ]
    ),
    # A body nobody reads into a buffer: the callee returns the request body as
    # its response body, so each side of the crossing is a kj pump over a
    # relayed stream. This is the path whose synchronous fast path the relay
    # gives up -- pumpTo()'s loop in api/streams/internal.c++ calls
    # tryReadSync() unconditionally, which the kj pipe answers whenever a
    # writer is blocked with enough buffered, and which the relay always
    # declines.
    "pump": [
        ("upthru/%d" % n, "/upthru/%d" % n, str(n))
        for n in (0, K, 64 * K, 256 * K, 4 * M)
    ],
}
# The cases the headline figures rest on, over more rounds: the two directions
# read whole, the pump, and a size on each side of the 64 KiB staging buffer.
GROUPS["focus"] = [
    c
    for c in GROUPS["main"]
    if c[0]
    in ("down/0", "down/65536", "down/4194304", "up/0", "up/65536", "up/4194304")
] + [
    c for c in GROUPS["pump"] if c[0] in ("upthru/0", "upthru/65536", "upthru/4194304")
]
CASES = GROUPS[os.environ.get("GKM_CASES", "main")]
CONNS = tuple(int(c) for c in os.environ.get("GKM_CONNS", "1,8").split(","))


def write_config(d, port):
    os.makedirs(d, exist_ok=True)
    for name, src in (("front.js", FRONT), ("back.js", BACK)):
        with open(os.path.join(d, name), "w") as f:
            f.write(src)
    cfg = os.path.join(d, "c.capnp")
    with open(cfg, "w") as f:
        f.write(
            'using Workerd = import "/workerd/workerd.capnp";\n'
            "const config :Workerd.Config = (\n"
            "  services = [\n"
            '    (name = "front", worker = (modules = [(name = "worker", esModule = embed "front.js")],\n'
            '      bindings = [(name = "BACK", service = "back")],\n'
            '      compatibilityDate = "%s")),\n'
            '    (name = "back", worker = (modules = [(name = "worker", esModule = embed "back.js")],\n'
            '      compatibilityDate = "%s"))\n'
            "  ],\n"
            '  sockets = [(name = "sf", address = "127.0.0.1:%d", http = (), service = "front")]\n);\n'
            % (H.COMPAT_DATE, H.COMPAT_DATE, port)
        )
    return cfg


def launch(cfg, gk, env, log):
    e = dict(os.environ)
    if gk:
        e["WORKERD_EXPERIMENTAL_GUEST_KERNEL"] = "1"
    else:
        e.pop("WORKERD_EXPERIMENTAL_GUEST_KERNEL", None)
    e.update(env)
    argv = ["taskset", "-c", SERVER_CPUS, H.WORKERD, "serve", cfg]
    return subprocess.Popen(
        argv,
        env=e,
        stdout=open(log, "wb"),
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def bench(port, path, conns, seconds):
    argv = [
        "taskset",
        "-c",
        CLIENT_CPUS,
        BENCH,
        "127.0.0.1",
        str(port),
        path,
        str(conns),
        str(seconds),
        "2",
    ]
    f = subprocess.run(argv, capture_output=True, text=True, check=True).stdout.split()
    return {
        "total": int(f[0]),
        "p50_us": float(f[1]),
        "p90_us": float(f[2]),
        "p99_us": float(f[3]),
        "mean_us": float(f[4]),
        "rps": float(f[5]),
        "errors": int(f[6]),
    }


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def check(port, path, want):
    r = H.http_get(port, path).decode("latin1")
    body = r.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in r else ""
    if " 200 " not in r.split("\r\n")[0] or body.strip() != want:
        raise RuntimeError("%s answered %r, wanted %r" % (path, r[:200], want))


def mitigations():
    return {
        "vmscape": open("/sys/devices/system/cpu/vulnerabilities/vmscape")
        .read()
        .strip(),
        "halt_poll_ns": open("/sys/module/kvm/parameters/halt_poll_ns").read().strip(),
    }


def run_arm(name, env, gk, rnd, port, cfg, rows):
    p = launch(cfg, gk, env, os.path.join(WORK, "wd-%s-%d.log" % (name, rnd)))
    try:
        if not H.wait_port(port, time.time() + 120, p):
            raise RuntimeError("%s: port never opened" % name)
        # Every route once, both to compile the handlers and to check that the
        # body really crossed: a wrong length here means the arm is not
        # measuring what it claims.
        for label, path, want in CASES:
            check(port, path, want)
        bench(port, "/down/1024", 4, 4)  # let the JIT settle
        for label, path, want in CASES:
            for conns in CONNS:
                c0, t0 = cpu_seconds(p.pid), time.time()
                r = bench(port, path, conns, DUR)
                r["cpu_us_per_req"] = (
                    (cpu_seconds(p.pid) - c0) / r["total"] * 1e6 if r["total"] else None
                )
                r["cpus_busy"] = (cpu_seconds(p.pid) - c0) / (time.time() - t0)
                rows.setdefault(name, {}).setdefault(
                    "%s c%d" % (label, conns), []
                ).append(r)
                print(
                    "r%d %-6s %-18s c=%d p50 %9.1f us  p99 %9.1f  rps %9.1f  "
                    "cpu %8.1f us  err %d"
                    % (
                        rnd,
                        name,
                        label,
                        conns,
                        r["p50_us"],
                        r["p99_us"],
                        r["rps"],
                        r["cpu_us_per_req"] or -1,
                        r["errors"],
                    ),
                    flush=True,
                )
        if p.poll() is not None:
            raise RuntimeError("%s: workerd exited with %s" % (name, p.returncode))
    finally:
        H.kill_all([p])


def main():
    out = sys.argv[1]
    arms = []
    for spec in sys.argv[2:]:
        name, _, rest = spec.partition(":")
        arms.append((name, dict(kv.split("=", 1) for kv in rest.split(",") if kv)))
    gk = os.environ.get("GK", "0") == "1"
    before = mitigations()
    print(
        "workerd %s; gk %s; arms %s; server cpus %s; client cpus %s"
        % (H.WORKERD, gk, arms, SERVER_CPUS, CLIENT_CPUS),
        flush=True,
    )
    print(
        "vmscape: %(vmscape)s; host halt_poll_ns: %(halt_poll_ns)s" % before, flush=True
    )
    shutil.rmtree(WORK, ignore_errors=True)
    port = H.PORT_BASE
    cfg = write_config(WORK, port)
    rows = {}
    for rnd in range(ROUNDS):
        for name, env in arms:
            run_arm(name, env, gk, rnd, port, cfg, rows)
    after = mitigations()
    print(
        "vmscape after: %(vmscape)s; halt_poll_ns after: %(halt_poll_ns)s" % after,
        flush=True,
    )
    summary = {}
    for name in rows:
        for key, rs in rows[name].items():
            summary.setdefault(name, {})[key] = {
                "p50_us": statistics.median(r["p50_us"] for r in rs),
                "p50_us_range": [
                    min(r["p50_us"] for r in rs),
                    max(r["p50_us"] for r in rs),
                ],
                "rps": statistics.median(r["rps"] for r in rs),
                "rps_range": [min(r["rps"] for r in rs), max(r["rps"] for r in rs)],
                "cpu_us_per_req": statistics.median(r["cpu_us_per_req"] for r in rs),
                "errors": sum(r["errors"] for r in rs),
            }
    H.jdump(
        out,
        {
            "workerd": H.WORKERD,
            "gk": gk,
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
    names = [n for n, _ in arms]
    base = names[0]
    print()
    head = "%-22s" % "case"
    for n in names:
        head += "  %12s %12s" % (n + " p50", n + " rps")
    if len(names) > 1:
        head += "  %10s %8s" % ("delta p50", "paired")
    print(head)
    for key in sorted(summary[base]):
        line = "%-22s" % key
        for n in names:
            s = summary[n][key]
            line += "  %12.1f %12.1f" % (s["p50_us"], s["rps"])
        if len(names) > 1:
            other = names[1]
            line += "  %10.1f" % (
                summary[other][key]["p50_us"] - summary[base][key]["p50_us"]
            )
            # The same delta taken round by round and then medianed, which does
            # not mix one arm's good round with the other's bad one.
            pairs = [
                b["p50_us"] - a["p50_us"]
                for a, b in zip(rows[base][key], rows[other][key], strict=False)
            ]
            line += " %8.1f" % statistics.median(pairs)
        print(line)


if __name__ == "__main__":
    main()
