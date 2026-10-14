# Runs oha REPS times against one URL at one concurrency and prints the median
# of each statistic. A single oha run of a few seconds varies by several
# percent on a loaded machine, and the guest-kernel deltas being measured are
# sometimes that small, so every published number is a median of repeats.
#
# usage: oha-median.py <url> <concurrency> <duration> <reps> [taskset-cpus]
import json, statistics, subprocess, sys

url, conc, dur, reps = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
cpus = sys.argv[5] if len(sys.argv) > 5 else None

runs = []
for _ in range(reps):
    cmd = ["oha", "-z", dur, "-c", conc, "--no-tui",
           "--output-format", "json", url]
    if cpus:
        cmd = ["taskset", "-c", cpus] + cmd
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    d = json.loads(out)
    s, p = d["summary"], d["latencyPercentiles"]
    if s.get("successRate", 1.0) < 1.0:
        print(f"warning: successRate {s['successRate']} at c={conc}", file=sys.stderr)
    runs.append({
        "rps": s["requestsPerSec"],
        "mean_us": s["average"] * 1e6,
        "p50_us": p["p50"] * 1e6,
        "p99_us": p["p99"] * 1e6,
    })

med = {k: statistics.median(r[k] for r in runs) for k in runs[0]}
# Spread of the repeats, so a reader can tell a real delta from run-to-run noise.
rps = [r["rps"] for r in runs]
med["rps_spread_pct"] = (max(rps) - min(rps)) / statistics.median(rps) * 100
print(json.dumps(med))
