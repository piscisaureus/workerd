#!/usr/bin/env bash
# Checks, against a built workerd, that the guest-writable thread-locals the host reads after a JS
# turn do not carry a value a turn left in them. These thread-locals (threadLocalRequest,
# currentApi, the SQLite memory scope, the VFS temp-dir and symlink-guard scopes, the thread-scope
# depths) name things the host dereferences or asserts on after the turn; the host saves them in
# the stack frame that runs the turn -- read-only to the guest under the host-frame wall -- and
# puts them back once the guest has returned (GuestTurnThreadLocals in src/workerd/io/, and
# ~Worker::Lock for the Api pointer the lock restores from its heap record).
#
# With WORKERD_GK_TEST_PROBE set, /__gk_tls_restore on the "victim" worker overwrites, from inside
# the turn, the heap slot ~Worker::Lock restores Worker::Api's thread-local from, with a sentinel
# (0x1), as a write primitive would, and logs the slot's address and old value. ~Worker::Lock must
# then leave the thread-local holding the value it had before the lock, drawn from a stack frame
# and not from the overwritten heap slot; an event-loop callback, running once the lock is gone,
# logs whether Worker::Api's thread-local is the sentinel or the restored value. "sentinel-leaked"
# would mean the host trusted the value the turn wrote.
#
# Usage: tls-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_TLS_TEST_HELLO_PORT / GK_TLS_TEST_VICTIM_PORT override the ports (8865 / 8864).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_TLS_TEST_HELLO_PORT:-8865}
victim_port=${GK_TLS_TEST_VICTIM_PORT:-8864}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
log=$(mktemp)

cleanup() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$log"; }
fail() { echo "FAIL: $*"; echo "--- workerd log ---"; cat "$log"; exit 1; }
status() { curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response; }

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/tls-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, victim on $victim"

s=$(status "$victim/__gk_tls_restore")
echo "1. GET $victim/__gk_tls_restore -> $s (the in-turn overwrite of the restore slot must land)"
[ "$s" = 200 ] || fail "the request that overwrote the restore slot did not complete"
over=$(grep -o "overwrote the Api restore slot from inside the JS turn.*" "$log" | head -n1)
[ -n "$over" ] || fail "the restore slot was not overwritten"
echo "   $over" | sed 's/^   WORKERD_GK_TEST_PROBE: //'

for _ in $(seq 1 100); do
  grep -q "Worker::Api after the turn" "$log" && break
  sleep 0.1
done
after=$(grep -o "Worker::Api after the turn.*" "$log" | head -n1)
[ -n "$after" ] || fail "the event-loop check did not run"
echo "2. $after" | sed 's/WORKERD_GK_TEST_PROBE: //'
verdict=$(echo "$after" | sed -n 's/.*= //p')
[ "$verdict" = restored ] || fail "Worker::Api held the value the turn wrote ($verdict): the restore did not come from the host frame"

s=$(status "$victim/")
echo "3. GET $victim/ -> $s (the worker keeps serving after the overwrite)"
[ "$s" = 200 ] || fail "the victim stopped serving"
s=$(status "$hello/")
echo "4. GET $hello/ -> $s (the other worker keeps serving)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a turn's overwrite of the Api restore slot did not reach the host; Worker::Api after the turn held the value restored from the host frame, not the sentinel; the process kept serving"
