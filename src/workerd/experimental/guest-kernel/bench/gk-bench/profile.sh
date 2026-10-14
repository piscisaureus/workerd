#!/bin/bash
# Splits each workload's per-request cost into guest-side and host-side cycles.
#
#   WORKERD=/path/to/workerd ./profile.sh [workload ...]
#
# On AMD + KVM the PMU does not count guest execution in the usual modes: a
# plain `perf stat -p <pid>` on a workerd that is running JS inside the guest
# reports a small fraction of the real cycles (and an impossible apparent
# clock), because the cycles spent in the guest land in neither `:u` nor `:k`.
# They have to be asked for explicitly with `:G`. Every number here therefore
# names its modifier, and the comparison is:
#
#   guest_extra = cycles:G(on) - cycles:u(off)    the runtime's own extra work
#   host_extra  = cycles:H(on) - cycles:k(off)    the added world-switch cost
#
# so that baseline work merely relocating behind a VM exit is not counted as
# new cost. Check that the reported frequency is the same in both modes before
# reading cycle deltas as time.
#
# Needs perf access: either run as root or set
# /proc/sys/kernel/perf_event_paranoid to 1 or less.
set -euo pipefail
cd "$(dirname "$0")"
. ./common.sh
require_tools perf
mkdir -p "$OUT"

WINDOW=${WINDOW:-20}
CONC=${CONC:-1}

workloads=("$@")
[ ${#workloads[@]} -gt 0 ] || workloads=(hello json cdn cpu alloc)

for w in "${workloads[@]}"; do
  cfg="$WORKLOADS/$w.capnp"
  [ -f "$cfg" ] || { echo "no such workload: $w" >&2; exit 1; }
  for mode in off on; do
    if [ "$mode" = on ]; then ev=cycles:G,cycles:H,instructions:G,instructions:H
    else ev=cycles:u,cycles:k,instructions:u,instructions:k; fi
    start_server "$mode" "$cfg"
    warm
    taskset -c "$CLIENT_CPUS" oha -z "${WINDOW}s" -c "$CONC" --no-tui --output-format json \
      "http://127.0.0.1:$PORT/" > "$OUT/$w-$mode-load.json" 2>/dev/null &
    load=$!
    perf stat -e "$ev" -p "$SRV_PID" -x, -o "$OUT/$w-$mode-perf.csv" \
      -- sleep "$WINDOW" || true
    wait $load
    stop_server
    python3 ./perf-split.py "$w" "$mode" "$OUT/$w-$mode-perf.csv" \
      "$OUT/$w-$mode-load.json"
  done
done
