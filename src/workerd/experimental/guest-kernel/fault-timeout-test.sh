#!/usr/bin/env bash
# Checks that a fault inside the guest kernel during a setTimeout callback fails only that
# request and condemns only that isolate, while the workerd process keeps serving its other
# worker. fault-test.sh faults in a fetch handler, before the turn has taken any host state; this
# one faults in the turn a timer fires, whose abandoned frames hold the timeout's running mark
# (TimeoutState::isRunning in src/workerd/io/io-context.c++), which ~TimeoutState asserts on when
# the request's IoContext is destroyed. The fault path clears the mark; the process must outlive
# the teardown and keep answering. (In the WORKERD_GK_NEG_TIMEOUT_RUNNING build the fault path
# leaves the mark standing, the assert fires during teardown, and this script fails on the dead
# process.)
#
# Usage: fault-timeout-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_FAULT_TIMEOUT_TEST_HELLO_PORT / GK_FAULT_TIMEOUT_TEST_VICTIM_PORT override the ports
#   (8825 / 8824).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_FAULT_TIMEOUT_TEST_HELLO_PORT:-8825}
victim_port=${GK_FAULT_TIMEOUT_TEST_VICTIM_PORT:-8824}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
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
# Prints the HTTP status of a GET, or "no-response" if the connection failed.
status() {
  curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$1" || echo no-response
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_FAULT=1 \
  "$workerd" serve "$here/fault-timeout-test.capnp" \
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
echo "1. GET $victim/                    -> $s"
[ "$s" = 200 ] || fail "victim did not serve before the fault"

s=$(status "$victim/__gk_fault_timeout")
echo "2. GET $victim/__gk_fault_timeout  -> $s (request must fail: its timer turn faulted)"
[ "$s" != 200 ] || fail "the request whose timeout callback faulted succeeded"
grep -q "forcing a guest fault inside a timeout callback" "$log" ||
  fail "the fault was not taken inside the timeout callback"
grep -q "isolate condemned" "$log" || fail "no condemnation logged"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
echo "   the fault was taken inside the timeout callback; workerd pid $pid is still alive"

# The faulted request's IoContext is torn down on the event loop once the request is over, which
# is where the abandoned timeout state is destroyed. Give that a moment, with the other worker
# kept busy, and require the process to be alive throughout.
for i in $(seq 1 20); do
  s=$(status "$hello/")
  [ "$s" = 200 ] || fail "the other isolate stopped serving (attempt $i: $s)"
  kill -0 "$pid" 2>/dev/null || fail "workerd died after the faulted request's teardown"
  sleep 0.1
done
echo "3. GET $hello/ x20                 -> 200 (process alive through the faulted request's teardown)"

s=$(status "$victim/")
echo "4. GET $victim/                    -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"

s=$(status "$hello/")
echo "5. GET $hello/                     -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest fault"

echo "PASS: guest fault inside a timeout callback failed one request, condemned one isolate, and the process outlived that request's teardown and kept serving"
