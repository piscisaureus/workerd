#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), that an isolate serving large response bodies
# does not consume its heap arena region. The region is a fixed reservation inside the isolate's
# guest-kernel arena (16 GiB, GUEST_ARENA_HEAP_SIZE in jsg/setup.c++), and an isolate that
# exhausts it faults on its next allocation and is condemned, after which every request to it
# fails. A Response built from a buffer keeps a copy of the buffer (JsReadableStream::Buffer in
# api/js-readable-stream.h) until the Response is garbage collected, and a worker that serves
# such bodies allocates little else on the JS heap, so unless that memory is reported to V8 and
# acted on (ExternalMemoryTarget in jsg/jsg.c++), the copies accumulate until some unrelated
# collection, which for 4 MiB bodies is gigabytes later and may be never.
#
# "back" serves 4 MiB bodies from one buffer it keeps; "front" reads them whole across its
# service binding to back. The script serves more than enough of them to have exhausted back's
# region at the rate seen without the report (about 4 MB of region per response) and asserts,
# besides the absence of any fault or condemnation, that both isolates' regions plateau: once
# the workload has warmed up, a region may grow by at most a few of the soft limit's worth
# (EXTERNAL_MEMORY_SOFT_LIMIT, 64 MiB), whatever the bytes served, and over the second half of
# the run by at most one of jemalloc's growth steps. The offsets are read through the
# __gk_heap_region probe route in api/global-scope.c++, which logs HeapArena::getStats() for the
# isolate serving the request.
#
# Negative build (never shipped), failing this script on the growth of back's region:
#   --copt=-DWORKERD_GK_NEG_BODY_BUFFER_UNACCOUNTED  the copies are not reported to V8, so
#       back holds them until a collection V8 finds some other reason for
#
# Usage: heap-region-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_HEAP_REGION_PORT overrides front's port (8903); the plain worker takes the port below.
#   GK_HEAP_REGION_BURSTS and GK_HEAP_REGION_BURST_SIZE override the number of bursts (12) and
#   the responses per burst (250).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_HEAP_REGION_PORT:-8903}
plain_port=$((port - 1))
front=http://127.0.0.1:$port
bursts=${GK_HEAP_REGION_BURSTS:-12}
burst_size=${GK_HEAP_REGION_BURST_SIZE:-250}
body_bytes=4194304
log=$(mktemp)

# workerd drains in-flight requests on SIGTERM, so the stop is bounded: a few seconds, then
# SIGKILL. A probe that hangs instead of failing is worse than no probe.
stop_workerd() {
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -KILL "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}
cleanup() {
  stop_workerd
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- faults and condemnations ---"
  grep -E "unmappable fault|isolate condemned|has been condemned" "$log" | head -n 6
  echo "--- workerd log, without the guest kernel's per-turn chatter ---"
  grep -vE "^\[gk\] vcpu [0-9]+(:? (syscall|.*: (wall|root switch)))" "$log" | tail -n 40
  exit 1
}
body() {
  curl -s --max-time 120 "$1" || echo no-response
}

GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve --experimental "$here/heap-region-test.capnp" \
  --socket-addr "plain=127.0.0.1:$plain_port" --socket-addr "front=127.0.0.1:$port" >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 200); do
  [ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] || fail "the plain worker never served"
grep -q "guest-kernel isolation enabled" "$log" ||
  fail "the server is not running with guest-kernel isolation; the test would prove nothing"

if ! grep -q "per-isolate heap arenas enabled" "$log"; then
  echo "SKIP: this workerd has no per-isolate heap arenas, so there is no region to exhaust."
  echo "      Build with --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
[ "$(body "$front/")" = "front" ] || fail "front never served"
echo "workerd pid $pid serving front on $front, with guest-kernel isolation and per-isolate"
echo "heap arenas; the plain worker holds the first arena"

# The region bytes the probe route last logged for the isolate that served `url`.
region_of() {  # url
  grep -F "url = $1;" "$log" | grep -F "heap arena region" | tail -n 1 |
    grep -o "regionBytes = [0-9]*" | grep -o "[0-9]*$"
}
# Samples both isolates' regions and sets front_region and back_region (bytes).
sample() {
  [ "$(body "$front/__gk_heap_region")" = "front" ] || fail "front stopped serving"
  body "$front/back-region" | grep -q "back-region status=200" || fail "back stopped serving"
  front_region=$(region_of "$front/__gk_heap_region")
  back_region=$(region_of "http://back/__gk_heap_region")
  [ -n "$front_region" ] && [ -n "$back_region" ] ||
    fail "the probe route logged no region figure; is WORKERD_GK_TEST_PROBE honoured?"
}
mib() {
  echo $(( $1 / 1048576 ))
}
check_alive() {  # what
  if grep -qE "unmappable fault|condemned" "$log"; then
    fail "$1: $(grep -m1 -E "unmappable fault|condemned" "$log")"
  fi
  kill -0 "$pid" 2>/dev/null || fail "$1: workerd died"
}

