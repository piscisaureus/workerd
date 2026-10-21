#!/usr/bin/env python3
# What the tunnel and WebSocket relays cost a chatty protocol that crosses an
# in-process service binding.
#
# A connect() tunnel through a binding is a kj two-way pipe and a WebSocket
# through one is a kj WebSocket pipe; where per-isolate heap arenas are active,
# IoStreamRelay and WebSocketRelay (io/worker-interface.c++) make every read
# and every receive from the event loop, as ReadRelay does for bodies. The body
# relay's unit is a tryRead() call, and bodies pay few of those because the
# reader asks in 128-512 KiB buffers; a socket's unit is whatever the worker's
# JS reads, and a WebSocket's is a message, so a chatty protocol pays the
# relay's fixed cost per read or per message. This measures that: n round
# trips of a few bytes inside one request, over a tunnel and over a WebSocket.
#
# The arms are bodyrelay.py's, with the same temporary knob in the three
# relay predicates of io/worker-interface.c++ (bodiesRelayed(),
# tunnelsRelayed(), webSocketsRelayed()), never committed:
#
#   static const bool forced = ::getenv("WORKERD_GK_FORCE_BODY_RELAY") != nullptr;
#   return forced || jsg::heapArenasAvailable();
#
# Routes: /tunnel/<n>, the front connects to the back over its binding and
# writes a four-byte line n times, reading the back's four-byte echo after each;
# /ws/<n>, the same over a WebSocket, one message at a time; /ws-burst/<n>, the
# n messages sent before any echo is read, so that the relay's turns overlap
# the sender's. There is no tunnel burst: a kj pipe has no buffer, so a tunnel
# whose caller writes n lines before reading any echo parks the callee's second
# echo, which stops the callee reading, which parks the caller -- with or
# without the relay, and without the guest kernel. Every route answers the
# count, so the client's traffic is the same at every n. The WebSocket routes
# wait for the callee's close reply before answering, as binding-crossing-
# test.sh's do: a caller that closes and returns at once lets the callee cancel
# its pump while its close is still queued on the event loop, which under heap
# arenas is the event-disarm hazard README.md records as not covered, and
# condemns the callee (GK_WS_LEAVE=1 runs that variant).
#
# Usage: socketrelay.py <out.json> <arm> [<arm> ...]
#   as bodyrelay.py: an arm is NAME or NAME:KEY=VAL,KEY=VAL; GK=1 runs with the
#   guest kernel; GKM_DUR seconds per round (default 4), GKM_ROUNDS rounds
#   (default 4); WORKERD names the binary; SERVER_CPUS and CLIENT_CPUS taskset
#   the server and the client.

import os
import shutil
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bodyrelay as B
import harness as H

WORK = Path(H.RUN_DIR) / "socketrelay"
DUR = float(os.environ.get("GKM_DUR", "4"))
ROUNDS = int(os.environ.get("GKM_ROUNDS", "4"))

FRONT = r"""
const enc = new TextEncoder();
const dec = new TextDecoder();
function lineReader(readable) {
  const reader = readable.getReader();
  let buf = '';
  return async () => {
    for (;;) {
      const i = buf.indexOf('\n');
      if (i >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); return l; }
      const { value, done } = await reader.read();
      if (done) return null;
      buf += dec.decode(value, { stream: true });
    }
  };
}
function wsQueue(ws) {
  const queue = [], waiters = [];
  const push = (v) => { const w = waiters.shift(); if (w) w(v); else queue.push(v); };
  ws.addEventListener('message', (e) => push(e.data));
  ws.addEventListener('close', (e) => push(`close:${e.code}`));
  ws.addEventListener('error', (e) => push(`error:${e.message}`));
  return () => queue.length ? Promise.resolve(queue.shift()) : new Promise((r) => waiters.push(r));
}
export default {
  async fetch(req, env) {
    const p = new URL(req.url).pathname.split('/');
    const n = +p[2];
    if (p[1] === 'tunnel') {
      const s = env.BACK.connect('back:1');
      const w = s.writable.getWriter();
      const line = lineReader(s.readable);
      let got = 0;
      for (let i = 0; i < n; i++) {
        await w.write(enc.encode('ping\n'));
        if ((await line()) === 'pong') got++;
      }
      await w.close();
      while ((await line()) !== null) {}
      return new Response(String(got));
    }
    if (p[1] === 'ws' || p[1] === 'ws-burst' || p[1] === 'ws-leave') {
      const r = await env.BACK.fetch('http://back/ws', { headers: { Upgrade: 'websocket' } });
      const ws = r.webSocket;
      ws.accept();
      const next = wsQueue(ws);
      let got = 0;
      if (p[1] === 'ws') {
        for (let i = 0; i < n; i++) { ws.send('ping'); if ((await next()) === 'pong') got++; }
      } else {
        for (let i = 0; i < n; i++) ws.send('ping');
        for (let i = 0; i < n; i++) if ((await next()) === 'pong') got++;
      }
      ws.close(1000, 'done');
      // The callee's close reply is awaited, except by /ws-leave: see the header.
      if (p[1] !== 'ws-leave') await next();
      return new Response(String(got));
    }
    return new Response('?', { status: 404 });
  },
};
"""

