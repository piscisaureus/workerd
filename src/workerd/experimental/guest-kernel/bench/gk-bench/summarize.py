# Turns the per-run JSON that bench.sh wrote into the off-vs-on delta table.
#
# usage: summarize.py <results-dir> <workload> [workload ...]
import json, os, sys

out, workloads = sys.argv[1], sys.argv[2:]

def load(w, mode, conc):
    p = os.path.join(out, f"{w}-{mode}-c{conc}.json")
    with open(p) as f:
        return json.load(f)

hdr = ("workload", "lat c=1 off", "on", "delta", "rps c=1 off", "on", "delta",
       "rps c=8 delta")
print("%-8s %12s %10s %8s %12s %10s %8s %14s" % hdr)
for w in workloads:
    try:
        o1, n1 = load(w, "off", 1), load(w, "on", 1)
        o8, n8 = load(w, "off", 8), load(w, "on", 8)
    except FileNotFoundError as e:
        print(f"{w:<8} missing: {e.filename}")
        continue
    dl = (n1["mean_us"] - o1["mean_us"]) / o1["mean_us"] * 100
    d1 = (n1["rps"] - o1["rps"]) / o1["rps"] * 100
    d8 = (n8["rps"] - o8["rps"]) / o8["rps"] * 100
    print(f"{w:<8} {o1['mean_us']:12.1f} {n1['mean_us']:10.1f} {dl:+7.0f}% "
          f"{o1['rps']:12.0f} {n1['rps']:10.0f} {d1:+7.0f}% {d8:+13.0f}%")
