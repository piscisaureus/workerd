#!/usr/bin/env bash
# Compares workerd with the guest kernel off and on: per-request latency and throughput for a
# set of workloads, and resident memory per idle isolate. Both modes run the same binary,
# toggled by WORKERD_EXPERIMENTAL_GUEST_KERNEL.
#
# Workloads (bench.capnp): hello (trivial fetch), cpu (compute-bound JS), json
# (parse/serialize), cdn (subrequest fetch and forward over a service binding, two isolates per
# request), alloc (a fresh 4MB buffer per request).
#
# Each workload/concurrency pair gets a 2s warmup and then three 5s oha runs; the median run's
# mean latency and throughput are reported. A run in which a request stalls past the end of the
# run is repeated, and the number of such runs is reported alongside. workerd is pinned to the first 4 CPUs and oha to
# the CPUs after them.
#
# Usage: bench.sh [latency|memory|all] [path/to/workerd]
#   defaults to all and bazel-bin/src/workerd/server/workerd in the repo root.
#   Needs /dev/kvm, curl, jq, taskset and oha (https://github.com/hatoo/oha).
#   BENCH_SERVER_CPUS / BENCH_CLIENT_CPUS override the pinning (0-3 / 4-11).
#   BENCH_DURATION overrides the run length (5s); BENCH_RUNS the run count (3).
#   BENCH_ISOLATES overrides the isolate counts of the memory fit (1 25 50 100 150).
#   BENCH_MODES overrides the modes measured (off on); the tables need both.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../.." && pwd)
what=${1:-all}
workerd=${2:-$root/bazel-bin/src/workerd/server/workerd}
server_cpus=${BENCH_SERVER_CPUS:-0-3}
client_cpus=${BENCH_CLIENT_CPUS:-4-11}
duration=${BENCH_DURATION:-5s}
runs=${BENCH_RUNS:-3}
isolate_counts=${BENCH_ISOLATES:-1 25 50 100 150}
modes=${BENCH_MODES:-off on}

pid=
tmp=$(mktemp -d)
cleanup() {
  [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*" >&2
  [ -f "$tmp/workerd.log" ] && { echo "--- workerd log ---" >&2; cat "$tmp/workerd.log" >&2; }
  exit 1
}

# start_workerd <off|on> <config> <url to wait for>
start_workerd() {
  local gk=()
  [ "$1" = on ] && gk=(WORKERD_EXPERIMENTAL_GUEST_KERNEL=1)
  env "${gk[@]}" taskset -c "$server_cpus" "$workerd" serve "$2" >"$tmp/workerd.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 300); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$3" || true)" = 200 ] && return
    kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
    sleep 0.1
  done
  fail "workerd did not start serving $3"
}
stop_workerd() {
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  pid=
}

