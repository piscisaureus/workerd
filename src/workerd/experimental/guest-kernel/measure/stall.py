#!/usr/bin/env python3
# Where the guest kernel's per-request cycles go.
#
# One workerd, the guest kernel off and on in alternation, two handlers: hello,
# and a heavier one that builds, serializes and parses a 200-key object. For
# each: the latency a single connection sees, and then, under a four-connection
# load, hardware counters on the event-loop thread alone (the V8 background
# threads do their own work at their own rate and would blur the per-request
# figures), split into guest-mode and host-mode counts and divided by the
# requests served in the window.
#
# The split is what attributes the cost. Host-mode code runs natively: no
# nested paging, no arena root, nothing the guest kernel changes about its
# page walks. So cycles the host side loses over the same code with the guest
# kernel off cannot be page-walk or TLB cost; the branch-mispredict, return-
# mispredict and instruction-fill counters say what they are instead. On the
# guest side the TLB-miss and page-walk counters, priced by walk-cost.c, bound
# what nested paging can account for.
#
# A single tenant never switches page-table roots between turns (its vCPU's
# CR3 already names its arena), so the root switch's TLB refill is measured
# separately: two tenants in one process and a client whose requests alternate
# between them, so every turn runs under a different arena root than the one
# before and loads a new CR3.
#
# Usage: stall.py <out.json> [rounds [arrangement...]]   (needs sudo for perf; see run-all.sh)

import os
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

WORK = os.path.join(H.RUN_DIR, "stall")
BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "httpbench")
LAT_DUR = float(os.environ.get("GKM_DUR", "10"))
WINDOW = 5

MODULE = r"""
const TAG = "tenant-%d-aa55aa55";
const SALT = 7;
export default {
  async fetch(req) {
    const u = new URL(req.url);
    if (u.pathname === '/heavy') {
      const o = {};
      for (let i = 0; i < 200; i++) o['k' + i] = { n: i * SALT, s: 'v'.repeat(i %% 17), a: [i, i + 1, i + 2] };
      const s = JSON.stringify(o);
      const p = JSON.parse(s);
      let t = 0;
      for (const k in p) t += p[k].n + p[k].a.length;
      return new Response(t + ' ' + s.length);
    }
    return new Response(TAG);
  },
};
"""

# Event groups, each within the six counters the core has so nothing is
# multiplexed. :G counts guest mode, :H host mode (user and kernel); with the
# guest kernel off everything is host mode. The kvm tracepoints are software
# counters and take no counter slot.
GROUPS = [
    "cycles:G,cycles:H,instructions:G,instructions:H,kvm:kvm_exit,kvm:kvm_userspace_exit,"
    "kvm:kvm_halt_poll_ns,kvm:kvm_msi_set_irq,kvm:kvm_vcpu_wakeup,context-switches,cpu-migrations",
    "ex_ret_brn_misp:G,ex_ret_brn_misp:H,ex_ret_brn_ind_misp:G,ex_ret_brn_ind_misp:H,"
    "ex_ret_near_ret_mispred:G,ex_ret_near_ret_mispred:H",
    "ex_ret_brn:G,ex_ret_brn:H,ic_cache_fill_sys:G,ic_cache_fill_sys:H,"
    "ic_cache_fill_l2:G,ic_cache_fill_l2:H",
    "ls_l1_d_tlb_miss.all_l2_miss:G,ls_l1_d_tlb_miss.all_l2_miss:H,"
    "bp_l1_tlb_miss_l2_tlb_miss.all:G,bp_l1_tlb_miss_l2_tlb_miss.all:H,"
    "ls_tlb_flush.all:G,ls_tlb_flush.all:H",
    "ls_l1_d_tlb_miss.all:G,ls_l1_d_tlb_miss.all:H,bp_l1_tlb_miss_l2_tlb_hit:G,"
    "bp_l1_tlb_miss_l2_tlb_hit:H,op_cache_hit_miss.op_cache_miss:G,op_cache_hit_miss.op_cache_miss:H",
    "de_no_dispatch_per_slot.no_ops_from_frontend:G,de_no_dispatch_per_slot.no_ops_from_frontend:H,"
    "de_no_dispatch_per_slot.backend_stalls:G,de_no_dispatch_per_slot.backend_stalls:H,"
    "ls_dmnd_fills_from_sys.all:G,ls_dmnd_fills_from_sys.all:H",
    "ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss:G,ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss:H,"
    "ls_l1_d_tlb_miss.tlb_reload_2m_l2_miss:G,ls_l1_d_tlb_miss.tlb_reload_2m_l2_miss:H,"
    "bp_l1_tlb_miss_l2_tlb_miss.if2m:G,bp_l1_tlb_miss_l2_tlb_miss.if2m:H",
]