BACK = r"""
const enc = new TextEncoder();
const dec = new TextDecoder();
export default {
  async fetch(req) {
    if (new URL(req.url).pathname === '/ws') {
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      server.accept();
      server.addEventListener('message', () => server.send('pong'));
      server.addEventListener('close', (e) => { try { server.close(e.code, e.reason); } catch {} });
      return new Response(null, { status: 101, webSocket: client });
    }
    return new Response('?', { status: 404 });
  },
  async connect(socket) {
    const w = socket.writable.getWriter();
    const reader = socket.readable.getReader();
    let buf = '';
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        buf = buf.slice(i + 1);
        await w.write(enc.encode('pong\n'));
      }
    }
    await w.close();
  },
};
"""

CASES = (
    [(f"tunnel/{n}", f"/tunnel/{n}", str(n)) for n in (1, 10, 100, 1000)]
    + [(f"ws/{n}", f"/ws/{n}", str(n)) for n in (1, 10, 100, 1000)]
    + [(f"ws-burst/{n}", f"/ws-burst/{n}", str(n)) for n in (100, 1000)]
    + (
        [(f"ws-leave/{n}", f"/ws-leave/{n}", str(n)) for n in (100,)]
        if os.environ.get("GK_WS_LEAVE")
        else []
    )
)
CONNS = tuple(int(c) for c in os.environ.get("GKM_CONNS", "1").split(","))


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
    name, env = arm
    gk, port, cfg = run
    p = B.launch(cfg, gk, env, str(WORK / f"wd-{name}-{rnd}.log"))
    try:
        if not H.wait_port(port, time.time() + 120, p):
            raise RuntimeError(f"{name}: port never opened")
        for _label, path, want in CASES:
            B.check(port, path, want)
        B.bench(port, "/tunnel/10", 4, 3)  # let the JIT settle
        B.bench(port, "/ws/10", 4, 3)
        for label, path, _want in CASES:
            for conns in CONNS:
                c0, t0 = B.cpu_seconds(p.pid), time.time()
                r = B.bench(port, path, conns, DUR)
                r["cpu_us_per_req"] = (
                    (B.cpu_seconds(p.pid) - c0) / r["total"] * 1e6
                    if r["total"]
                    else None
                )
                r["cpus_busy"] = (B.cpu_seconds(p.pid) - c0) / (time.time() - t0)
                rows.setdefault(name, {}).setdefault(f"{label} c{conns}", []).append(r)
                cpu = r["cpu_us_per_req"] or -1
                print(
                    f"r{rnd} {name:<6} {label:<18} c={conns} p50 {r['p50_us']:9.1f} us  "
                    f"p99 {r['p99_us']:9.1f}  rps {r['rps']:9.1f}  cpu {cpu:8.1f} us  "
                    f"err {r['errors']}",
                    flush=True,
                )
        if p.poll() is not None:
            raise RuntimeError(f"{name}: workerd exited with {p.returncode}")
    finally:
        H.kill_all([p])


def main():
    out = sys.argv[1]
    arms = []
    for spec in sys.argv[2:]:
        name, _, rest = spec.partition(":")
        arms.append((name, dict(kv.split("=", 1) for kv in rest.split(",") if kv)))
    gk = os.environ.get("GK", "0") == "1"
    before = B.mitigations()
    print(
        f"workerd {H.WORKERD}; gk {gk}; arms {arms}; server cpus {B.SERVER_CPUS}; "
        f"client cpus {B.CLIENT_CPUS}",
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
            "workerd": H.WORKERD,
            "gk": gk,
            "arms": arms,
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
    head = f"{'case':<22}"
    for n in names:
        head += f"  {n + ' p50':>12}"
    if len(names) > 1:
        head += f"  {'delta p50':>10} {'paired (min, max)':>24}"
    print(head)
    for key in sorted(
        summary[base], key=lambda k: (k.split("/")[0], int(k.split("/")[1].split()[0]))
    ):
        line = f"{key:<22}"
        for n in names:
            line += f"  {summary[n][key]['p50_us']:12.1f}"
        if len(names) > 1:
            other = names[1]
            line += f"  {summary[other][key]['p50_us'] - summary[base][key]['p50_us']:10.1f}"
            pairs = [
                b["p50_us"] - a["p50_us"]
                for a, b in zip(rows[base][key], rows[other][key], strict=False)
            ]
            line += f" {statistics.median(pairs):8.1f} ({min(pairs):7.1f}, {max(pairs):7.1f})"
        print(line)


if __name__ == "__main__":
    main()
