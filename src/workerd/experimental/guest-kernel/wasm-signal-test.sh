#!/usr/bin/env bash
# Checks, against a built workerd, that the Wasm shutdown signal still reaches a module whose JS
# turn runs at guest ring 3, and that the descriptor the signal handler writes through is no
# longer the one in the heap. The signal has no source in workerd itself, so with
# WORKERD_GK_TEST_PROBE set three request paths stand in for the CPU limiter (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++):
#
# /__gk_wasm_arm arms, on the event loop after the turn, a one-shot CPU-time timer on the thread
# whose signal handler calls writeShutdownSignal(), as a limiter's would. A following /burn
# request spins reading the registered module's __instance_signal field, from JS through the
# module's own export, until the value lands: it must observe 24 (SIGXCPU).
#
# /__gk_wasm_corrupt overwrites, from inside the turn, the heap entry of the registered module --
# the pointer and size of its linear memory and the signal offset -- with a 64-byte buffer this
# code chose, which is what a write primitive aiming the old descriptor would leave, then arms the
# timer. The next /burn must still observe 24 in the module's real memory, and /__gk_wasm_check,
# which reads the chosen buffer and restores the entry, must find its first word still 0.
#
# Usage: wasm-signal-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_WASM_TEST_HELLO_PORT / GK_WASM_TEST_VICTIM_PORT override the ports (8875 / 8874).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_WASM_TEST_HELLO_PORT:-8875}
victim_port=${GK_WASM_TEST_VICTIM_PORT:-8874}
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
status() {
  curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
body() {
  curl -s --max-time 30 "$1"
}
# Waits for a log line matching $1 to appear $2 times.
await_log() {
  for _ in $(seq 1 100); do
    [ "$(grep -c "$1" "$log")" -ge "$2" ] && return 0
    sleep 0.1
  done
  return 1
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/wasm-signal-test.capnp" \
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

r=$(body "$victim/instantiate")
echo "1. GET $victim/instantiate -> $r (the module registers; its field reads 0)"
echo "$r" | grep -q '"signal":0' || fail "the module did not instantiate with a zero field"

s=$(status "$victim/__gk_wasm_arm")
echo "2. GET $victim/__gk_wasm_arm -> $s (a CPU-time timer is armed on the turn's thread)"
[ "$s" = 200 ] || fail "arming the timer failed"
await_log "armed a CPU-time timer" 1 || fail "the timer was not armed"

r=$(body "$victim/burn")
echo "3. GET $victim/burn -> $r (the signal must land in the module's memory: 24)"
echo "$r" | grep -q '"observed":24' || fail "the module did not observe the shutdown signal"
echo "$r" | grep -q '"iterations":0}' && fail "the field was not clear before the burn"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the signal"

s=$(status "$victim/__gk_wasm_corrupt")
echo "4. GET $victim/__gk_wasm_corrupt -> $s (the in-turn overwrite of the heap descriptor must land)"
[ "$s" = 200 ] || fail "the request that overwrote the descriptor did not complete"
over=$(grep -o "overwrote a Wasm signal descriptor in the heap from inside the JS turn.*" "$log" | head -n1)
[ -n "$over" ] || fail "the descriptor was not overwritten"
oldptr=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 1p)
chosen=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 2p)
echo "   linear memory pointer $oldptr overwritten with $chosen (a 64-byte buffer the turn chose), signal offset with 0"
[ -n "$oldptr" ] && [ -n "$chosen" ] && [ "$oldptr" != "$chosen" ] || fail "no pointer values in: $over"
await_log "armed a CPU-time timer" 2 || fail "the timer was not armed again"

r=$(body "$victim/burn")
echo "5. GET $victim/burn -> $r (the signal must still land in the module's real memory)"
echo "$r" | grep -q '"observed":24' || fail "the module did not observe the shutdown signal after the overwrite"
echo "$r" | grep -q '"iterations":0}' && fail "the field was not clear before the second burn"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the signal"

s=$(status "$victim/__gk_wasm_check")
echo "6. GET $victim/__gk_wasm_check -> $s"
[ "$s" = 200 ] || fail "the check request did not complete"
word=$(grep -o "the chosen buffer's first word after the signal.*" "$log" | head -n1)
[ -n "$word" ] || fail "the chosen buffer was not checked"
value=$(echo "$word" | grep -o 'word = [0-9]*' | sed 's/.*= //')
echo "   the chosen buffer at $chosen holds $value after the signal (24 would be the old write landing there)"
[ "$value" = 0 ] || fail "the signal handler wrote through the overwritten descriptor"
grep -q "restored the Wasm signal descriptors" "$log" || fail "the descriptors were not restored"

s=$(status "$hello/")
echo "7. GET $hello/ -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a module at guest ring 3 observed the shutdown signal from a CPU-time timer's handler; with its heap descriptor overwritten from inside a turn to name a chosen buffer, the signal still reached the module's real memory and the chosen buffer stayed untouched"
