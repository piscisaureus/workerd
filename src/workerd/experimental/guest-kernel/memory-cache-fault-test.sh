#!/usr/bin/env bash
# Checks that a fault inside the guest kernel during a MemoryCache fallback fails only that
# request and condemns only that isolate, and that a co-tenant worker sharing the cache can still
# read the key afterwards. A cache read whose key is not cached inserts an in-progress entry for
# the key in the shared cache and runs the fallback; every other read of that key, from any
# worker sharing the cache id, waits on that entry until the fallback's done-callback settles or
# is destroyed (api/memory-cache.c++). The callback is owned by the request's IoContext before
# the fallback runs, so the teardown of the faulted request releases the entry and the peer's
# read completes with its own fallback. (In the WORKERD_GK_NEG_CACHE_FALLBACK build the callback
# is owned only once the fallback has returned, so the fault strands the entry: the peer's read
# hangs, which this script bounds with a timeout and reports as a failure.)
#
# Usage: memory-cache-fault-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_MEMORY_CACHE_TEST_VICTIM_PORT / GK_MEMORY_CACHE_TEST_PEER_PORT override the ports
#   (8814 / 8815).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
victim_port=${GK_MEMORY_CACHE_TEST_VICTIM_PORT:-8814}
peer_port=${GK_MEMORY_CACHE_TEST_PEER_PORT:-8815}
victim=http://127.0.0.1:$victim_port
peer=http://127.0.0.1:$peer_port
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
  cat "$log"
  exit 1
}
# Prints the HTTP status of a GET, or "no-response" if the connection failed or timed out.
status() {
  curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
# Prints the body of a GET, or "no-response" if the connection failed or timed out.
body() {
  curl -s --max-time 10 "$1" || echo no-response
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_FAULT=1 \
  "$workerd" serve --experimental "$here/memory-cache-fault-test.capnp" \
  --socket-addr "victim=127.0.0.1:$victim_port" --socket-addr "peer=127.0.0.1:$peer_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$victim/")" = 200 ] && [ "$(status "$peer/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving victim on $victim and peer on $peer"

b=$(body "$victim/read?key=warm")
echo "1. GET $victim/read?key=warm           -> ${b%$'\n'}"
[ "$b" = "victim read: warm from victim" ] || fail "victim's own fallback did not produce the value"
b=$(body "$peer/read?key=warm")
echo "2. GET $peer/read?key=warm           -> ${b%$'\n'} (the cache is shared: peer sees victim's value)"
[ "$b" = "peer read: warm from victim" ] || fail "the two workers do not share the cache"

s=$(status "$victim/__gk_fault_cache_fallback")
echo "3. GET $victim/__gk_fault_cache_fallback  -> $s (request must fail: its fallback turn faulted)"
[ "$s" != 200 ] || fail "the request whose fallback faulted succeeded"
grep -q "forcing a guest fault inside a memory cache fallback" "$log" ||
  fail "the fault was not taken inside the fallback"
grep -q "isolate condemned" "$log" || fail "no condemnation logged"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
echo "   the fault was taken inside the fallback; workerd pid $pid is still alive"

b=$(body "$peer/read?key=stranded")
echo "4. GET $peer/read?key=stranded       -> ${b%$'\n'} (must complete: the faulted request's entry was released)"
[ "$b" != no-response ] || fail "the peer's read of the key hung: the in-progress entry was stranded"
[ "$b" = "peer read: stranded from peer" ] || fail "the peer's read did not run its own fallback: $b"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the peer's read"

s=$(status "$victim/")
echo "5. GET $victim/                      -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"

b=$(body "$peer/read?key=warm")
echo "6. GET $peer/read?key=warm           -> ${b%$'\n'} (the shared cache still serves)"
[ "$b" = "peer read: warm from victim" ] || fail "the shared cache stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest fault"

echo "PASS: guest fault inside a memory cache fallback failed one request and condemned one isolate, and the co-tenant's read of the same key completed with its own fallback instead of waiting on the faulted one"
