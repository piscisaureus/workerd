#!/usr/bin/env bash
# Per-launch latency and throughput of workloads that do and do not report external memory in
# bulk, with the soft-limit collection of ExternalMemoryTarget (jsg/jsg.c++) on and off, the
# arms interleaved round by round so that each launch is its own measurement.
#
# The "off" arm needs a knob that is not committed, because it is a configuration nobody should
# ship: in applyLocked(), the collection is skipped when WORKERD_GK_MEASURE_NO_EXTERNAL_GC is
# set, with the report left in place. One binary then serves both arms and no build difference
# can enter the delta. The knob the measurement used was
#
#   } else if (total - low > EXTERNAL_MEMORY_SOFT_LIMIT && !measureNoExternalGc() &&
#       !collecting.load(std::memory_order_relaxed)) {
#
# with measureNoExternalGc() a function-local static read of getenv().
#
# Usage: cost.sh <tag> <workerd> <capnp> <rounds> <gk 0|1> [workloads...]
#   The capnp is one of the cost-*.capnp beside this script (a heap limit or none, --trace-gc or
#   not). A workload is "name port connections"; the default set is hello (c=1 and c=8), bigheap,
#   bigbody and bigheapbody, see the cost-*.js workers. GKM_OUT names the output directory
#   (default /tmp/gk-extgc), DUR and WARM the seconds measured and warmed per workload (6 and 2),
#   SERVER_CPUS and CLIENT_CPUS the pinning (two CCDs of this machine). Needs oha, jq, taskset
#   and, for gk=1, /dev/kvm.
set -u
tag=$1; W=$2; CFG=$3; R=$4; GK=$5; shift 5
wls=("$@"); [ ${#wls[@]} = 0 ] && wls=("hello 9101 1" "hello 9101 8" "bigheap 9102 1" "bigbody 9103 1" "bigheapbody 9104 1")
out_dir=${GKM_OUT:-/tmp/gk-extgc}
mkdir -p "$out_dir/logs" "$out_dir/res"
out=$out_dir/res/cost-$tag.txt
SERVER_CPUS=${SERVER_CPUS:-12-17,36-41}; CLIENT_CPUS=${CLIENT_CPUS:-0-5,24-29}
DUR=${DUR:-6}; WARM=${WARM:-2}
for r in $(seq 1 "$R"); do
  for arm in on off; do
    e=(); [ "$arm" = off ] && e+=(WORKERD_GK_MEASURE_NO_EXTERNAL_GC=1); [ "$GK" = 1 ] && e+=(WORKERD_EXPERIMENTAL_GUEST_KERNEL=1)
    log=$out_dir/logs/cost-$tag-$r-$arm.log
    env "${e[@]}" taskset -c "$SERVER_CPUS" "$W" serve "$CFG" >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 300); do curl -s -o /dev/null http://127.0.0.1:9101/ && break; kill -0 "$pid" 2>/dev/null || { echo "$tag round=$r arm=$arm workerd exited" >> "$out"; break; }; sleep 0.1; done
    for wl in "${wls[@]}"; do
      set -- $wl
      # One request first, so that the worker's isolate and any heap it builds exist before the
      # clock starts.
      curl -s -o /dev/null "http://127.0.0.1:$2/"
      taskset -c "$CLIENT_CPUS" oha -z "${WARM}s" -c "$3" --no-tui --output-format json "http://127.0.0.1:$2/" >/dev/null
      res=$(taskset -c "$CLIENT_CPUS" oha -z "${DUR}s" -c "$3" --no-tui --output-format json "http://127.0.0.1:$2/" |
        jq -r '"n=\(.statusCodeDistribution["200"] // 0) p50=\(.latencyPercentiles.p50 * 1e6 | floor) p90=\(.latencyPercentiles.p90 * 1e6 | floor) p99=\(.latencyPercentiles.p99 * 1e6 | floor) mean=\(.summary.average * 1e6 | floor) rps=\(.summary.requestsPerSec | floor) non200=\([.statusCodeDistribution | to_entries[] | select(.key != "200") | .value] | add // 0) errors=\([.errorDistribution | to_entries[] | .value] | add // 0)"')
      rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status")
      # Cumulative over the launch; a --trace-gc capnp makes them meaningful.
      mc=$(grep -c "Mark-Compact" "$log"); sc=$(grep -c "Scavenge" "$log")
      echo "$tag round=$r arm=$arm wl=$1 c=$3 $res rss_kb=$rss mc=$mc sc=$sc" | tee -a "$out"
    done
    kill -TERM "$pid" 2>/dev/null; for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done; kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  done
done
echo "DONE $tag" | tee -a "$out"
