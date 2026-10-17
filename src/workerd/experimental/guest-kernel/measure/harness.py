# Shared pieces of the guest-kernel density measurements: config generation,
# workerd process control, and memory accounting.
#
# A "tenant" here is one worker with its own V8 isolate. The three arrangements
# under test are a single workerd holding every tenant with the guest kernel
# off, the same with it on, and one single-tenant workerd per tenant.

import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

WORKERD = os.environ.get("WORKERD", "bazel-bin/src/workerd/server/workerd")
PORT_BASE = int(os.environ.get("GKM_PORT_BASE", "21000"))
RUN_DIR = os.environ.get(
    "GKM_RUN_DIR", os.path.join(os.environ.get("TMPDIR", "/tmp"), "gkmeasure-run"))

COMPAT_DATE = "2024-10-01"


def tenant_module(i):
    # Distinct constants so two tenants never compile to identical bytecode, as
    # two real tenants would not. The body is otherwise the same in every
    # arrangement, so the arrangements differ only in how the isolates are
    # housed.
    return (
        'const TAG = "tenant-%d-aa55aa55";\n'
        "const SALT = %d;\n"
        "export default {\n"
        "  async fetch(req) {\n"
        "    const u = new URL(req.url);\n"
        "    if (u.pathname === '/work') {\n"
        "      let a = SALT;\n"
        "      for (let i = 0; i < 1000; i++) a = (a * 31 + i) >>> 0;\n"
        "      return new Response(String(a));\n"
        "    }\n"
        "    return new Response(TAG);\n"
        "  },\n"
        "};\n" % (i, 0x9E3779B1 ^ (i * 2654435761))
    )


def _worker_service(name, module_src, bindings=""):
    return (
        '(name = "%s", worker = (\n'
        '  modules = [(name = "worker", esModule = embed "%s")],\n'
        "  %s\n"
        '  compatibilityDate = "%s"))' % (name, module_src, bindings, COMPAT_DATE)
    )


def write_multi_config(path, n, port_base):
    """One workerd holding n tenants, each on its own socket."""
    os.makedirs(path, exist_ok=True)
    services = []
    sockets = []
    for i in range(n):
        src = os.path.join(path, "t%d.js" % i)
        with open(src, "w") as f:
            f.write(tenant_module(i))
        services.append(_worker_service("w%d" % i, "t%d.js" % i))
        sockets.append(
            '(name = "s%d", address = "127.0.0.1:%d", http = (), service = "w%d")'
            % (i, port_base + i, i)
        )
    cfg = os.path.join(path, "config.capnp")
    with open(cfg, "w") as f:
        f.write('using Workerd = import "/workerd/workerd.capnp";\n')
        f.write("const config :Workerd.Config = (\n")
        f.write("  services = [\n    " + ",\n    ".join(services) + "\n  ],\n")
        f.write("  sockets = [\n    " + ",\n    ".join(sockets) + "\n  ]\n")
        f.write(");\n")
    return cfg


def write_single_configs(path, n, port_base):
    """n separate single-tenant workerd configs."""
    os.makedirs(path, exist_ok=True)
    out = []
    for i in range(n):
        d = os.path.join(path, "p%d" % i)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "t.js"), "w") as f:
            f.write(tenant_module(i))
        cfg = os.path.join(d, "config.capnp")
        with open(cfg, "w") as f:
            f.write('using Workerd = import "/workerd/workerd.capnp";\n')
            f.write("const config :Workerd.Config = (\n")
            f.write("  services = [" + _worker_service("w", "t.js") + "],\n")
            f.write(
                '  sockets = [(name = "s", address = "127.0.0.1:%d", http = (), service = "w")]\n'
                % (port_base + i)
            )
            f.write(");\n")
        out.append(cfg)
    return out