# run_oha <url> <concurrency> <duration>: prints "mean_seconds rps", failing on any non-200 or
# failed request. Returns 2 if requests stalled: oha cuts off the requests in flight at the
# deadline, at most one per connection, so more deadline aborts than that, or no response at
# all, means some request took longer than the run.
run_oha() {
  local out
  out=$(taskset -c "$client_cpus" oha -z "$3" -c "$2" --no-tui --output-format json "$1") ||
    fail "oha failed on $1"
  local bad
  bad=$(echo "$out" | jq '[.statusCodeDistribution | to_entries[] | select(.key != "200") | .value]
                          | add // 0')
  [ "$bad" = 0 ] || fail "$bad non-200 responses from $1"
  echo "$out" | jq -e '.errorDistribution | del(.["aborted due to deadline"]) | length == 0' \
    >/dev/null || fail "requests to $1 failed: $(echo "$out" | jq -c .errorDistribution)"
  echo "$out" | jq -e --argjson c "$2" '.summary.average != null
      and (.errorDistribution["aborted due to deadline"] // 0) <= $c' >/dev/null || return 2
  echo "$out" | jq -r '"\(.summary.average) \(.summary.requestsPerSec)"'
}

# The median (by mean latency) of $runs runs, as "mean_seconds rps stalled_runs". A run in which
# requests stalled is discarded and repeated, up to 5 times per run, and counted.
measure() {
  run_oha "$1" "$2" 2s >/dev/null
  local stalled=0 r rc
  for _ in $(seq 1 "$runs"); do
    for attempt in 1 2 3 4 5; do
      r=$(run_oha "$1" "$2" "$duration") && break
      rc=$?
      [ "$rc" = 2 ] || exit 1
      stalled=$((stalled + 1))
      [ "$attempt" = 5 ] && fail "requests to $1 stalled in 5 consecutive runs"
    done
    echo "$r"
  done >"$tmp/runs"
  echo "$(sort -g "$tmp/runs" | sed -n "$(((runs + 1) / 2))p") $stalled"
}

latency() {
  local cases=("hello 9001 1" "hello 9001 8" "cpu 9002 1" "json 9003 1" "cdn 9004 1" "alloc 9005 1")
  declare -A res
  for mode in $modes; do
    start_workerd "$mode" "$here/bench.capnp" http://127.0.0.1:9001/
    for c in "${cases[@]}"; do
      set -- $c
      res[$mode,$1,$3]=$(measure "http://127.0.0.1:$2/" "$3")
      [ -n "${res[$mode,$1,$3]}" ] || fail "no result for gk $mode $1 c=$3"
      echo "  gk $mode $1 c=$3: ${res[$mode,$1,$3]}" >&2
    done
    stop_workerd
  done
  echo
  echo "| workload | conc | off mean | on mean | Δ latency | off rps | on rps | Δ rps | stalled runs (off/on) |"
  echo "|---|---|---|---|---|---|---|---|---|"
  for c in "${cases[@]}"; do
    set -- $c
    read -r om orps os <<<"${res[off,$1,$3]}"
    read -r nm nrps ns <<<"${res[on,$1,$3]}"
    awk -v w="$1" -v c="$3" -v om="$om" -v nm="$nm" -v orps="$orps" -v nrps="$nrps" \
        -v os="$os" -v ns="$ns" 'BEGIN {
      printf "| %s | %s | %.0fus | %.0fus | %+.0f%% | %.0f | %.0f | %+.0f%% | %d/%d |\n",
        w, c, om * 1e6, nm * 1e6, (nm / om - 1) * 100, orps, nrps, (nrps / orps - 1) * 100, os, ns }'
  done
}

# A config of n idle hello workers, each its own isolate on its own port from 10000.
write_isolates_config() {
  local n=$1 f=$2
  {
    echo 'using Workerd = import "/workerd/workerd.capnp";'
    echo 'const config :Workerd.Config = ('
    echo '  services = ['
    for i in $(seq 1 "$n"); do
      echo "    (name = \"w$i\", worker = (modules = [(name = \"worker\", esModule = embed \"hello.js\")], compatibilityDate = \"2024-01-01\")),"
    done
    echo '  ],'
    echo '  sockets = ['
    for i in $(seq 1 "$n"); do
      echo "    (name = \"w$i\", address = \"127.0.0.1:$((10000 + i))\", http = (), service = \"w$i\"),"
    done
    echo '  ],'
    echo ');'
  } >"$f"
}

# Resident memory after every isolate has served one request, fit linearly over the isolate
# counts: base + per-isolate slope.
memory() {
  declare -A rss
  cp "$here/hello.js" "$tmp/"
  for n in $isolate_counts; do
    write_isolates_config "$n" "$tmp/isolates-$n.capnp"
  done
  for mode in $modes; do
    for n in $isolate_counts; do
      start_workerd "$mode" "$tmp/isolates-$n.capnp" http://127.0.0.1:10001/
      for i in $(seq 1 "$n"); do
        curl -s -o /dev/null "http://127.0.0.1:$((10000 + i))/" || fail "isolate $i did not serve"
      done
      sleep 1
      rss[$mode,$n]=$(awk '/^VmRSS/ { print $2 }' "/proc/$pid/status")
      echo "  gk $mode, $n isolates: ${rss[$mode,$n]} kB" >&2
      stop_workerd
    done
  done
  echo
  echo "| mode | base | per isolate |"
  echo "|---|---|---|"
  for mode in off on; do
    for n in $isolate_counts; do echo "$n ${rss[$mode,$n]}"; done |
      awk -v mode="$mode" '{ x += $1; y += $2; xx += $1 * $1; xy += $1 * $2; k++ } END {
        b = (k * xy - x * y) / (k * xx - x * x); a = (y - b * x) / k
        printf "| gk %s | %.1f MB | %.2f MB |\n", mode, a / 1024, b / 1024 }'
  done
}

case $what in
  latency) latency ;;
  memory) memory ;;
  all) latency; memory ;;
  *) fail "unknown mode $what (latency, memory or all)" ;;
esac
