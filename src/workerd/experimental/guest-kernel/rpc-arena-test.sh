#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), that an in-process Cap'n Proto call -- every JS
# RPC call and every service binding -- runs with nothing of it left in either isolate's heap
# arena.
#
# A call hands host-side objects both ways. The caller's turn builds the request message with its
# segments and capability table, the stubs its arguments serialize to with their membranes and
# pending events, and the call context and promise plumbing that sending makes; the callee's turn
# reads those, writes the response message the same way, and builds the stubs its return value
# serializes to; and each side then frees what the other built. Under per-isolate heap arenas any
# of that left in one isolate's arena is unaddressable from the other's turns, which fault on it
# and are condemned -- and a free faults as surely as a read, because `operator delete` is reached
# through the object's destructor.
#
# /rpc makes `n` calls that each take a stub from the callee, call a method on that stub, and
# dispose of it inside the caller's turn, so every stub's host-side plumbing is built in one
# isolate's turn and freed in the other's. /blob returns 4 KiB per call, which takes more than
# one message segment. /sub is a service-binding fetch whose response status and headers the
# caller reads. The script fails if any of them fails, if any isolate is condemned, or if the
# guest kernel reports an unmappable fault.
#
# Response *bodies* are deliberately not read: body bytes cross an in-process service binding by
# pointer and are not placed yet (see the Limitations in README.md), so reading one faults in
# `kj::AsyncPipe::tryRead` whatever this script checks.
#
# Negative builds (never shipped), each failing this script:
#   --copt=-DWORKERD_GK_NEG_RPC_SHARED_HEAP  the call's shared-heap windows are not entered, so
#       the first stub the caller disposes of, or the first capability either side passes through
#       the session's membrane, is in the other isolate's arena
#   --copt=-DWORKERD_GK_NEG_ENTRYPOINT_NAME_HEAP  a worker's entrypoint names stay in the heap
#       arena of the isolate whose modules declared them, and the caller's turn reads one when it
#       starts the subrequest
#   --copt=-DWORKERD_GK_NEG_TURN_JOURNAL_PAGES  the thread's resolve journal comes from the C++
#       heap, so the first isolate to run a turn on the thread puts it in its own arena and every
#       other isolate's turn faults reading it
#
# Usage: rpc-arena-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_RPC_ARENA_PORT overrides the caller's port (8867); the plain worker takes the port below.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_RPC_ARENA_PORT:-8867}
plain_port=$((port - 1))
caller=http://127.0.0.1:$port
rounds=${GK_RPC_ARENA_ROUNDS:-20}
log=$(mktemp)

cleanup() {
  kill "$pid" 2>/dev/null
  # A condemned isolate can leave the server not exiting on SIGTERM, which is a cost of the
  # fault and not something to wait out here.
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- faults and condemnations ---"
  grep -E "unmappable fault|isolate condemned" "$log" | head -n 6
  echo "--- workerd log, without the guest kernel's per-turn chatter ---"
  grep -vE "^\[gk\] vcpu [0-9]+(:? (syscall|.*: (wall|root switch)))" "$log" | tail -n 40
  exit 1
}
body() {
  curl -s --max-time 120 "$1" || echo no-response
}

GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
  "$workerd" serve --experimental "$here/rpc-arena-test.capnp" \
  --socket-addr "plain=127.0.0.1:$plain_port" --socket-addr "caller=127.0.0.1:$port" >"$log" 2>&1 &
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
  echo "SKIP: this workerd has no per-isolate heap arenas, so an in-process call has no arena"
  echo "      wall to cross. Build with --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
echo "workerd pid $pid serving the caller on $caller, with guest-kernel isolation and"
echo "per-isolate heap arenas; the plain worker holds the first arena"

for round in $(seq 1 "$rounds"); do
  for case in "rpc rpc?n=5 rpc=5" "blob blob?n=5 blob=20480" "sub sub?n=5 sub=1000"; do
    set -- $case
    got=$(body "$caller/$2")
    [ "$got" = "$3" ] || fail "round $round: GET $caller/$2 -> ${got:-no-response}, wanted $3"
  done
done
echo "1. $rounds rounds of GET /rpc?n=5, /blob?n=5 and /sub?n=5 all answered: $((rounds * 5))"
echo "   stubs taken from the callee, called and disposed of in the caller's turn,"
echo "   $((rounds * 5)) 4 KiB return values and $((rounds * 5)) service-binding fetches"

faults=$(grep -c "unmappable fault" "$log")
echo "2. unmappable faults reported by the guest kernel: $faults"
[ "$faults" = 0 ] || fail "$(grep -m1 "unmappable fault" "$log")"

condemned=$(grep -c "isolate condemned" "$log")
echo "3. isolates condemned: $condemned"
[ "$condemned" = 0 ] || fail "$(grep -m1 "isolate condemned" "$log")"

[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] ||
  fail "the plain worker stopped serving"
[ "$(body "$caller/")" = "caller" ] || fail "the caller stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
echo "4. all three workers still serve"

echo "PASS: $((rounds * 15)) in-process Cap'n Proto calls across three isolates with per-isolate heap arenas left nothing of themselves in either peer's arena: no turn faulted, no isolate was condemned, and every call returned what the other side built"
