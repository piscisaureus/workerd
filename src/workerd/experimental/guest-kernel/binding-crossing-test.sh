#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), the crossings of an in-process service binding
# that carry no body: what a request brings beside the HTTP interface, and what a call that
# fails hands back.
#
# body-arena-test.sh builds every subrequest from a URL string and every callee of it succeeds,
# so three things never cross there. A request's `cf` blob travels as
# IoChannelFactory::SubrequestMetadata rather than through kj's HttpClient adapter, whose clones
# of the url and headers are made in the shared heap; it is serialized by the caller's turn and
# read by the callee's (CfProperty in api/global-scope.c++), and a request forwarded as a proxy
# forwards it always carries one, since the listener makes a `{"clientIp": ...}` blob for every
# connection. A callee that throws hands the caller a kj::Exception made under the callee's lock,
# which the caller's turn reads in jsg::exceptionToJs(); and a JS RPC call that rejects drops the
# caller's exported capabilities inside the callee's turn, where the caller's
# IoContext::PendingEvent destructor schedules its hang-abort task, which the caller's next turn
# cancels. WorkerEntrypoint (io/worker-entrypoint.c++) copies the blob and the exception into
# the shared heap, and ~PendingEvent (io/io-context.c++) allocates its task there. An RPC
# exception needs no copy: capnp forks the call's promise, and each branch of a fork gets a clone
# of the exception made on the event loop.
#
# The cdn pair of bench/bench.capnp is served as is: a front that forwards the incoming Request
# to an origin over a binding and returns `new Response(r.body, r)`. The script fails if any
# route answers wrongly, if any isolate is condemned, or if the guest kernel reports an
# unmappable fault.
#
# Negative builds (never shipped), each failing this script at the step named:
#   --copt=-DWORKERD_GK_NEG_CF_BLOB_HEAP    the cf blob stays in the caller's arena; step 1,
#       the benchmark's forwarded request, faults the origin's turn
#   --copt=-DWORKERD_GK_NEG_EXCEPTION_HEAP  a callee's exception stays in its arena; the
#       first throwing callee faults the caller's turn
#
# Usage: binding-crossing-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_BINDING_CROSSING_PORT overrides front's port (8882); cdn takes the port below it and the
#   plain worker the one below that.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_BINDING_CROSSING_PORT:-8882}
cdn_port=$((port - 1))
plain_port=$((port - 2))
front=http://127.0.0.1:$port
cdn=http://127.0.0.1:$cdn_port
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
  "$workerd" serve --experimental "$here/binding-crossing-test.capnp" \
  --socket-addr "plain=127.0.0.1:$plain_port" --socket-addr "cdn=127.0.0.1:$cdn_port" \
  --socket-addr "front=127.0.0.1:$port" >"$log" 2>&1 &
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
  echo "SKIP: this workerd has no per-isolate heap arenas, so nothing a binding carries has an"
  echo "      arena wall to cross. Build with --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
[ "$(body "$front/")" = "front" ] || fail "front never served"
echo "workerd pid $pid serving front on $front and the benchmark's cdn pair on $cdn, with"
echo "guest-kernel isolation and per-isolate heap arenas; the plain worker holds the first arena"

step=0
faulted() {
  if grep -q "unmappable fault" "$log"; then
    fail "$1 faulted: $(grep -m1 "unmappable fault" "$log")"
  fi
}
check() {  # route expected-line
  step=$((step + 1))
  got=$(body "$front/$1")
  echo "$step. GET $front/$1 -> ${got:-no-response}"
  [ "$got" = "$2" ] || fail "GET /$1 answered '${got:-no-response}', wanted '$2'"
  faulted "GET /$1"
}

# 1. The benchmark's cdn pair: the incoming request forwarded to the origin, whose response is
# passed through as the init of a new one. 4096 x's, 200, and the origin's header.
step=$((step + 1))
headers=$(curl -s --max-time 30 -D - -o /dev/null "$cdn/" || echo no-response)
got=$(body "$cdn/")
echo "$step. GET $cdn/ -> $(echo "$headers" | head -n 1 | tr -d '\r'), ${#got} bytes"
echo "$headers" | head -n 1 | grep -q " 200 " || fail "the cdn front did not answer 200: $headers"
echo "$headers" | grep -qi "^cache-control: max-age=60" ||
  fail "the origin's cache-control header did not pass through: $headers"
[ "${#got}" = 4096 ] && [ "$(echo -n "$got" | tr -d x | wc -c)" = 0 ] ||
  fail "the origin's body did not pass through intact (${#got} bytes)"
faulted "GET $cdn/"
echo "   the benchmark's forwarded request reached the origin and its response came back whole"

# 2-3. The cf blob, from the listener's blob on a forwarded request and from a fetch's init.
step=$((step + 1))
got=$(body "$front/forward")
echo "$step. GET $front/forward -> ${got:-no-response}"
echo "$got" | grep -q '^forward status=200 cf={"clientIp":"127.0.0.1' ||
  fail "the listener's cf blob did not reach the callee: ${got:-no-response}"
faulted "GET /forward"
check "cf" 'cf status=200 cf={"colo":"XYZ","n":1}'
echo "   a forwarded request's cf blob and a fetch init's reached the callee's handler"

# 4-7. Callees that fail, over fetch and over RPC, each twice: the second request after a
# failure is the caller's next turn, which cancels the hang-abort task the first one scheduled.
check "throw" "throw caught=Error:boom"
check "reject" "reject caught=TypeError:rejected"
check "throw" "throw caught=Error:boom"
check "rpc-ok" "rpc-ok 42"
check "rpc-throw" "rpc-throw caught=RangeError:rpc boom"
check "rpc-reject" "rpc-reject caught=TypeError:rpc rejected"
check "rpc-throw" "rpc-throw caught=RangeError:rpc boom"
check "rpc-ok" "rpc-ok 42"
echo "   a callee's thrown and rejected errors reached the caller over fetch and over RPC"

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
[ "$(body "$cdn/" | wc -c)" = 4096 ] || fail "the cdn front stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
step=$((step + 1))
echo "$step. all five workers still serve"

echo "PASS: a request's cf blob crossed an in-process service binding between two isolates with per-isolate heap arenas, on the benchmark's forwarded request and on a fetch init, and callees that threw and rejected over fetch and over JS RPC rejected the caller's call with their error: no turn faulted, no isolate was condemned"
