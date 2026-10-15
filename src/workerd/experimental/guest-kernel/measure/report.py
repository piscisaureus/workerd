#!/usr/bin/env python3
# Prints the tables that go in findings.md from the JSON the measurements left
# in results/, so the document's numbers come from the runs rather than from
# transcription.
#
# Usage: report.py [results-dir]

import glob
import json
import os
import sys

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "results")


def load(name):
    p = os.path.join(D, name)
    if not os.path.exists(p):
        return None
    with open(p) as f:
        return json.load(f)


def mb(kb):
    return kb / 1024.0


print("## 1. Marginal memory per tenant (opt build, least squares over the sweep)\n")
print("| arrangement | marginal USS | marginal PSS | marginal system | fixed (PSS intercept) | R2 (PSS) |")
print("| --- | --- | --- | --- | --- | --- |")
names = {
    "shared": "one process, guest kernel off",
    "gk": "one process, guest kernel on",
    "procs": "one process per tenant, launched independently",
    "procs-ksm": "one process per tenant, pages deduplicated (KSM)",
    "procs-uds": "one process per tenant, crossing over a unix socket",
    "procs-gk": "one process per tenant, guest kernel on",
}
marg = {}
for mode in ("shared", "gk", "procs", "procs-ksm"):
    j = load("mem-%s-opt.json" % mode)
    if not j:
        continue
    f = j["fits"]
    marg[mode] = f
    print("| %s | %.2f MB | %.2f MB | %.2f MB | %.1f MB | %.4f |" % (
        names[mode], mb(f["Uss"]["slope_kb"]), mb(f["Pss"]["slope_kb"]),
        mb(f["sys_used_delta"]["slope_kb"]), mb(f["Pss"]["intercept_kb"]),
        f["Pss"]["r2"]))

if "gk" in marg:
    g = marg["gk"]
    print()
    for mode in ("shared", "procs", "procs-ksm"):
        if mode not in marg:
            continue
        m = marg[mode]
        print("%-10s vs gk: PSS x%.2f, system x%.2f" % (
            mode, m["Pss"]["slope_kb"] / g["Pss"]["slope_kb"],
            m["sys_used_delta"]["slope_kb"] / g["sys_used_delta"]["slope_kb"]))
    print()
    for mode in ("shared", "gk", "procs", "procs-ksm"):
        if mode not in marg:
            continue
        print("%-10s tenants per GB at the margin (PSS): %.0f" % (
            mode, 1024.0 * 1024.0 / marg[mode]["Pss"]["slope_kb"]))

print("\n## Tenants that differ from each other\n")
j = load("mem-diverse.json")
if j:
    print("| arrangement | marginal USS | marginal PSS | marginal system |")
    print("| --- | --- | --- | --- |")
    for mode in ("shared", "gk", "procs", "procs-ksm"):
        m = j["marginal_kb"][mode]
        print("| %s | %.2f MB | %.2f MB | %.2f MB |" % (
            names[mode], mb(m["Uss"]), mb(m["Pss"]), mb(m["sys_used_delta"])))

print("\n## 2. Cross-tenant dispatch (one connection)\n")
print("| arrangement | own-tenant p50 | second-tenant p50 | crossing | crossing share |")
print("| --- | --- | --- | --- | --- |")
for mode in ("shared", "gk", "procs", "procs-uds"):
    rows = [load(os.path.basename(p)) for p in
            sorted(glob.glob(os.path.join(D, "disp-%s-opt-*.json" % mode)))]
    rows = [r for r in rows if r]
    if not rows:
        continue
    solo = sum(r["solo_c1"]["p50_us"] for r in rows) / len(rows)
    front = sum(r["front_c1"]["p50_us"] for r in rows) / len(rows)
    print("| %s | %.1f us | %.1f us | %.1f us | %.0f%% |" % (
        names.get(mode, mode), solo, front, front - solo,
        100.0 * (front - solo) / front))

print("\n## CPU per request and VM exits\n")
print("| arrangement | hello CPU | hello p50 | hello rps (one process) | exits/req | 1k-loop CPU |")
print("| --- | --- | --- | --- | --- | --- |")
for mode in ("shared", "gk", "procs"):
    j = load("cpu2-%s-opt.json" % mode) or load("cpu-%s-opt.json" % mode)
    if not j:
        continue
    h, w = j["hello"], j["loop1k"]
    print("| %s | %.1f us | %.1f us | %.0f | %s | %.1f us |" % (
        names[mode], h["cpu_us_per_req"], h["p50_us"], h["rps"],
        ("%.1f" % h["kvm_exits_per_req"]) if h.get("kvm_exits_per_req") else "-",
        w["cpu_us_per_req"]))

