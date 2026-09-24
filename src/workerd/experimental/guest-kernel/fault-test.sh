#!/usr/bin/env bash
# Checks that a fault inside the guest kernel during a JS turn fails only that request and
# condemns only that isolate, while the workerd process keeps serving its other worker.
#
# Usage: fault-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_FAULT_TEST_HELLO_PORT / GK_FAULT_TEST_VICTIM_PORT override the ports (8899 / 8898).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_FAULT_TEST_HELLO_PORT:-8899}
victim_port=${GK_FAULT_TEST_VICTIM_PORT:-8898}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
log=$(mktemp)

cleanup() {
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- workerd log ---"
  cat "$log"
  exit 1
}
# Prints the HTTP status of a GET, or "no-response" if the connection failed.
status() {
  curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_FAULT=1 \
  "$workerd" serve "$here/fault-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello and victim on $victim"

s=$(status "$victim/")
echo "1. GET $victim/            -> $s"
[ "$s" = 200 ] || fail "victim did not serve before the fault"

s=$(status "$victim/__gk_fault")
echo "2. GET $victim/__gk_fault  -> $s (request must fail)"
[ "$s" != 200 ] || fail "the faulting request succeeded"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
echo "   workerd pid $pid is still alive"
grep -q "isolate condemned" "$log" || fail "no condemnation logged"

s=$(status "$victim/")
echo "3. GET $victim/            -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"

s=$(status "$hello/")
echo "4. GET $hello/             -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest fault"

echo "PASS: guest fault failed one request, condemned one isolate, process kept serving"
