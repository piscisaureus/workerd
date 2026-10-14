# Prints cycles and instructions per request from one perf-stat CSV plus the
# oha JSON for the same window.
#
# usage: perf-split.py <workload> <off|on> <perf.csv> <load.json>
import json, sys

w, mode, csv_path, load_path = sys.argv[1:5]

counts = {}
with open(csv_path) as f:
    for line in f:
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        value, event = parts[0], parts[2]
        if value in ("<not counted>", "<not supported>"):
            continue
        counts[event.strip()] = float(value)

with open(load_path) as f:
    d = json.load(f)
reqs = sum(d["statusCodeDistribution"].values())
if not reqs:
    print(f"{w} {mode}: no requests served")
    sys.exit(1)

# ":u"/":G" is the isolate's own execution, ":k"/":H" the kernel and, with the
# guest kernel on, the world-switch layer around it.
own = counts.get("cycles:u") or counts.get("cycles:G") or 0.0
other = counts.get("cycles:k") or counts.get("cycles:H") or 0.0
own_i = counts.get("instructions:u") or counts.get("instructions:G") or 0.0
other_i = counts.get("instructions:k") or counts.get("instructions:H") or 0.0

def ipc(i, c):
    return i / c if c else 0.0

print(f"{w:<7} {mode:<4} reqs={reqs:<8} "
      f"own={own/reqs:>10.0f} cyc/req (IPC {ipc(own_i, own):.2f})  "
      f"other={other/reqs:>10.0f} cyc/req (IPC {ipc(other_i, other):.2f})  "
      f"total={(own+other)/reqs:>10.0f} cyc/req")