print("\n## Aggregate rate with every tenant busy\n")
print("| arrangement | tenants | total rps | rps/tenant | median tenant p50 | "
      "logical CPUs busy | rps per logical CPU |")
print("| --- | --- | --- | --- | --- | --- | --- |")
for n in (16, 24):
    for mode in ("shared", "gk", "procs", "procs-gk"):
        j = load("thr2-%s-%d.json" % (mode, n)) or load("thr-%s-%d.json" % (mode, n))
        if not j:
            continue
        print("| %s | %d | %.0f | %.0f | %.1f us | %.2f | %.0f |" % (
            names[mode], j["n"], j["total_rps"], j["rps_per_tenant"],
            j["p50_us_median_tenant"], j["server_cores_busy"],
            j["rps_per_core"] or 0))

print("\n## 3. Cold start\n")
print("| way in | guest kernel | time to first response | warm baseline | attributable to the new isolate |")
print("| --- | --- | --- | --- | --- |")
for mode in ("shared", "gk"):
    j = load("cs-loader-%s-opt.json" % mode)
    if j:
        print("| new worker in a running process | %s | %.0f us | %.0f us | %.0f us |" % (
            "off" if mode == "shared" else "on", j["cold"]["p50_us"],
            j["warm"]["p50_us"], j["isolate_create_p50_us"]))
for mode in ("shared", "gk"):
    j = load("cs-spawn-%s-opt.json" % mode)
    if j:
        print("| a whole new process | %s | %.0f us | - | %.0f us (all of it) |" % (
            "off" if mode == "shared" else "on", j["spawn"]["p50_us"],
            j["spawn"]["p50_us"]))

j = load("diag.json")
if j:
    print("\n## Where the guest kernel's per-request cost goes\n")
    print("| guest kernel | worker | cycles/req | instructions/req | IPC | exits/req | minor faults/req |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for r in j["rows"]:
        print("| %s | %s | %.0f | %.0f | %s | %.2f | %.2f |" % (
            "on" if r["gk"] else "off", r["path"], r["cycles_per_req"],
            r["instructions_per_req"], ("%.2f" % r["ipc"]) if r["ipc"] else "-",
            r["kvm_exits_per_req"], r["minflt_per_req"]))

for path in sorted(glob.glob(os.path.join(D, "stall*.json"))):
    j = load(os.path.basename(path))
    print("\n## Where the cycles go, per request on the event-loop thread and its runner: %s (%s)\n"
          % (os.path.basename(path), j["vmscape"]))
    print("| arrangement | p50 | rps | CPU us/req | CPUs busy | cycles/req | guest | host | instructions/req | IPC guest | IPC host | mispredicts | i-fills past L2 | page walks | root switches |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for k, v in j["summary"].items():
        c = v["per_req"]
        tot = lambda e: c.get(e + ":G", 0) + c.get(e + ":H", 0)
        ipc = lambda s: (c["instructions:" + s] / c["cycles:" + s]) if c.get("cycles:" + s) else 0.0
        print("| %s | %.1f us | %.0f | %.1f | %.2f | %.0f | %.0f | %.0f | %.0f | %.2f | %.2f | %.0f | %.0f | %.0f | %.2f |" % (
            k, v["p50_us"], v["rps1"], v.get("cpu_us", 0), v.get("cpus_busy", 0),
            tot("cycles"), c["cycles:G"], c["cycles:H"], tot("instructions"), ipc("G"), ipc("H"),
            tot("ex_ret_brn_misp"), tot("ic_cache_fill_sys"),
            tot("ls_l1_d_tlb_miss.all_l2_miss") + tot("bp_l1_tlb_miss_l2_tlb_miss.all"),
            c.get("ls_tlb_flush.all:G", 0)))

print("\n## The published figures, re-run here\n")
for mode in ("shared", "gk"):
    j = load("disp-%s-fastbuild.json" % mode)
    if j:
        print("fastbuild hello p50, %s: %.1f us" % (mode, j["solo_c1"]["p50_us"]))
for mode in ("shared", "gk"):
    rows = [load(os.path.basename(p)) for p in
            sorted(glob.glob(os.path.join(D, "disp-%s-opt-*.json" % mode)))]
    rows = [r for r in rows if r]
    if rows:
        print("opt hello p50, %s: %.1f us" % (
            mode, sum(r["solo_c1"]["p50_us"] for r in rows) / len(rows)))
