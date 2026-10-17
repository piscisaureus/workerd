#!/usr/bin/env bash
# Checks, against a built workerd, that a worker whose IoContext was built inside another
# worker's JS turn owns the objects it registers against that context. A subrequest builds the
# callee's IoContext from the caller's turn (IoContext::getSubrequestChannel, through
# Server::WorkerService::startRequest), so the context's resolve entry comes out of the
# caller's journal and gk owns it to the caller's arena. Every owned object the callee then
# registers against its own context, from its own turns, names a context of another isolate,
# which the commit hook refuses -- and registerOwned() has already handed the object to the
# table, so nothing owns it and nothing frees it: one object lost per registration. The same
# mis-ownership makes the callee's own turns resolve their context's handle to nothing, so an
# action scheduled for it is dropped as if the request had ended.
#
# The entry is therefore registered owned by no arena at all, where it resolves for no turn,
# and the isolate it belongs to adopts it host-side on its own thread, under its own lock,
# where gk takes the arena from its record of the thread rather than from anything a turn
# supplied (DeleteQueueOwner::adoptEntry, gk_resolve_adopt).
#
# /rpc makes 100 cross-worker RPC calls, each taking a stub from the callee and disposing of it
# inside the caller's turn, and /sub makes 20 service-binding fetches, which build the callee's
# context the same way without RPC. Across both, the server must refuse no registration, and
# the owned-node and context-record counts must come back to where they started. /rpc also
# drives the other side of the arena wall: the callee's ReverseIoOwn of its own JS handles is
# dropped inside the caller's turn, which cannot dispose of it -- the object waits for its own
# context's teardown -- and that is counted, so the script fails if the case stops happening.
#
# /__gk_refused_registration is the refusal path itself: an owned object registered against a
# context that no longer exists. The hook refuses it, and because registerOwned() has already
# given the object away, the refusal has to dispose of it; the object says so when it dies.
# /__gk_refused_in_refill makes that refusal many times over in one turn, interleaved with
# enough ordinary registrations that the refusals are found by the refill hostcall rather than
# by the end of the turn, where the hook that disposes of the object cannot run.
#
# Negative builds (never shipped), each failing this script:
#   --copt=-DWORKERD_GK_NEG_CONTEXT_ADOPT  the context keeps the entry its creating turn's
#       journal gave it, so the refusals and the lost objects come back
#   --copt=-DWORKERD_GK_NEG_REFUSED_DISPOSE  a refused registration only retires the entry,
#       leaving its object owned by nothing
#
# Usage: rpc-context-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_RPC_TEST_PORT overrides the port (8865).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_RPC_TEST_PORT:-8865}
caller=http://127.0.0.1:$port
log=$(mktemp)