step=0
step=$((step + 1))
got=$(body "$front/down?n=$body_bytes&count=50")
echo "$step. GET $front/down?n=$body_bytes&count=50 -> ${got:-no-response}"
[ "$got" = "down n=$body_bytes count=50 total=$((50 * body_bytes))" ] ||
  fail "the warm-up answered '${got:-no-response}'"
check_alive "warm-up"
sample
warm_front=$front_region
warm_back=$back_region
echo "   after warm-up: front region $(mib "$front_region") MiB, back region $(mib "$back_region") MiB"

step=$((step + 1))
echo "$step. $bursts bursts of $burst_size responses of $body_bytes bytes, read whole by front"
served=0
half_front=
half_back=
for burst in $(seq 1 "$bursts"); do
  got=$(body "$front/down?n=$body_bytes&count=$burst_size")
  [ "$got" = "down n=$body_bytes count=$burst_size total=$((burst_size * body_bytes))" ] ||
    fail "burst $burst answered '${got:-no-response}'"
  served=$((served + burst_size * body_bytes))
  check_alive "burst $burst"
  sample
  printf '   burst %2d: served %5d MiB in all; front region %5d MiB (+%d MiB since warm-up), back region %5d MiB (+%d MiB)\n' \
    "$burst" "$(mib $served)" "$(mib "$front_region")" "$(mib $((front_region - warm_front)))" \
    "$(mib "$back_region")" "$(mib $((back_region - warm_back)))"
  if [ "$burst" = $((bursts / 2)) ]; then
    half_front=$front_region
    half_back=$back_region
  fi
done

# Without the report, back's region grows by about 4 MB per response until a collection V8 runs
# for its own reasons, which is gigabytes in: here, with 12 GB served, several gigabytes. With
# it, what back holds is collected at every soft limit's worth of growth, so after the warm-up
# its region grows by at most a few of them, and over the second half of the run by at most one
# more growth step of jemalloc's retained pool (RETAIN_GROW_LIMIT in jsg/heap-arena.c++, 64 MiB).
grow_limit=$((256 * 1048576))
step_limit=$((64 * 1048576))
step=$((step + 1))
echo "$step. front region grew $(mib $((front_region - warm_front))) MiB over $(mib $served) MiB served (limit $(mib $grow_limit) MiB); $(mib $((front_region - half_front))) MiB over the second half (limit $(mib $step_limit) MiB)"
[ $((front_region - warm_front)) -le "$grow_limit" ] ||
  fail "front's heap arena region is not being reclaimed: it grew $(mib $((front_region - warm_front))) MiB over $(mib $served) MiB served"
[ $((front_region - half_front)) -le "$step_limit" ] ||
  fail "front's heap arena region did not plateau: it grew $(mib $((front_region - half_front))) MiB over the second half of the run"
step=$((step + 1))
echo "$step. back region grew $(mib $((back_region - warm_back))) MiB over the run (limit $(mib $grow_limit) MiB); $(mib $((back_region - half_back))) MiB over the second half (limit $(mib $step_limit) MiB)"
[ $((back_region - warm_back)) -le "$grow_limit" ] ||
  fail "back's heap arena region is not being reclaimed: it grew $(mib $((back_region - warm_back))) MiB over $(mib $served) MiB served"
[ $((back_region - half_back)) -le "$step_limit" ] ||
  fail "back's heap arena region did not plateau: it grew $(mib $((back_region - half_back))) MiB over the second half of the run"

faults=$(grep -c "unmappable fault" "$log")
step=$((step + 1))
echo "$step. unmappable faults reported by the guest kernel: $faults"
[ "$faults" = 0 ] || fail "$(grep -m1 "unmappable fault" "$log")"

condemned=$(grep -c "condemned" "$log")
step=$((step + 1))
echo "$step. isolates condemned: $condemned"
[ "$condemned" = 0 ] || fail "$(grep -m1 "condemned" "$log")"

[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] ||
  fail "the plain worker stopped serving"
[ "$(body "$front/")" = "front" ] || fail "front stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
step=$((step + 1))
echo "$step. all three workers still serve"

echo "PASS: front read $(( (served / body_bytes) + 50 )) response bodies of $body_bytes bytes from back across an in-process service binding, $(mib $served) MiB after warm-up, and both isolates' heap arena regions plateaued instead of filling: no turn faulted and no isolate was condemned"
