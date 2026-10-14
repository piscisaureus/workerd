#!/bin/bash
# Latency and throughput of every workload with guest-kernel isolation off and
# on, one workerd process per workload per mode.
#
#   WORKERD=/path/to/workerd ./bench.sh [workload ...]
#
# Defaults to all five workloads. See common.sh for the knobs (cores, port,
# duration, repeats) and README.md for what the numbers mean.
set -euo pipefail
cd "$(dirname "$0")"
. ./common.sh
require_tools
mkdir -p "$OUT"

workloads=("$@")
[ ${#workloads[@]} -gt 0 ] || workloads=(hello json cdn cpu alloc)

printf '%-7s %-4s %-4s %10s %10s %10s %10s %8s\n' \
  workload mode conc mean_us p50_us p99_us rps spread%
for w in "${workloads[@]}"; do
  cfg="$WORKLOADS/$w.capnp"
  [ -f "$cfg" ] || { echo "no such workload: $w" >&2; exit 1; }
  for mode in off on; do
    start_server "$mode" "$cfg"
    warm
    for conc in 1 8; do
      json=$(python3 ./oha-median.py "http://127.0.0.1:$PORT/" "$conc" "$DUR" \
               "$REPS" "$CLIENT_CPUS")
      echo "$json" > "$OUT/$w-$mode-c$conc.json"
      python3 - "$w" "$mode" "$conc" "$json" <<'PY'
import json, sys
w, mode, conc, raw = sys.argv[1:5]
d = json.loads(raw)
print(f"{w:<7} {mode:<4} {conc:<4} {d['mean_us']:10.1f} {d['p50_us']:10.1f} "
      f"{d['p99_us']:10.1f} {d['rps']:10.0f} {d['rps_spread_pct']:8.1f}")
PY
    done
    stop_server
  done
done

echo
echo "off vs on:"
python3 ./summarize.py "$OUT" "${workloads[@]}"