def launch(cfg, gk=False, extra_env=None, log=None, args=()):
    env = dict(os.environ)
    if gk:
        env["WORKERD_EXPERIMENTAL_GUEST_KERNEL"] = "1"
    else:
        env.pop("WORKERD_EXPERIMENTAL_GUEST_KERNEL", None)
    if extra_env:
        env.update(extra_env)
    out = open(log, "wb") if log else subprocess.DEVNULL
    p = subprocess.Popen(
        [WORKERD, "serve", cfg, *args],
        env=env,
        stdout=out,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    p._log = log
    return p


def wait_port(port, deadline, proc=None):
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            raise RuntimeError("workerd exited with %s" % proc.returncode)
        try:
            s = socket.create_connection(("127.0.0.1", port), 0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.01)
    return False


def http_get(port, path="/", timeout=20.0, sock=None):
    """One request, read exactly as far as Content-Length says.

    workerd keeps the connection open for five seconds whatever the request's
    Connection header says, so reading to end of stream would time every
    request at five seconds.
    """
    s = sock or socket.create_connection(("127.0.0.1", port), timeout)
    s.settimeout(timeout)
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode())
    buf = b""
    head = -1
    body = 0
    while True:
        if head < 0:
            i = buf.find(b"\r\n\r\n")
            if i >= 0:
                head = i + 4
                m = re.search(rb"[Cc]ontent-[Ll]ength:\s*(\d+)", buf[:head])
                body = int(m.group(1)) if m else 0
        if head >= 0 and len(buf) >= head + body:
            break
        b = s.recv(65536)
        if not b:
            break
        buf += b
    if sock is None:
        s.close()
    return buf


def warm_all(ports, timeout=600.0):
    """Drive one request into every tenant so its isolate really exists."""
    deadline = time.time() + timeout
    for p in ports:
        if not wait_port(p, deadline):
            raise RuntimeError("port %d never opened" % p)
        r = http_get(p)
        if b" 200 " not in r.split(b"\r\n")[0] and b"200 OK" not in r:
            raise RuntimeError("port %d: bad response %r" % (p, r[:120]))


SMAP_KEYS = (
    "Rss",
    "Pss",
    "Pss_Anon",
    "Pss_File",
    "Shared_Clean",
    "Shared_Dirty",
    "Private_Clean",
    "Private_Dirty",
    "Swap",
)


def smaps_rollup(pid):
    out = {k: 0 for k in SMAP_KEYS}
    try:
        with open("/proc/%d/smaps_rollup" % pid) as f:
            for line in f:
                m = re.match(r"^(\w+):\s+(\d+) kB", line)
                if m and m.group(1) in out:
                    out[m.group(1)] = int(m.group(2))
    except FileNotFoundError:
        return None
    out["Uss"] = out["Private_Clean"] + out["Private_Dirty"]
    return out


def proc_status(pid):
    out = {}
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                m = re.match(r"^(Vm\w+|Threads):\s+(\d+)", line)
                if m:
                    out[m.group(1)] = int(m.group(2))
    except FileNotFoundError:
        return {}
    return out


def meminfo():
    out = {}
    with open("/proc/meminfo") as f:
        for line in f:
            m = re.match(r"^(\w+):\s+(\d+) kB", line)
            if m:
                out[m.group(1)] = int(m.group(2))
    return out


def sys_used_kb():
    """Memory the system cannot hand out, page tables and slab included."""
    mi = meminfo()
    return mi["MemTotal"] - mi["MemAvailable"]


def pagetables_kb():
    return meminfo().get("PageTables", 0)


def collect(pids):
    """Totals across a set of processes, plus their per-process breakdown."""
    tot = {k: 0 for k in SMAP_KEYS}
    tot["Uss"] = 0
    vm = {"VmPTE": 0, "VmRSS": 0, "VmSize": 0, "Threads": 0}
    for pid in pids:
        r = smaps_rollup(pid)
        if r is None:
            raise RuntimeError("pid %d vanished" % pid)
        for k in tot:
            tot[k] += r[k]
        st = proc_status(pid)
        for k in vm:
            vm[k] += st.get(k, 0)
    tot.update(vm)
    return tot


def kill_all(procs, grace=10.0):
    for p in procs:
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.time() + grace
    for p in procs:
        while p.poll() is None and time.time() < deadline:
            time.sleep(0.02)
    for p in procs:
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()


def ols(xs, ys):
    """Least-squares slope, intercept and R^2."""
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx
    inter = my - slope * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (inter + slope * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0
    return slope, inter, r2


def jdump(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
    sys.stderr.write("wrote %s\n" % path)
