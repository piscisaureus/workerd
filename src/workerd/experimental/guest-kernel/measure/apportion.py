#!/usr/bin/env python3
# Where a body's cost goes: the allocator, the guest kernel, or per-isolate
# placement -- and what differs between the fast and the slow launches of the
# shipping configuration.
#
# The same two workers and routes as bodyrelay.py (imported from there), run
# as a ladder of arms that each change one thing, every arm launched afresh
# in every round so that a launch-decided mode can be seen as such. Each
# launch is reported on its own, never as a median, with what was observable
# about it: where the owner and its runner were pinned, which CPU every thread
# of the process last ran on, gk's counters over each benchmark window (a
# temporary WORKERD_GK_STATS_PERIOD knob prints them to stderr), KVM's
# per-VM debugfs counters over the same window, the process's huge-page
# backing, and perf counters per thread over the window (the 4 MiB response
# the 4 MiB request and the bodiless request).
#
# Usage: apportion.py <out.json> <arm> [<arm> ...]
#   An arm is NAME:KEY=VAL,KEY=VAL. Two keys are the harness's: bin=<path>
#   names the workerd binary and gk=1 turns the guest kernel on. Every other
#   key goes into workerd's environment (GK_RUNNER=resident, MALLOC_CONF=...,
#   WORKERD_GK_SUPPRESS_HEAP_ARENAS=1, ...). Arms are interleaved round by
#   round, each round a fresh launch of each.
#   GKM_ROUNDS launches per arm (default 8), GKM_DUR seconds per benchmark
#   (default 8), GKM_CASES as in bodyrelay.py plus "apportion" (the default
#   here: the bodiless and 64 KiB and 4 MiB response, the 4 MiB request, and
#   the 4 MiB pump). GKM_PERF=0 skips perf. SERVER_CPUS and CLIENT_CPUS as in
#   bodyrelay.py.

import glob
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bodyrelay as B
import harness as H

WORK = os.path.join(H.RUN_DIR, "apportion")
DUR = float(os.environ.get("GKM_DUR", "8"))
ROUNDS = int(os.environ.get("GKM_ROUNDS", "8"))
PERF = os.environ.get("GKM_PERF", "1") == "1"
CONNS = tuple(int(c) for c in os.environ.get("GKM_CONNS", "1").split(","))

B.GROUPS["apportion"] = [
    c
    for c in B.GROUPS["main"]
    if c[0] in ("down/0", "down/65536", "down/4194304", "up/4194304")
] + [c for c in B.GROUPS["pump"] if c[0] == "upthru/4194304"]
CASES = B.GROUPS[os.environ.get("GKM_CASES", "apportion")]
# The windows perf counts over: a bodiless request and a 4 MiB response.
PERF_CASES = ("down/0", "down/4194304", "up/4194304")
# Five events, within Zen 5 programmable counters; one group, no multiplexing. :HG counts
# host and guest mode both, which under the guest kernel is the whole turn.
PERF_EVENTS = (
    "cycles:HG",
    "instructions:HG",
    "ls_l1_d_tlb_miss.all_l2_miss:HG",
    "ls_dmnd_fills_from_sys.dram_io_all:HG",
    "ls_dmnd_fills_from_sys.far_all:HG",
)
KVM_STATS = (
    "exits",
    "halt_exits",
    "halt_attempted_poll",
    "halt_successful_poll",
    "halt_wakeup",
    "halt_poll_success_ns",
    "halt_poll_fail_ns",
    "halt_wait_ns",
    "irq_injections",
    "io_exits",
    "mmio_exits",
    "pf_taken",
    "pf_fixed",
    "pf_emulate",
    "pf_mmio_spte_created",
    "nested_run",
    "pages_4k",
    "pages_2m",
    "pages_1g",
    "npt_fault",  # not on every kernel; missing names are skipped
)


