#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), that request and response bodies cross an
# in-process service binding intact, with neither isolate's turn reaching the other's arena.
#
# A body crosses the binding through a kj one-way pipe whose ends are held by different
# isolates, and the pipe copies in whichever of the two operations arrives second, so without a
# relay the copy runs in one isolate's turn over the other isolate's arena: the reader's turn
# faults, is condemned, and the request fails. `ReadRelay` in io/worker-interface.c++ puts every
# operation on the pipe on the event loop, outside any turn.
#
# /post sends a string body, which the caller pumps from a buffer it holds; /stream sends a
# JS-implemented stream, which it pumps from JS reads; both are read whole by the callee with
# text(). /getbody reads a response body the callee built. /passthrough sends a JS-implemented
# stream that the callee returns unread as its response body, so the request body's relay feeds
# the response pipe directly. Each runs at a size below the relay's 64 KiB staging buffer and at
# one that takes several of them. /abort-upload aborts a streaming upload while the callee is
# blocked in a read of it: the abort tears the body pump down inside the caller's turn, which
# drops the pipe's write end, and that reaches whatever the callee left on the pipe -- with the
# relay, a node the host allocated. The script fails if any route answers wrongly, if any
# isolate is condemned, or if the guest kernel reports an unmappable fault.
#
# Negative build (never shipped), failing this script on its first route:
#   --copt=-DWORKERD_GK_NEG_BODY_RELAY  the relays are not installed, so a body crosses through
#       the bare pipe and the reading isolate's turn faults on the writing isolate's arena
#
# Usage: body-arena-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_BODY_ARENA_PORT overrides front's port (8863); the plain worker takes the port below.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_BODY_ARENA_PORT:-8863}
plain_port=$((port - 1))
front=http://127.0.0.1:$port
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
  curl -s --max-time 30 "$1" || echo no-response
}

GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
  "$workerd" serve --experimental "$here/body-arena-test.capnp" \
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
  echo "SKIP: this workerd has no per-isolate heap arenas, so a body crossing an in-process"
  echo "      binding has no arena wall to cross. Build with"
  echo "      --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
[ "$(body "$front/")" = "front" ] || fail "front never served"
echo "workerd pid $pid serving front on $front, with guest-kernel isolation and per-isolate"
echo "heap arenas; the plain worker holds the first arena"

step=0
check() {  # route expected-line
  step=$((step + 1))
  got=$(body "$front/$1")
  echo "$step. GET $front/$1 -> ${got:-no-response}"
  [ "$got" = "$2" ] || fail "GET /$1 answered '${got:-no-response}', wanted '$2'"
  if grep -q "unmappable fault" "$log"; then
    fail "GET /$1 faulted: $(grep -m1 "unmappable fault" "$log")"
  fi
}

for n in 1000 300000; do
  check "post?n=$n" "post n=$n status=200 back=len=$n ok=true"
  check "getbody?n=$n" "getbody n=$n status=200 len=$n ok=true"
  check "stream?n=$n" "stream n=$n status=200 back=len=$n ok=true"
  check "passthrough?n=$n" "passthrough n=$n status=200 len=$n ok=true"
done
echo "   a string body, a response body, a JS stream body and a passed-through stream body"
echo "   crossed the binding intact at 1000 bytes and at 300000"

step=$((step + 1))
got=$(body "$front/abort-upload")
echo "$step. GET $front/abort-upload -> ${got:-no-response}"
echo "$got" | grep -q "outcome=rejected:abort-upload" ||
  fail "the aborted upload did not reject with the abort reason: ${got:-no-response}"
elapsed=$(echo "$got" | grep -o "elapsed=[0-9]*" | cut -d= -f2)
[ -n "$elapsed" ] && [ "$elapsed" -lt 3000 ] ||
  fail "the upload was not cancelled promptly (elapsed=${elapsed:-?} ms)"
pulls=$(echo "$got" | grep -o "pulls=[0-9]*" | cut -d= -f2)
[ -n "$pulls" ] && [ "$pulls" -ge 1 ] ||
  fail "the upload never produced a chunk before the abort, so the callee was never reading it"
echo "   the abort reached a pump that had delivered $pulls chunk(s) to a callee blocked in a read"

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
[ "$(body "$front/")" = "front" ] || fail "front stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
step=$((step + 1))
echo "$step. all three workers still serve"

echo "PASS: request and response bodies crossed an in-process service binding between two isolates with per-isolate heap arenas, in both directions, from a buffer and from JS, read whole and passed straight through, at sizes below and above the relay's staging buffer, and a streaming upload was aborted while the callee was reading it: no turn faulted, no isolate was condemned, and every byte arrived"
