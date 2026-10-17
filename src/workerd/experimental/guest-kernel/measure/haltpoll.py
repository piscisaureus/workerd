#!/usr/bin/env python3
# The resident runner's sleeping arrangement, launch by launch: hello's
# one-connection latency and the process's CPU per request, with KVM's own
# per-vCPU halt-poll statistics (debugfs, read with sudo) and where the
# scheduler put the owner, the runner and the client, so that a slow launch
# can be told apart by what the kernel did rather than by its latency alone.
#
# Usage: haltpoll.py <out.json> <launches> [rounds-per-launch] [KEY=VAL ...]
#   KEY=VAL are extra environment for workerd (GK_RUNNER=resident ...).
#   GKM_DUR: seconds per latency round (default 8). GK=0 runs without the
#   guest kernel. CLIENT_CPU pins httpbench to that CPU.

import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

WORK = os.path.join(H.RUN_DIR, "haltpoll")
BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")
LAT_DUR = float(os.environ.get("GKM_DUR", "8"))
STATS = ["halt_exits", "halt_attempted_poll", "halt_successful_poll", "halt_poll_invalid", "halt_wakeup",
         "halt_poll_success_ns", "halt_poll_fail_ns", "halt_wait_ns", "exits", "nmi_injections"]

MODULE = 'export default { async fetch(req) { return new Response("tenant-0-aa55aa55"); } };\n'


def sudo_read(path):
    r = subprocess.run(["sudo", "-n", "cat", path], capture_output=True, text=True)
    return r.stdout


def vm_dirs(pid):
    out = subprocess.run(["sudo", "-n", "ls", "/sys/kernel/debug/kvm"], capture_output=True, text=True).stdout
    return ["/sys/kernel/debug/kvm/" + d for d in out.split() if d.startswith("%d-" % pid)]


def vcpu_stats(pid):
    """The VM's statistics (summed over its vCPUs by the kernel): {stat: value}."""
    tot = {}
    for d in vm_dirs(pid):
        for s in STATS:
            txt = sudo_read("%s/%s" % (d, s)).strip()
            if txt:
                tot[s] = tot.get(s, 0) + int(txt)
    return tot


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def thread_cpu(pid, tid):
    try:
        with open("/proc/%d/task/%d/stat" % (pid, tid)) as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return int(fields[36])  # the CPU it last ran on
    except OSError:
        return -1


def thread_time(pid, tid):
    try:
        with open("/proc/%d/task/%d/stat" % (pid, tid)) as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except OSError:
        return 0.0


def named_tids(pid, name):
    out = []
    for tid in os.listdir("/proc/%d/task" % pid):
        try:
            with open("/proc/%d/task/%s/comm" % (pid, tid)) as f:
                if f.read().strip() == name:
                    out.append(int(tid))
        except OSError:
            pass
    return out


def siblings(cpu):
    try:
        return open("/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list" % cpu).read().strip()
    except OSError:
        return "?"


def placement(wd_pid, client_pid):
    owner = thread_cpu(wd_pid, wd_pid)
    runners = [thread_cpu(wd_pid, t) for t in named_tids(wd_pid, "gk-vcpu-run")]
    client = [thread_cpu(client_pid, int(t)) for t in os.listdir("/proc/%d/task" % client_pid)]
    return {"owner": owner, "runner": runners, "client": client}


def bench_start(port, conns, seconds):
    args = [BENCH, "127.0.0.1", str(port), "/", str(conns), str(seconds), "0"]
    if os.environ.get("CLIENT_CPU"):
        args = ["taskset", "-c", os.environ["CLIENT_CPU"]] + args
    return subprocess.Popen(args, stdout=subprocess.PIPE, text=True)


def bench_result(p):
    f = p.communicate()[0].split()
    return {"total": int(f[0]), "p50_us": float(f[1]), "p99_us": float(f[3]), "rps": float(f[5])}


