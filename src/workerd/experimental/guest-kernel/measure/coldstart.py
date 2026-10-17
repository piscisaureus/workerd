#!/usr/bin/env python3
# Time to first successful response for a tenant that does not exist yet.
#
# Two ways of acquiring one. "loader" asks a running workerd for a worker it
# has never loaded, through a worker-loader binding, and times the request that
# causes the isolate to be created and the module to be compiled; the same
# request repeated against an already-loaded name is the warm baseline, so the
# difference is what the new isolate cost. "spawn" starts a whole workerd for
# one tenant and times from the exec to the first 200.
#
# Usage: coldstart.py <loader|spawn> <shared|gk> <out.json> [rounds]

import os
import shutil
import socket
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

WORK = os.path.join(H.RUN_DIR, "coldstart")

LOADER_HOST = """export default {
  async fetch(req, env) {
    const id = new URL(req.url).searchParams.get("id");
    let salt = 0;
    for (const c of id) salt = (salt * 31 + c.charCodeAt(0)) >>> 0;
    const w = env.LOADER.get(id, () => ({
      compatibilityDate: "%s",
      mainModule: "m.js",
      modules: {
        "m.js": `const TAG = "loaded-${id}";\\n`
          + `const SALT = ${salt};\\n`
          + `export default { async fetch(req) {\\n`
          + `  const u = new URL(req.url);\\n`
          + `  if (u.pathname === "/work") { let a = SALT;\\n`
          + `    for (let i = 0; i < 1000; i++) a = (a * 31 + i) >>> 0;\\n`
          + `    return new Response(String(a)); }\\n`
          + `  return new Response(TAG); } };\\n`,
      },
    }));
    const r = await w.getEntrypoint().fetch("http://x/");
    return new Response(await r.text());
  },
};
""" % H.COMPAT_DATE


def timed_get(port, path, sock=None):
    t0 = time.perf_counter()
    r = H.http_get(port, path, sock=sock)
    dt = (time.perf_counter() - t0) * 1e6
    if b"200" not in r.split(b"\r\n")[0]:
        raise RuntimeError("bad response %r" % r[:200])
    return dt


def run_loader(gk, rounds):
    d = os.path.join(WORK, "loader")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    with open(os.path.join(d, "host.js"), "w") as f:
        f.write(LOADER_HOST)
    port = H.PORT_BASE
    cfg = os.path.join(d, "config.capnp")
    with open(cfg, "w") as f:
        f.write(
            'using Workerd = import "/workerd/workerd.capnp";\n'
            "const config :Workerd.Config = (\n"
            '  services = [(name = "host", worker = (\n'
            '    modules = [(name = "worker", esModule = embed "host.js")],\n'
            '    bindings = [(name = "LOADER", workerLoader = (id = "L"))],\n'
            '    compatibilityDate = "%s"))],\n'
            '  sockets = [(name = "s", address = "127.0.0.1:%d", http = (), service = "host")]\n'
            ");\n" % (H.COMPAT_DATE, port)
        )
    p = H.launch(cfg, gk=gk, log=os.path.join(d, "wd.log"), args=("--experimental",))
    cold, warm = [], []
    try:
        H.wait_port(port, time.time() + 60, p)
        # One connection for every request, so what is timed is the request
        # and not the handshake.
        sock = socket.create_connection(("127.0.0.1", port), 10)
        # A throwaway tenant first, so the first measured one does not also pay
        # for whatever the loader sets up on its first use.
        timed_get(port, "/?id=prime", sock)
        timed_get(port, "/?id=prime", sock)
        for i in range(rounds):
            name = "t%06d" % i
            cold.append(timed_get(port, "/?id=" + name, sock))
            warm.append(timed_get(port, "/?id=" + name, sock))
        sock.close()
    finally:
        H.kill_all([p])
    return cold, warm


def run_spawn(gk, rounds):
    d = os.path.join(WORK, "spawn")
    shutil.rmtree(d, ignore_errors=True)
    cfgs = H.write_single_configs(d, rounds, H.PORT_BASE)
    out = []
    for i, cfg in enumerate(cfgs):
        port = H.PORT_BASE + i
        t0 = time.perf_counter()
        p = H.launch(cfg, gk=gk, log=os.path.join(d, "p%d.log" % i))
        ok = False
        while time.perf_counter() - t0 < 120:
            try:
                s = socket.create_connection(("127.0.0.1", port), 0.2)
                s.settimeout(5)
                s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                buf = s.recv(4096)
                s.close()
                if b"200" in buf.split(b"\r\n")[0]:
                    ok = True
                    break
            except OSError:
                time.sleep(0.0005)
        dt = (time.perf_counter() - t0) * 1e6
        H.kill_all([p])
        if not ok:
            raise RuntimeError("spawn %d never answered" % i)
        out.append(dt)
    return out


def stats(v):
    v = sorted(v)
    return {
        "n": len(v),
        "min_us": v[0],
        "p50_us": statistics.median(v),
        "p90_us": v[int(len(v) * 0.9)] if len(v) > 9 else v[-1],
        "max_us": v[-1],
        "mean_us": statistics.fmean(v),
    }


def main():
    which, mode, out = sys.argv[1], sys.argv[2], sys.argv[3]
    rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    gk = mode == "gk"
    if which == "loader":
        cold, warm = run_loader(gk, rounds)
        res = {"kind": "loader", "mode": mode, "cold": stats(cold), "warm": stats(warm)}
        res["isolate_create_p50_us"] = res["cold"]["p50_us"] - res["warm"]["p50_us"]
        print("loader %-6s cold p50 %9.1f us  warm p50 %8.1f us  new-isolate %9.1f us"
              % (mode, res["cold"]["p50_us"], res["warm"]["p50_us"],
                 res["isolate_create_p50_us"]))
    else:
        v = run_spawn(gk, rounds)
        res = {"kind": "spawn", "mode": mode, "spawn": stats(v)}
        print("spawn  %-6s first-200 p50 %9.1f us  min %9.1f  p90 %9.1f"
              % (mode, res["spawn"]["p50_us"], res["spawn"]["min_us"], res["spawn"]["p90_us"]))
    H.jdump(out, res)


main()
