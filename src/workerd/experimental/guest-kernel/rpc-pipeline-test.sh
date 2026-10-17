#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), that Cap'n Proto promise pipelining over an
# in-process JS RPC binding -- `binding.foo().bar()`, a call made on the promise of an earlier
# call's result -- leaves neither isolate's turn reaching the other's arena.
#
# Pipelining is the one way a worker's JS extends a cross-isolate kj promise chain: every call
# made on a pending promise adds a queued call and a pipeline to forward it, and the chain is
# as long as JS makes it. What keeps the crossing safe is where the chain is placed and where it
# fires, both described at the windows in api/worker-rpc.c++: the queued-call plumbing is built
# inside the caller's `callImpl()` window, so it is in the shared heap, and it is forwarded and
# resolved only from the event loop, outside any turn, because `LocalClient::call` defers every
# in-process dispatch through `evalLater()` and the callee never resolves a pipeline from its
# turn. This script is the check that the reasoning holds against a running server: it fails if
# any route answers wrongly, if any isolate is condemned, or if the guest kernel reports an
# unmappable fault.
#
# /pipeline calls a method on the promise of a stub. /deep makes eight pipelined hops before
# the awaited call. /object pipelines on a plain-object result, which the callee wraps in a
# transient target, through a property path too. /far pipelines on a stub the callee holds on a
# third isolate. /fanout queues fifty calls on one promise that is still pending when all of
# them are sent. /resolved pipelines on a promise that has already settled. /reject pipelines
# on a call that throws and on one that returns a number. /drop abandons pipelined calls while
# the first call is still pending. /echo sends a stub as an argument and pipelines on the echo
# of it.
#
# Negative build (never shipped), failing this script on its first route:
#   --copt=-DWORKERD_GK_NEG_RPC_SHARED_HEAP  the call's shared-heap windows are not entered, so
#       the queued call and the stubs it reaches are in one isolate's arena and the other's
#       turn faults on them
#
# Usage: rpc-pipeline-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_RPC_PIPELINE_PORT overrides the caller's port (8871); the plain worker takes the port
#   below. GK_RPC_PIPELINE_ROUNDS sets how many times the routes are driven (5).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_RPC_PIPELINE_PORT:-8871}
plain_port=$((port - 1))
caller=http://127.0.0.1:$port
rounds=${GK_RPC_PIPELINE_ROUNDS:-5}
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
  grep -E "unmappable fault|isolate condemned" "$log" | head -n 6
  echo "--- workerd log, without the guest kernel's per-turn chatter ---"
  grep -vE "^\[gk\] vcpu [0-9]+(:? (syscall|.*: (wall|root switch)))" "$log" | tail -n 40
  exit 1
}
body() {
  curl -s --max-time 60 "$1" || echo no-response
}

GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
  "$workerd" serve --experimental "$here/rpc-pipeline-test.capnp" \
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
  echo "SKIP: this workerd has no per-isolate heap arenas, so a pipelined call has no arena"
  echo "      wall to cross. Build with --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
[ "$(body "$caller/")" = "caller" ] || fail "the caller never served"
echo "workerd pid $pid serving the caller on $caller, with guest-kernel isolation and"
echo "per-isolate heap arenas; the plain worker holds the first arena"

step=0
check() {  # route expected-line
  step=$((step + 1))
  got=$(body "$caller/$1")
  echo "$step. GET $caller/$1 -> ${got:-no-response}"
  [ "$got" = "$2" ] || fail "GET /$1 answered '${got:-no-response}', wanted '$2'"
  if grep -q "unmappable fault" "$log"; then
    fail "GET /$1 faulted: $(grep -m1 "unmappable fault" "$log")"
  fi
}

for round in $(seq 1 "$rounds"); do
  check "pipeline?n=5" "pipeline n=5 ok=5"
  check "deep?n=5&depth=8" "deep n=5 depth=8 ok=5"
  check "object?n=5" "object n=5 ok=5"
  check "far?n=5" "far n=5 ok=5"
  check "fanout?n=5&width=50" "fanout n=5 width=50 ok=5"
  check "resolved?n=5" "resolved n=5 ok=5"
  check "reject?n=5" "reject n=5 ok=5"
  check "drop?n=5" "drop n=5 ok=true"
  check "echo?n=5" "echo n=5 ok=5"
done
echo "   $rounds rounds: calls pipelined on a stub, eight hops deep, on a plain object and a"
echo "   property of it, on a third isolate's stub, fifty at a time on one pending promise,"
echo "   on a settled promise, on a rejected call, abandoned mid-flight, and on an echoed stub"

# The abandoned calls of /drop complete after their responses; give them time to, so that a
# fault in the forwarding or the teardown of a queued call is counted below.
sleep 1

faults=$(grep -c "unmappable fault" "$log")
step=$((step + 1))
echo "$step. unmappable faults reported by the guest kernel: $faults"
[ "$faults" = 0 ] || fail "$(grep -m1 "unmappable fault" "$log")"

condemned=$(grep -c "isolate condemned" "$log")
step=$((step + 1))
echo "$step. isolates condemned: $condemned"
[ "$condemned" = 0 ] || fail "$(grep -m1 "isolate condemned" "$log")"

[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] ||
  fail "the plain worker stopped serving"
[ "$(body "$caller/")" = "caller" ] || fail "the caller stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
step=$((step + 1))
echo "$step. all four workers still serve"

echo "PASS: promise pipelining over an in-process JS RPC binding between isolates with per-isolate heap arenas, in every shape a worker's JS can give it, faulted no turn and condemned no isolate, and every pipelined call returned what the other side built"
