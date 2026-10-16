#!/usr/bin/env bash
# Checks, against a built workerd, that an AbortSignal's native abort registration
# (api/basics.h) keeps the host out of the signal's own memory: the record holding the abort
# action is the signal's, read and written only under the isolate lock, and what the consumer's
# RAII handle owns is an entry in the guest kernel's resolve table, which it retires when it is
# dropped. The handle is dropped on the event loop, outside the guest -- that is where a
# `fetch` with a signal drops it when its promise settles -- so before this change the host
# took a mutex in the signal's memory there and destroyed a kj::Function through a disposer
# pointer a turn can write. With WORKERD_GK_TEST_PROBE set three request paths drive this from
# inside the turn (see maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++):
#
# /__gk_abort_cell_corrupt creates an AbortSignal and registers a native abort action, keeping
# both alive. A later turn overwrites the disposer word of the action's kj::Function with a
# disposer of its own, as a write primitive would, and announces both addresses. The handle is
# then dropped on the event loop: it must retire the table entry and touch nothing else, so the
# chosen disposer is never called and the action is never destroyed there. A further turn shows
# the record still the signal's, with its entry retired. (Where the handle clears the action
# wherever it is dropped, as in the WORKERD_GK_NEG_ABORT_CELL build, the host destroys the
# function and calls the disposer the turn chose, which logs that it was reached, and this
# script fails.)
#
# /__gk_abort_cell_live is the behaviour the entry has to preserve on one side: with the handle
# still held, aborting the signal runs the action. /__gk_abort_cell_dropped is the other side:
# the handle is dropped on the event loop first, and the abort must then not run the action,
# although the record and the action are still the signal's -- the entry is what says the
# consumer is gone.
#
# /fetch-abort on the "fetcher" worker is the whole path end to end: a subrequest through a
# service binding carrying an AbortController signal, aborted 200 ms in while the backend is
# five seconds from answering. The registration is made inside the turn and disarmed from the
# event loop, so it has to reject promptly, and the signal has to be left holding no live
# registration afterwards.
#
# Usage: abort-signal-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_ABORT_TEST_HELLO_PORT / GK_ABORT_TEST_VICTIM_PORT / GK_ABORT_TEST_FETCHER_PORT override
#   the ports (8875 / 8874 / 8873).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_ABORT_TEST_HELLO_PORT:-8875}
victim_port=${GK_ABORT_TEST_VICTIM_PORT:-8874}
fetcher_port=${GK_ABORT_TEST_FETCHER_PORT:-8873}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
fetcher=http://127.0.0.1:$fetcher_port
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
wait_for() {  # $1 pattern
  for _ in $(seq 1 100); do
    grep -q "$1" "$log" && return 0
    sleep 0.1
  done
  return 1
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve --experimental "$here/abort-signal-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  --socket-addr "fetcher=127.0.0.1:$fetcher_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] &&
    [ "$(status "$fetcher/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, victim on $victim, fetcher on $fetcher"

s=$(status "$victim/__gk_abort_cell_corrupt")
echo "1. GET $victim/__gk_abort_cell_corrupt -> $s (registers an abort action the turn then rewrites)"
[ "$s" = 200 ] || fail "the request that registered the abort action did not complete"
wait_for "created an AbortSignal registration inside the JS turn" ||
  fail "no native abort registration was made"
created=$(grep -o "created an AbortSignal registration inside the JS turn.*" "$log" | head -n1)
record=$(echo "$created" | grep -o '0x[0-9a-f]*' | sed -n 1p)
slot=$(echo "$created" | grep -o '0x[0-9a-f]*' | sed -n 2p)
armed=$(echo "$created" | grep -o '0x[0-9a-f]*' | sed -n 3p)
[ -n "$record" ] && [ "$armed" != "0x0" ] || fail "no record and entry in: $created"
echo "   the record is at $record, its action's disposer word at $slot, armed by entry $armed"

wait_for "overwrote the abort action's disposer from inside the JS turn" ||
  fail "the later turn did not overwrite the disposer"
over=$(grep -o "overwrote the abort action's disposer from inside the JS turn.*" "$log" | head -n1)
saved=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 2p)
chosen=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 3p)
[ -n "$chosen" ] && [ "$chosen" != "$saved" ] || fail "no disposer values in: $over"
echo "2. the disposer word at $slot was overwritten: $saved -> $chosen (a disposer the turn chose)"

