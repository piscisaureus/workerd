# Fits RSS = base + per_isolate * N over the series iso-ramp.sh recorded, and
# reports what the per-isolate difference means for isolate density.
#
# usage: iso-fit.py <iso-ramp.csv>
import csv, sys
from collections import defaultdict

rows = defaultdict(list)
with open(sys.argv[1]) as f:
    for r in csv.DictReader(f):
        rows[r["mode"]].append((int(r["n"]), float(r["vmrss_kb"]),
                                float(r["anon_huge_kb"])))

def fit(points):
    # Ordinary least squares on (n, rss).
    n = len(points)
    sx = sum(p[0] for p in points)
    sy = sum(p[1] for p in points)
    sxx = sum(p[0] * p[0] for p in points)
    sxy = sum(p[0] * p[1] for p in points)
    denom = n * sxx - sx * sx
    if denom == 0:
        return None, None
    slope = (n * sxy - sx * sy) / denom
    return slope, (sy - slope * sx) / n

results = {}
for mode in ("off", "on"):
    pts = rows.get(mode)
    if not pts:
        continue
    slope, base = fit(pts)
    huge = max(p[2] for p in pts)
    results[mode] = slope
    print(f"gk {mode}: base {base/1024:.1f} MB + {slope/1024:.2f} MB per "
          f"isolate (max AnonHugePages seen {huge/1024:.0f} MB, "
          f"{len(pts)} points)")

if len(results) == 2:
    off, on = results["off"], results["on"]
    print(f"delta: {(on-off)/1024:+.2f} MB per isolate "
          f"({(on/off-1)*100:+.0f}%)")
    # Marginal density: isolates per GB once the fixed base is paid.
    print(f"marginal density: {1024*1024/off:.0f} isolates/GB off, "
          f"{1024*1024/on:.0f} on ({(off/on-1)*100:.0f}% fewer)")