# The arrangements: (name, guest kernel, tenants, path, extra environment).
# The runner ones give the vCPU a runner thread (GK_RUNNER, see gk.c): its
# counters are added to the event-loop thread's, so the per-request figures
# cover both threads of the pair.
RUNNER_SIBLING = {"GK_RUNNER": "1", "GK_RUNNER_SPIN": "-1", "GK_RUNNER_PIN": "sibling"}
RUNNER_OTHER = {"GK_RUNNER": "1", "GK_RUNNER_SPIN": "-1", "GK_RUNNER_PIN": "other"}
RUNNER_OTHER_SLEEP = {"GK_RUNNER": "1", "GK_RUNNER_PIN": "other"}
RUNNER_FREE_SLEEP = {"GK_RUNNER": "1"}
# The resident ones keep the runner inside KVM_RUN across turns (GK_RUNNER=
# resident, see gk.c): the guest parks in a ring-0 loop, spinning or halted,
# and the owner spins or sleeps on an eventfd, with the same budget.
RESIDENT_OTHER = {"GK_RUNNER": "resident", "GK_RUNNER_SPIN": "-1", "GK_RUNNER_PIN": "other"}
RESIDENT_OTHER_SLEEP = {"GK_RUNNER": "resident", "GK_RUNNER_PIN": "other"}
ARRANGEMENTS = [
    ("off hello", False, 1, "/", {}),
    ("on hello", True, 1, "/", {}),
    ("on hello runner-sibling", True, 1, "/", RUNNER_SIBLING),
    ("on hello runner-other", True, 1, "/", RUNNER_OTHER),
    ("on hello runner-other-sleep", True, 1, "/", RUNNER_OTHER_SLEEP),
    ("on hello runner-free-sleep", True, 1, "/", RUNNER_FREE_SLEEP),
    ("on hello resident-other", True, 1, "/", RESIDENT_OTHER),
    ("on hello resident-other-sleep", True, 1, "/", RESIDENT_OTHER_SLEEP),
    ("off hello x2", False, 2, "/", {}),
    ("on hello x2", True, 2, "/", {}),
    ("on hello x2 runner-other", True, 2, "/", RUNNER_OTHER),
    ("on hello x2 resident-other", True, 2, "/", RESIDENT_OTHER),
    ("on hello x2 resident-other-sleep", True, 2, "/", RESIDENT_OTHER_SLEEP),
    ("off heavy", False, 1, "/heavy", {}),
    ("on heavy", True, 1, "/heavy", {}),
    ("on heavy runner-other", True, 1, "/heavy", RUNNER_OTHER),
    ("on heavy runner-other-sleep", True, 1, "/heavy", RUNNER_OTHER_SLEEP),
    ("on heavy resident-other", True, 1, "/heavy", RESIDENT_OTHER),
    ("on heavy resident-other-sleep", True, 1, "/heavy", RESIDENT_OTHER_SLEEP),
    ("off heavy x2", False, 2, "/heavy", {}),
    ("on heavy x2", True, 2, "/heavy", {}),
]


def perf(tids, seconds, events):
    """Counts per thread: {"comm-tid": {event: count}}, plus "all" summed."""
    r = subprocess.run(
        ["sudo", "-n", "perf", "stat", "-x,", "--per-thread", "-e", events, "-t", tids, "--", "sleep",
         str(seconds)], capture_output=True, text=True, timeout=seconds + 60)
    out = {"all": {}}
    for line in r.stderr.splitlines():
        f = line.split(",")
        if len(f) >= 4 and f[1].strip().isdigit():
            thread, count, event = f[0], int(f[1]), f[3]
            out.setdefault(thread, {})[event] = count
            out["all"][event] = out["all"].get(event, 0) + count
    if len(out) == 1:
        sys.stderr.write(r.stderr)
    return out