def launch_once(env, gk, rounds, idx):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    port = H.PORT_BASE
    with open(os.path.join(WORK, "t0.js"), "w") as f:
        f.write(MODULE)
    cfg = os.path.join(WORK, "c.capnp")
    with open(cfg, "w") as f:
        f.write('using Workerd = import "/workerd/workerd.capnp";\n'
                "const config :Workerd.Config = (\n"
                '  services = [(name = "w0", worker = (modules = [(name = "worker", esModule = embed "t0.js")],'
                ' compatibilityDate = "%s"))],\n'
                '  sockets = [(name = "s0", address = "127.0.0.1:%d", http = (), service = "w0")]\n);\n'
                % (H.COMPAT_DATE, port))
    p = H.launch(cfg, gk=gk, extra_env=env, log=os.path.join(WORK, "wd.log"))
    rows = []
    try:
        H.warm_all([port])
        bench_result(bench_start(port, 4, 6))  # let the compiler settle
        for rnd in range(rounds):
            s0 = vcpu_stats(p.pid) if gk else {}
            c0, t0 = cpu_seconds(p.pid), time.time()
            runner_t0 = sum(thread_time(p.pid, t) for t in named_tids(p.pid, "gk-vcpu-run"))
            b = bench_start(port, 1, LAT_DUR)
            places = []
            while b.poll() is None:
                time.sleep(0.5)
                places.append(placement(p.pid, b.pid))
            lat = bench_result(b)
            runner_t1 = sum(thread_time(p.pid, t) for t in named_tids(p.pid, "gk-vcpu-run"))
            c1, t1 = cpu_seconds(p.pid), time.time()
            s1 = vcpu_stats(p.pid) if gk else {}
            n = lat["total"]
            d = {k: s1.get(k, 0) - s0.get(k, 0) for k in STATS}
            per = {k: v / n for k, v in d.items()}
            row = {"launch": idx, "round": rnd, "p50_us": lat["p50_us"], "p99_us": lat["p99_us"],
                   "rps": lat["rps"], "requests": n,
                   "cpu_us": (c1 - c0) / n * 1e6, "cpus_busy": (c1 - c0) / (t1 - t0),
                   "runner_cpu_us": (runner_t1 - runner_t0) / n * 1e6,
                   "halt_per_req": per, "halt_delta": d, "placement": places}
            rows.append(row)
            print(line(row), flush=True)
    finally:
        H.kill_all([p])
        time.sleep(0.5)
    return rows


def line(r):
    h = r["halt_per_req"]
    pl = r["placement"]
    own = sorted(set(x["owner"] for x in pl))
    run = sorted(set(c for x in pl for c in x["runner"]))
    cli = sorted(set(c for x in pl for c in x["client"]))
    return ("L%d r%d  p50 %6.1f us  p99 %6.1f  rps %6.0f  cpu %5.1f us/req (%.2f CPUs, runner %5.1f)"
            " | halts/req %.2f  polls %.2f  poll-ok %.2f  wakeups %.2f  poll-ok %6.0f ns  poll-fail %6.0f ns"
            "  wait %6.0f ns  nmi %.2f | owner %s runner %s client %s"
            % (r["launch"], r["round"], r["p50_us"], r["p99_us"], r["rps"], r["cpu_us"], r["cpus_busy"],
               r["runner_cpu_us"], h["halt_exits"], h["halt_attempted_poll"], h["halt_successful_poll"],
               h["halt_wakeup"], h["halt_poll_success_ns"], h["halt_poll_fail_ns"], h["halt_wait_ns"],
               h["nmi_injections"], own, run, cli))


def main():
    out = sys.argv[1]
    launches = int(sys.argv[2])
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 and "=" not in sys.argv[3] else 1
    env = {}
    for a in sys.argv[3:]:
        if "=" in a:
            k, v = a.split("=", 1)
            env[k] = v
    gk = os.environ.get("GK", "1") != "0"
    mit = open("/sys/devices/system/cpu/vulnerabilities/vmscape").read().strip()
    hp = open("/sys/module/kvm/parameters/halt_poll_ns").read().strip()
    print("vmscape: %s; host halt_poll_ns: %s; env %s" % (mit, hp, env), flush=True)
    rows = []
    for i in range(launches):
        rows += launch_once(env, gk, rounds, i)
    mit2 = open("/sys/devices/system/cpu/vulnerabilities/vmscape").read().strip()
    hp2 = open("/sys/module/kvm/parameters/halt_poll_ns").read().strip()
    print("vmscape after: %s; host halt_poll_ns after: %s" % (mit2, hp2), flush=True)
    H.jdump(out, {"env": env, "gk": gk, "vmscape": [mit, mit2], "host_halt_poll_ns": [hp, hp2], "rows": rows})


if __name__ == "__main__":
    main()
