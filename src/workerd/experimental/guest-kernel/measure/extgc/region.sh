#!/usr/bin/env bash
# Watches both isolates' heap arena regions over the heap-region workload (the front and back
# workers of ../../heap-region-test.capnp), with V8's GC trace, for one arm of a comparison.
#
# Usage: region.sh <tag> <workerd> <capnp> [bursts] [burst_size]
#   The capnp is one of the region-*.capnp beside this script, which differ in the V8 flags they
#   set process-wide (a heap limit or none, --trace-gc). The environment is passed through to
#   workerd; the arm with the soft-limit collection off needs a binary with the uncommitted knob
#   described in cost.sh and WORKERD_GK_MEASURE_NO_EXTERNAL_GC=1. GKM_OUT names the directory
#   for logs and results (default /tmp/gk-extgc). Needs /dev/kvm, curl and bc, and a workerd
#   with per-isolate heap arenas (--//src/workerd/server:allocator=jemalloc).
set -u
tag=$1; W=$2; CFG=$3; bursts=${4:-12}; burst_size=${5:-250}
out_dir=${GKM_OUT:-/tmp/gk-extgc}
mkdir -p "$out_dir/logs" "$out_dir/res"
port=8913; plain_port=8912; front=http://127.0.0.1:$port
body_bytes=4194304
log=$out_dir/logs/region-$tag.log
out=$out_dir/res/region-$tag.txt
: > "$out"
body() { curl -s --max-time 300 "$1" || echo no-response; }
GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$W" serve --experimental "$CFG" --socket-addr "plain=127.0.0.1:$plain_port" \
  --socket-addr "front=127.0.0.1:$port" >"$log" 2>&1 &
pid=$!
stop() { kill -TERM "$pid" 2>/dev/null; for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done; kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }
trap stop EXIT
for _ in $(seq 1 200); do [ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] && break; kill -0 "$pid" 2>/dev/null || { echo "workerd exited" | tee -a "$out"; exit 1; }; sleep 0.1; done
grep -q "per-isolate heap arenas enabled" "$log" || { echo "no heap arenas" | tee -a "$out"; exit 1; }
[ "$(body "$front/")" = "front" ] || { echo "front never served" | tee -a "$out"; exit 1; }
# The figure `field` the probe route last logged for the isolate that served `url`.
fig() {
  grep -F "url = $1;" "$log" | grep -F "heap arena region" | tail -n 1 | grep -o "$2 = [0-9]*" | grep -o "[0-9]*$"
}
sample() {
  body "$front/__gk_heap_region" >/dev/null; body "$front/back-region" >/dev/null
  fr=$(fig "$front/__gk_heap_region" regionBytes); br=$(fig "http://back/__gk_heap_region" regionBytes)
  ba=$(fig "http://back/__gk_heap_region" stats.allocated); bx=$(fig "http://back/__gk_heap_region" externalMemory)
  mc=$(grep -c "Mark-Compact" "$log"); sc=$(grep -c "Scavenge" "$log")
}
mib() { echo $(( ${1:-0} / 1048576 )); }
got=$(body "$front/down?n=$body_bytes&count=50")
sample
echo "$tag warm-up: $got front_region=$(mib "$fr")MiB back_region=$(mib "$br")MiB back_alloc=$(mib "$ba")MiB back_ext=$(mib "$bx")MiB mc=$mc sc=$sc" | tee -a "$out"
served=0
for burst in $(seq 1 "$bursts"); do
  t0=$(date +%s.%N)
  got=$(body "$front/down?n=$body_bytes&count=$burst_size")
  t1=$(date +%s.%N)
  served=$((served + burst_size * body_bytes))
  sample
  alive=ok; grep -qE "unmappable fault|condemned" "$log" && alive=DEAD; kill -0 "$pid" 2>/dev/null || alive=EXITED
  printf '%s burst %2d: served %5d MiB; front_region %5d MiB; back_region %5d MiB; back_alloc %5d MiB; back_ext %4d MiB; mc %d sc %d; %.2fs; %s; %s\n' \
    "$tag" "$burst" "$(mib $served)" "$(mib "$fr")" "$(mib "$br")" "$(mib "$ba")" "$(mib "$bx")" "$mc" "$sc" "$(echo "$t1 - $t0" | bc)" "$alive" "$got" | tee -a "$out"
  [ $alive = ok ] || break
done
echo "DONE $tag" | tee -a "$out"