def bench_start(ports, path, conns, seconds):
    # Two ports: every connection alternates between them, one request in
    # flight at a time (see httpbench.c), so consecutive turns on the event
    # loop belong to different tenants and each one switches page-table roots.
    args = [BENCH, "127.0.0.1", str(ports[0]), path, str(conns), str(seconds), "0"]
    if len(ports) > 1:
        args.append(str(ports[1]))
    return subprocess.Popen(args, stdout=subprocess.PIPE, text=True)


def bench_result(p):
    f = p.communicate()[0].split()
    return {"total": int(f[0]), "p50_us": float(f[1]), "p99_us": float(f[3]), "rps": float(f[5])}


def bench(ports, path, conns, seconds):
    return bench_result(bench_start(ports, path, conns, seconds))


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")  # utime + stime


def tids(pid):
    """The event-loop thread (the main thread) and any vCPU runner threads."""
    out = [pid]
    for tid in os.listdir("/proc/%d/task" % pid):
        try:
            with open("/proc/%d/task/%s/comm" % (pid, tid)) as f:
                if f.read().strip() == "gk-vcpu-run":
                    out.append(int(tid))
        except OSError:
            pass
    return out


def measure(name, gk, tenants, path, env, rnd):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    ports = [H.PORT_BASE + i for i in range(tenants)]
    services, sockets = [], []
    for i in range(tenants):
        with open(os.path.join(WORK, "t%d.js" % i), "w") as f:
            f.write(MODULE % i)
        services.append('(name = "w%d", worker = (modules = [(name = "worker", esModule = embed "t%d.js")],'
                        ' compatibilityDate = "%s"))' % (i, i, H.COMPAT_DATE))
        sockets.append('(name = "s%d", address = "127.0.0.1:%d", http = (), service = "w%d")'
                       % (i, ports[i], i))
    cfg = os.path.join(WORK, "c.capnp")
    with open(cfg, "w") as f:
        f.write('using Workerd = import "/workerd/workerd.capnp";\n'
                "const config :Workerd.Config = (\n  services = [%s],\n  sockets = [%s]\n);\n"
                % (", ".join(services), ", ".join(sockets)))
    p = H.launch(cfg, gk=gk, extra_env=env, log=os.path.join(WORK, "wd.log"))
    try:
        H.warm_all(ports)
        bench(ports, path, 4, 6)  # let the compiler settle on its tiers first
        lat = bench(ports, path, 1, LAT_DUR)
        # The counter window's load: four connections for one tenant; for two,
        # a single alternating connection, so that every turn really follows
        # a turn of the other tenant (four would let the event loop serve the
        # same tenant several times in a row).
        load = bench_start(ports, path, 4 if tenants == 1 else 1, len(GROUPS) * WINDOW + 4)
        time.sleep(2.0)
        counts = {}
        threads = ",".join(str(t) for t in tids(p.pid))  # the main thread's tid is the pid
        cpu0, t0 = cpu_seconds(p.pid), time.time()
        for g in GROUPS:
            for thread, c in perf(threads, WINDOW, g).items():
                counts.setdefault(thread, {}).update(c)
        cpu1, t1 = cpu_seconds(p.pid), time.time()
        rps_load = bench_result(load)["rps"]
        n = rps_load * WINDOW
        per = {k: v / n for k, v in counts["all"].items()}
        # The owning (event-loop) thread's own counts, and the runner's, when
        # there is one: the runner's host-mode cycles are its handoff spin.
        owner = {k: v / n for k, v in counts.get("workerd-%d" % p.pid, {}).items()}
        runner = {}
        for thread, c in counts.items():
            if thread.startswith("gk-vcpu-run"):
                for k, v in c.items():
                    runner[k] = runner.get(k, 0) + v / n
        # The whole process's CPU time per request over the counter windows
        # (V8's background threads and any runner included), and the number
        # of logical CPUs that kept busy.
        cpu_us = (cpu1 - cpu0) / (rps_load * (t1 - t0)) * 1e6
        cpus_busy = (cpu1 - cpu0) / (t1 - t0)
    finally:
        H.kill_all([p])
        time.sleep(0.5)
    row = {"name": name, "gk": gk, "tenants": tenants, "path": path, "env": env, "round": rnd,
           "p50_us": lat["p50_us"], "p99_us": lat["p99_us"], "rps1": lat["rps"], "rps_load": rps_load,
           "cpu_us": cpu_us, "cpus_busy": cpus_busy, "per_req": per, "owner": owner, "runner": runner}
    print(line(name, rnd, row), flush=True)
    return row


