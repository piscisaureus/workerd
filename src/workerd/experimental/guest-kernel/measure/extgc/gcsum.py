#!/usr/bin/env python3
# Per-isolate GC summary of a --trace-gc log: counts, pauses, and the live size
# after collection.
# usage: gcsum.py <log>...
import collections
import re
import sys

PAT = re.compile(
    r"^\[(\d+):(0x[0-9a-f]+)\]\s+(\d+) ms: (Scavenge|Mark-Compact)[^0-9]*"
    r"([\d.]+) \(([\d.]+)\) -> ([\d.]+) \(([\d.]+)\) MB.*?([\d.]+) / ([\d.]+) ms"
)


def summarize(path):
    count = collections.Counter()
    pause = collections.defaultdict(float)
    longest = collections.defaultdict(float)
    live = collections.defaultdict(float)
    with open(path, errors="replace") as f:
        for line in f:
            m = PAT.match(line)
            if not m:
                continue
            key = (m.group(2), m.group(4))
            ms = float(m.group(9))
            count[key] += 1
            pause[key] += ms
            longest[key] = max(longest[key], ms)
            live[key] = max(live[key], float(m.group(7)))
    print("==", path)
    for key in sorted(count):
        print(
            f"  {key[0]} {key[1]:13s} n={count[key]:5d}"
            f" pause_total={pause[key]:9.1f}ms mean={pause[key] / count[key]:6.2f}ms"
            f" max={longest[key]:6.2f}ms live_after_max={live[key]:.1f}MB"
        )


for path in sys.argv[1:]:
    summarize(path)