# Stops workerd without waiting for it indefinitely: it drains in-flight requests on SIGTERM, so a
# process that still holds a hung request -- what a failed probe can leave behind -- is killed
# after a moment rather than waited for.
stop_workerd() {  # $1 pid
  kill "$1" 2>/dev/null
  for _ in $(seq 1 50); do
    kill -0 "$1" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$1" 2>/dev/null
  wait "$1" 2>/dev/null
}
cleanup() {
  stop_workerd "$pid"
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- workerd log ---"
  tail -n 80 "$log"
  exit 1
}
status() {
  curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
# The counters the server logs for /__gk_ioown_counts, as shell variables. The request only
# asks for them; the log line comes from the event loop, so the read waits for a new one.
counts() {
  before=$(grep -c "owned-node counts" "$log")
  [ "$(status "$caller/__gk_ioown_counts")" = 200 ] || fail "the counts request failed"
  for _ in $(seq 1 100); do
    [ "$(grep -c 'owned-node counts' "$log")" -gt "$before" ] && break
    sleep 0.1
  done
  line=$(grep "owned-node counts" "$log" | tail -n1)
  [ -n "$line" ] || fail "the server did not log the owned-node counts"
  foreign_drops=$(echo "$line" | sed -n 's/.*stats.foreignInTurnDrops = \([0-9]*\).*/\1/p')
  refused=$(echo "$line" | sed -n 's/.*stats.registrationsRefused = \([0-9]*\).*/\1/p')
  disposed=$(echo "$line" | sed -n 's/.*stats.objectsDisposedOnRefusal = \([0-9]*\).*/\1/p')
  nodes_a=$(echo "$line" | sed -n 's/.*stats.nodesAllocated = \([0-9]*\).*/\1/p')
  nodes_f=$(echo "$line" | sed -n 's/.*stats.nodesFreed = \([0-9]*\).*/\1/p')
  recs_a=$(echo "$line" | sed -n 's/.*stats.recordsAllocated = \([0-9]*\).*/\1/p')
  recs_f=$(echo "$line" | sed -n 's/.*stats.recordsFreed = \([0-9]*\).*/\1/p')
  [ -n "$refused" ] && [ -n "$nodes_a" ] || fail "could not read the counts from: $line"
  live_nodes=$((nodes_a - nodes_f))
  live_recs=$((recs_a - recs_f))
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve --experimental "$here/rpc-context-test.capnp" \
  --socket-addr "caller=127.0.0.1:$port" >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$caller/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
[ "$(status "$caller/")" = 200 ] || fail "the caller never started serving"
grep -q "guest-kernel isolation enabled" "$log" ||
  fail "the server is not running with guest-kernel isolation; the test would prove nothing"
echo "workerd pid $pid serving the caller on $caller, with guest-kernel isolation"

counts
base_refused=$refused
base_drops=$foreign_drops
base_nodes=$live_nodes
base_recs=$live_recs
echo "1. before the load: $base_refused registrations refused, $base_drops foreign in-turn"
echo "   drops, $base_nodes owned nodes and $base_recs context records live"

body=$(curl -s --max-time 120 "$caller/rpc?n=100")
echo "2. GET $caller/rpc?n=100 -> ${body:-no-response} (100 cross-worker RPC calls, each stub"
echo "   disposed of inside the caller's turn)"
[ "$body" = "rpc=100" ] || fail "the RPC load did not complete: ${body:-no-response}"

body=$(curl -s --max-time 120 "$caller/sub?n=20")
echo "3. GET $caller/sub?n=20  -> ${body:-no-response} (20 service-binding fetches, which build"
echo "   the callee's context the same way)"
[ "$body" = "sub=20" ] || fail "the subrequest load did not complete: ${body:-no-response}"

cross=$(grep -c "names a context of another isolate" "$log")
echo "4. registrations refused for naming another isolate's context: $cross"
[ "$cross" = 0 ] ||
  fail "the callee could not register against its own context; $cross registrations were refused"

counts
echo "5. after the load: $refused registrations refused ($base_refused before),"
echo "   $disposed objects disposed of by a refusal, $foreign_drops foreign in-turn drops"
echo "   ($base_drops before)"
[ "$refused" = "$base_refused" ] ||
  fail "the load refused $((refused - base_refused)) registrations"
[ "$foreign_drops" -gt "$base_drops" ] ||
  fail "no holder of another isolate's object was dropped inside a turn, so the load did not reach the case this tests"

# The contexts the load created have to end, and their objects with them. The teardowns run on
# the event loop after the responses, so this waits for the counts to come back.
for _ in $(seq 1 100); do
  counts
  [ "$live_nodes" -le "$base_nodes" ] && [ "$live_recs" -le "$base_recs" ] && break
  sleep 0.1
done
echo "6. after the contexts ended: $live_nodes owned nodes and $live_recs context records live"
echo "   ($base_nodes and $base_recs before the load)"
[ "$live_nodes" -le "$base_nodes" ] ||
  fail "$((live_nodes - base_nodes)) owned nodes outlived the requests that made them"
[ "$live_recs" -le "$base_recs" ] ||
  fail "$((live_recs - base_recs)) context records outlived the requests that made them"

s=$(status "$caller/__gk_refused_registration")
echo "7. GET $caller/__gk_refused_registration -> $s (an owned object registered against a"
echo "   context that no longer exists)"
[ "$s" = 200 ] || fail "the refused-registration request failed"
for _ in $(seq 1 100); do
  grep -q "the object of a refused registration was disposed of" "$log" && break
  sleep 0.1
done
grep -q "registered an owned object naming no live context" "$log" ||
  fail "the probe did not make a registration"
grep -q "the object of a refused registration was disposed of" "$log" ||
  fail "the refused registration left its object owned by nothing; it is never freed"
echo "   the refusal disposed of the object: nothing else could still reach it"

counts
before_refused=$refused
s=$(status "$caller/__gk_refused_in_refill")
echo "8. GET $caller/__gk_refused_in_refill -> $s (refusals found by the refill hostcall"
echo "   rather than at the end of the turn)"
for _ in $(seq 1 200); do
  grep -q "registrations refused across refills" "$log" && break
  sleep 0.1
done
line=$(grep "registrations refused across refills" "$log" | tail -1)
[ -n "$line" ] || fail "the probe did not run"
rounds=$(echo "$line" | sed -n 's/.*rounds = \([0-9]*\).*/\1/p')
[ -n "$rounds" ] && [ "$rounds" -gt 0 ] || fail "could not read the round count from: $line"
echo "   $rounds refused registrations made in one turn; $line"

counts
[ "$((refused - before_refused))" = "$rounds" ] ||
  fail "$rounds refusals were made and $((refused - before_refused)) counted"
echo "   all $rounds were counted as refused"
[ "$disposed" -gt 0 ] || fail "the refusal was not counted"
echo "9. $refused registrations refused in all, $disposed of their objects disposed of"
[ "$refused" = "$disposed" ] ||
  fail "$((refused - disposed)) refused registrations did not dispose of their object"

s=$(status "$caller/")
echo "10. GET $caller/            -> $s (both workers must still serve)"
[ "$s" = 200 ] || fail "the caller stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a worker whose IoContext was built inside another worker's turn registered 120 requests' worth of owned objects against that context with none refused, and the nodes and records went away with the requests; $((foreign_drops - base_drops)) holders of the callee's objects were dropped inside the caller's turn, which left them for their own context's teardown rather than disposing of them under the wrong arena; and $disposed registrations refused because the context they named had ended disposed of their object instead of orphaning it, $rounds of them made in one turn so that a refill rather than the turn's end found them"