def tot(c, e):
    return c.get(e + ":G", 0) + c.get(e + ":H", 0)


def line(name, rnd, row):
    c = row["per_req"]
    o = row.get("owner", {})
    owner = ("  owner H %7.0f cyc %7.0f insn misp %4.0f" % (o.get("cycles:H", 0), o.get("instructions:H", 0),
             o.get("ex_ret_brn_misp:H", 0))) if row.get("runner") else ""
    ipc = lambda s: (c["instructions:" + s] / c["cycles:" + s]) if c.get("cycles:" + s) else 0.0
    return ("%-22s r%s  p50 %6.1f us  rps %6.0f  cpu %5.1f us/req on %.2f CPUs | cycles %7.0f (G %7.0f H %7.0f)  insn %7.0f (G %7.0f H %7.0f)"
            "  IPC G %.2f H %.2f | misp %5.0f (G %4.0f H %4.0f) ind %4.0f ret %4.0f  ifill-sys %5.0f opc-miss %6.0f"
            " | walks d %3.0f i %3.0f (G d %3.0f i %3.0f)  flush %.2f  exits %.2f  uexits %.2f  cs %.3f migr %.4f%s"
            % (name, rnd, row["p50_us"], row["rps1"], row["cpu_us"], row["cpus_busy"], tot(c, "cycles"), c["cycles:G"], c["cycles:H"],
               tot(c, "instructions"), c["instructions:G"], c["instructions:H"], ipc("G"), ipc("H"),
               tot(c, "ex_ret_brn_misp"), c["ex_ret_brn_misp:G"], c["ex_ret_brn_misp:H"],
               tot(c, "ex_ret_brn_ind_misp"), tot(c, "ex_ret_near_ret_mispred"),
               tot(c, "ic_cache_fill_sys"), tot(c, "op_cache_hit_miss.op_cache_miss"),
               tot(c, "ls_l1_d_tlb_miss.all_l2_miss"), tot(c, "bp_l1_tlb_miss_l2_tlb_miss.all"),
               c["ls_l1_d_tlb_miss.all_l2_miss:G"], c["bp_l1_tlb_miss_l2_tlb_miss.all:G"],
               tot(c, "ls_tlb_flush.all"), c.get("kvm:kvm_exit", 0), c.get("kvm:kvm_userspace_exit", 0),
               c.get("context-switches", 0), c.get("cpu-migrations", 0), owner) +
            "  halts %.2f kicks %.2f wakeups %.2f" % (c.get("kvm:kvm_halt_poll_ns", 0), c.get("kvm:kvm_msi_set_irq", 0), c.get("kvm:kvm_vcpu_wakeup", 0)))


def main():
    out = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    only = sys.argv[3:]  # arrangement names to run; all when empty
    arrangements = [a for a in ARRANGEMENTS if not only or a[0] in only]
    rows = []
    for rnd in range(rounds):
        for name, gk, tenants, path, env in arrangements:
            rows.append(measure(name, gk, tenants, path, env, rnd))
    summary = {}
    for name, gk, tenants, path, env in arrangements:
        rs = [r for r in rows if r["name"] == name]
        med = lambda k: statistics.median(r[k] for r in rs)
        keys = rs[0]["per_req"].keys()
        summary[name] = {
            "gk": gk, "tenants": tenants, "path": path, "env": env,
            "p50_us": med("p50_us"), "p99_us": med("p99_us"), "rps1": med("rps1"), "rps_load": med("rps_load"),
            "cpu_us": med("cpu_us"), "cpus_busy": med("cpus_busy"),
            "per_req": {k: statistics.median(r["per_req"][k] for r in rs) for k in keys},
            "owner": {k: statistics.median(r["owner"].get(k, 0) for r in rs) for k in rs[0]["owner"]},
            "runner": {k: statistics.median(r["runner"].get(k, 0) for r in rs) for k in rs[0]["runner"]},
        }
    print()
    for name, v in summary.items():
        print(line(name, "med", v))
    H.jdump(out, {"rows": rows, "summary": summary, "cmdline": open("/proc/cmdline").read().strip(),
                  "vmscape": open("/sys/devices/system/cpu/vulnerabilities/vmscape").read().strip()})


main()