wait_for "dropping the abort registration on the event loop" ||
  fail "the registration was not dropped on the event loop"
drop=$(grep -o "dropping the abort registration on the event loop.*" "$log" | head -n1)
echo "$drop" | grep -q "inTurn = false" ||
  fail "the drop did not happen outside a turn, so it proves nothing about the host: $drop"
wait_for "the abort registration was dropped" || fail "the drop did not complete"
echo "3. the handle was dropped on the event loop, outside any turn ($drop)"

wait_for "the turn after the registration was dropped ran" ||
  fail "the turn after the drop did not run"
after=$(grep -o "the turn after the registration was dropped ran.*" "$log" | head -n1)
echo "4. after the drop: ${after#*ran; }"
echo "$after" | grep -q "resolves = false" ||
  fail "the entry still resolves to the record, so the handle did not retire it: $after"
echo "$after" | grep -q "action = present" ||
  fail "the action was destroyed although the drop was on the event loop: $after"
echo "$after" | grep -q "count = 1" || fail "the record did not stay with the signal: $after"

if grep -q "the chosen action disposer was called" "$log"; then
  fail "the chosen disposer was called: $(grep -o 'the chosen action disposer was called.*' "$log" | head -n1)"
fi
echo "5. the chosen disposer was never called: the drop retired a table entry and destroyed nothing"

s=$(status "$victim/")
echo "6. GET $victim/            -> $s (the isolate must still serve)"
[ "$s" = 200 ] || fail "the victim stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

s=$(status "$victim/__gk_abort_cell_live")
echo "7. GET $victim/__gk_abort_cell_live -> $s (abort with the handle still held)"
[ "$s" = 200 ] || fail "the live probe request failed"
wait_for "the live probe's signal was aborted" || fail "the live probe did not abort its signal"
grep -q "the live probe's abort action ran" "$log" ||
  fail "the action did not run although its handle was still held"
echo "   the action ran: an armed registration still delivers"

s=$(status "$victim/__gk_abort_cell_dropped")
echo "8. GET $victim/__gk_abort_cell_dropped -> $s (handle dropped on the event loop, then abort)"
[ "$s" = 200 ] || fail "the dropped probe request failed"
wait_for "the dropped probe's registration was dropped" || fail "the dropped probe did not drop its handle"
dropped=$(grep -o "the dropped probe's registration was dropped.*" "$log" | head -n1)
echo "$dropped" | grep -q "inTurn = false" || fail "the dropped probe's drop was not on the event loop: $dropped"
wait_for "the dropped probe's signal was aborted" || fail "the dropped probe did not abort its signal"
grep -q "the dropped probe's abort action ran" "$log" &&
  fail "the action ran although its handle had been dropped"
echo "   the action did not run: the retired entry is what says the consumer is gone"

body=$(curl -s --max-time 20 "$fetcher/fetch-abort")
echo "9. GET $fetcher/fetch-abort -> ${body:-no-response} (a real subrequest aborted 200 ms in, against a backend 5 s from answering)"
echo "$body" | grep -q "outcome=rejected:abort-signal-test" ||
  fail "the aborted subrequest did not reject with the abort reason: ${body:-no-response}"
elapsed=$(echo "$body" | grep -o "elapsed=[0-9]*" | cut -d= -f2)
[ -n "$elapsed" ] && [ "$elapsed" -lt 3000 ] ||
  fail "the subrequest was not cancelled promptly (elapsed=${elapsed:-?} ms); the registration never delivered"
echo "$body" | grep -q "held=0" ||
  fail "the signal still holds a live registration after the abort: $body"
echo "   the abort reached the in-flight subrequest in $elapsed ms and left no registration behind"

s=$(status "$hello/")
echo "10. GET $hello/            -> $s (other isolates must still serve)"
[ "$s" = 200 ] || fail "hello stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: an AbortSignal's abort registration was disarmed from the event loop, outside the guest, by retiring the table entry the consumer's handle owns -- the host neither locked nor destroyed anything in the signal's own memory, so a disposer a turn had written into the action's kj::Function was never called; an armed registration still ran its action on abort, and one whose handle had been dropped did not; and a real subrequest carrying a signal was cancelled by aborting it, leaving the signal holding nothing"