def launch(binary, cfg, gk, env, log):
    e = dict(os.environ)
    if gk:
        e["WORKERD_EXPERIMENTAL_GUEST_KERNEL"] = "1"
        e.setdefault("WORKERD_GK_STATS_PERIOD", "500")
    else:
        e.pop("WORKERD_EXPERIMENTAL_GUEST_KERNEL", None)
    e.update(env)
    argv = ["taskset", "-c", B.SERVER_CPUS, binary, "serve", cfg]
    return subprocess.Popen(
        argv,
        env=e,
        stdout=open(log, "wb"),
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def threads(pid):
    """Every thread of the process: name, affinity, last CPU, CPU time."""
    out = {}
    for d in glob.glob("/proc/%d/task/*" % pid):
        tid = int(os.path.basename(d))
        try:
            with open(d + "/comm") as f:
                comm = f.read().strip()
            with open(d + "/status") as f:
                st = f.read()
            with open(d + "/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
        except OSError:
            continue
        m = re.search(r"Cpus_allowed_list:\s*(\S+)", st)
        out[tid] = {
            "comm": comm,
            "affinity": m.group(1) if m else None,
            "cpu": int(fields[36]),
            "cpu_s": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
        }
    return out


def pinning(pid):
    """The owner's and the runner's pinned CPUs, from their affinity masks."""
    th = threads(pid)
    owner = th.get(pid, {}).get("affinity")
    runners = [t["affinity"] for t in th.values() if t["comm"] == "gk-vcpu-run"]
    return {"owner": owner, "runners": runners, "nthreads": len(th)}


def gkstats_last(log):
    """The last gk counter line the process printed, as a dict."""
    try:
        with open(log, "rb") as f:
            f.seek(0, 2)
            n = f.tell()
            f.seek(max(0, n - 65536))
            data = f.read().decode("latin1")
    except OSError:
        return None
    lines = [ln for ln in data.split("\n") if ln.startswith("[gkstats] ")]
    if not lines:
        return None
    return {k: float(v) for k, v in re.findall(r"(\w+)=([-\d.]+)", lines[-1])}


def kvm_dirs(pid):
    # debugfs is root-only, listing included; reading is all that is done.
    r = subprocess.run(
        ["sudo", "-n", "ls", "/sys/kernel/debug/kvm"], capture_output=True, text=True
    )
    return sorted(
        "/sys/kernel/debug/kvm/" + d
        for d in r.stdout.split()
        if d.startswith("%d-" % pid)
    )


def kvm_stats(pid):
    """KVM's per-VM debugfs counters, summed over the process's VMs."""
    out = {}
    for d in kvm_dirs(pid):
        for name in KVM_STATS:
            p = os.path.join(d, name)
            r = subprocess.run(["sudo", "-n", "cat", p], capture_output=True, text=True)
            if r.returncode == 0:
                try:
                    out[name] = out.get(name, 0) + int(r.stdout.strip())
                except ValueError:
                    pass
    return out or None


def delta(a, b):
    if a is None or b is None:
        return None
    return {k: b[k] - a[k] for k in b if k in a}


def hugepages(pid):
    """AnonHugePages in total and for every mapping that has any."""
    out = {"AnonHugePages_kB": 0, "Rss_kB": 0, "mappings": []}
    try:
        with open("/proc/%d/smaps" % pid) as f:
            head = None
            size = 0
            for line in f:
                m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) ", line)
                if m:
                    head = m.group(1) + "-" + m.group(2)
                    size = int(m.group(2), 16) - int(m.group(1), 16)
                    continue
                m = re.match(r"^(\w+):\s+(\d+) kB", line)
                if not m:
                    continue
                k, v = m.group(1), int(m.group(2))
                if k == "Rss":
                    out["Rss_kB"] += v
                elif k == "AnonHugePages":
                    out["AnonHugePages_kB"] += v
                    if v > 0:
                        out["mappings"].append(
                            {"range": head, "size_kB": size // 1024, "huge_kB": v}
                        )
    except OSError:
        return None
    return out


def perf_stat(pid, seconds):
    """perf counters per thread of the process over `seconds`."""
    argv = [
        "perf",
        "stat",
        "-x",
        ",",
        "--per-thread",
        "-p",
        str(pid),
        "-e",
        ",".join(PERF_EVENTS),
        "--",
        "sleep",
        "%g" % seconds,
    ]
    r = subprocess.run(argv, capture_output=True, text=True)
    out = {}
    for line in r.stderr.splitlines():
        f = line.split(",")
        if len(f) < 4 or not f[0]:
            continue
        name, _, tid = f[0].rpartition("-")
        if f[1] in ("<not counted>", "<not supported>"):
            continue
        try:
            out.setdefault(name + "-" + tid, {})[f[3]] = int(f[1])
        except ValueError:
            pass
    return out


def run_arm(name, spec, rnd, port, cfg, launches):
    binary = spec.get("bin", H.WORKERD)
    gk = spec.get("gk", "0") == "1"
    env = {k: v for k, v in spec.items() if k not in ("bin", "gk")}
    log = os.path.join(WORK, "wd-%s-%d.log" % (name, rnd))
    p = launch(binary, cfg, gk, env, log)
    rec = {
        "arm": name,
        "round": rnd,
        "binary": binary,
        "gk": gk,
        "env": env,
        "pid": p.pid,
        "cases": {},
    }
    try:
        if not H.wait_port(port, time.time() + 120, p):
            raise RuntimeError("%s: port never opened" % name)
        for label, path, want in CASES:
            B.check(port, path, want)
        B.bench(port, "/down/1024", 4, 4)  # let the JIT settle
        rec["pinning"] = pinning(p.pid)
        rec["threads_after_warmup"] = threads(p.pid)
        for label, path, want in CASES:
            for conns in CONNS:
                key = "%s c%d" % (label, conns)
                c0, t0 = B.cpu_seconds(p.pid), time.time()
                g0, k0 = gkstats_last(log), kvm_stats(p.pid)
                perf = {}
                if PERF and label in PERF_CASES and conns == 1:
                    th = threading.Thread(
                        target=lambda: perf.update(perf_stat(p.pid, DUR - 1))
                    )
                    th.start()
                else:
                    th = None
                r = B.bench(port, path, conns, DUR)
                if th is not None:
                    th.join()
                c1, t1 = B.cpu_seconds(p.pid), time.time()
                g1, k1 = gkstats_last(log), kvm_stats(p.pid)
                r["cpu_us_per_req"] = (
                    (c1 - c0) / r["total"] * 1e6 if r["total"] else None
                )
                r["cpus_busy"] = (c1 - c0) / (t1 - t0)
                r["gk"] = delta(g0, g1)
                r["gk_abs"] = g1
                r["kvm_abs"] = k1
                r["kvm"] = delta(k0, k1)
                r["threads"] = threads(p.pid)
                if perf:
                    r["perf"] = perf
                if "4194304" in label:
                    r["hugepages"] = hugepages(p.pid)
                rec["cases"][key] = r
                print(
                    "r%d %-8s %-16s c=%d p50 %9.1f us  p99 %9.1f  rps %9.1f  "
                    "cpu %8.1f us  err %d  owner %s runner %s"
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
                        rec["pinning"]["owner"],
                        ",".join(rec["pinning"]["runners"]) or "-",
                    ),
                    flush=True,
                )
        if p.poll() is not None:
            raise RuntimeError("%s: workerd exited with %s" % (name, p.returncode))
    finally:
        H.kill_all([p])
    # What the binary said about its own configuration: the knobs' first-call
    # lines and the arena-support warning, so every arm's configuration is
    # confirmed by the process rather than by the arm's name.
    with open(log, "rb") as f:
        text = f.read().decode("latin1")
    rec["config_lines"] = [
        ln
        for ln in text.split("\n")
        if "[knob]" in ln or "heap arena" in ln or "guest-kernel:" in ln
    ][:12]
    launches.append(rec)


def main():
    out = sys.argv[1]
    arms = []
    for spec in sys.argv[2:]:
        name, _, rest = spec.partition(":")
        arms.append((name, dict(kv.split("=", 1) for kv in rest.split(",") if kv)))
    before = B.mitigations()
    print(
        "arms %s; server cpus %s; client cpus %s; rounds %d; dur %g"
        % (arms, B.SERVER_CPUS, B.CLIENT_CPUS, ROUNDS, DUR),
        flush=True,
    )
    print(
        "vmscape: %(vmscape)s; host halt_poll_ns: %(halt_poll_ns)s" % before, flush=True
    )
    shutil.rmtree(WORK, ignore_errors=True)
    port = H.PORT_BASE
    cfg = B.write_config(WORK, port)
    launches = []
    for rnd in range(ROUNDS):
        for name, spec in arms:
            run_arm(name, spec, rnd, port, cfg, launches)
            H.jdump(
                out,
                {
                    "arms": arms,
                    "rounds": ROUNDS,
                    "duration_s": DUR,
                    "server_cpus": B.SERVER_CPUS,
                    "client_cpus": B.CLIENT_CPUS,
                    "perf_events": PERF_EVENTS if PERF else [],
                    "mitigations": [before, B.mitigations()],
                    "launches": launches,
                },
            )
    after = B.mitigations()
    print(
        "vmscape after: %(vmscape)s; halt_poll_ns after: %(halt_poll_ns)s" % after,
        flush=True,
    )
    print()
    print("%-8s %-3s %-6s %-8s" % ("arm", "rnd", "owner", "runner"), end="")
    for label, _, _ in CASES:
        print(" %13s" % label, end="")
    print()
    for rec in launches:
        print(
            "%-8s %-3d %-6s %-8s"
            % (
                rec["arm"],
                rec["round"],
                rec["pinning"]["owner"],
                ",".join(rec["pinning"]["runners"]) or "-",
            ),
            end="",
        )
        for label, _, _ in CASES:
            r = rec["cases"].get("%s c1" % label)
            print(" %13.1f" % r["p50_us"] if r else " %13s" % "-", end="")
        print()


if __name__ == "__main__":
    main()
