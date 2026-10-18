#!/usr/bin/env bash
# Checks, against a built workerd, that a JS turn cannot leave the host looking at an event loop
# of the turn's choosing. Every host-side lookup of event-loop state starts from one thread-local
# pointer (threadLocalEventLoop in kj/async.c++): the loop an event is armed on, the executor
# getCurrentThreadExecutor hands out, and the local map EventLoop::getLocal reads, where the async
# lock queue keeps its waiter. (Turning the loop is not reached that way: WaitScope holds its loop
# by reference and asserts the pointer still names it.) That pointer is
# ordinary thread-local storage, which a turn at guest ring 3 can write, and kj exposes no
# accessor for it, so the host saves it in the frame that enters the guest and puts it back as
# soon as the guest returns (runTrampolineInGuest in src/workerd/jsg/guest-run.c++, on
# EventLoop::saveCurrentLoopPointer from
# patches/capnp-cpp/0004-Expose-the-thread-s-current-event-loop-pointer.patch).
#
# With WORKERD_GK_TEST_PROBE set, two request paths on the "victim" worker drive this (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++). Both rewrite the pointer as
# the turn's last act in the guest -- the rest of the turn arms its own events through it, so a
# rewrite earlier would break the turn before the host got to read it. Both read the pointer back
# at ring 3 afterwards, so the log shows the store landing rather than faulting the guest, which
# is what makes this thread-local a hazard at all:
#
# /__gk_loop_repoint builds a kj::EventLoop of its own inside the turn and leaves the pointer
# naming it, which is what a turn aiming the pointer at a loop it controls leaves behind.
# /__gk_loop_clear leaves the pointer null, the other state such a turn can leave, and the one a
# fault can leave as well.
#
# /__gk_loop_firing does the same for the other half of that state, the event this thread is
# firing (threadLocalCurrentlyFiring in kj/async.c++). kj clears that one when the fire() that ran
# the turn returns, and with a constant rather than a saved value, so nothing a turn leaves in it
# outlives that fire() -- but the rest of that fire() is host code running on what the turn left,
# and ~Event() compares the pointer with every event destroyed on the thread from a noexcept
# function that aborts the process on a match. The host-side report for this one is taken inside
# that same fire(), where it must find the event the turn ran under.
#
# In the first two cases the host must find the loop the turn ran on. Two host-side lookups report
# it, both through kj::EventLoop::Id::current(), the host's own path to the loop: the first once the
# turn is over but while the isolate lock is still held (Worker::Lock::runAtNextReleaseForTest,
# run from ~Worker::Lock), the second on the event loop once the lock is gone. "restored" is the
# loop the turn ran on, "repointed" the loop the turn built, "cleared" the lookup failing
# outright.
#
# Usage: event-loop-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_LOOP_TEST_HELLO_PORT / GK_LOOP_TEST_VICTIM_PORT override the ports (8805 / 8804).
#
# Negative build, never shipped, which this script must fail:
#   --copt=-DWORKERD_GK_NEG_LOOP_POINTER  neither pointer is put back, so the host looks at
#       the loop the turn chose: the first lookup after the turn logs it repointed, and the
#       process then dies releasing the isolate lock, on ~AsyncLockWaiter's noexcept assert that
#       the loop's local map still names the waiter -- the map it looks in is the one belonging to
#       the loop the turn built, which is empty. Step 1 reports no response.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_LOOP_TEST_HELLO_PORT:-8805}
victim_port=${GK_LOOP_TEST_VICTIM_PORT:-8804}
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
fail() { echo "FAIL: $*"; echo "--- workerd log ---"; cat "$log"; exit 1; }
status() { curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$1" || echo no-response; }
# Waits for the Nth line matching a pattern and prints it, stripped of the log prefix.
wait_line() {  # $1 pattern, $2 occurrence
  for _ in $(seq 1 100); do
    [ "$(grep -c "$1" "$log")" -ge "$2" ] && break
    sleep 0.1
  done
  grep -o "$1.*" "$log" | sed -n "${2}p" | sed 's/^WORKERD_GK_TEST_PROBE: //'
}
last_field() { echo "$1" | sed -n 's/.*= //p'; }
field() { echo "$2" | sed -n "s/.*$1 = \\([^;]*\\).*/\\1/p"; }

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/event-loop-test.capnp" \
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

# A loop of the turn's own left in the pointer.
s=$(status "$victim/__gk_loop_repoint")
echo "1. GET $victim/__gk_loop_repoint -> $s (the in-turn rewrite of the pointer must land)"
[ "$s" = 200 ] || fail "the request that rewrote the event loop pointer did not complete"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the turn rewrote the event loop pointer"
over=$(wait_line "rewrote this thread's event loop pointer from inside the JS turn" 1)
[ -n "$over" ] || fail "the event loop pointer was not rewritten"
echo "   $over"
# The turn read the pointer back after writing it, at ring 3: the store landed rather than
# faulting the guest, which is what makes this thread-local a hazard at all.
[ "$(field 'addr(chosen)' "$over")" = "$(field 'addr(readBack)' "$over")" ] ||
  fail "the in-turn write of the event loop pointer did not land"
held=$(wait_line "the event loop pointer on the host after the turn" 1)
[ -n "$held" ] || fail "the host-side lookup after the turn did not run"
echo "2. $held"
[ "$(last_field "$held")" = restored ] ||
  fail "host code holding the lock after the turn found the loop the turn left ($(last_field "$held"))"
after=$(wait_line "the event loop pointer on the event loop" 1)
[ -n "$after" ] || fail "the event-loop lookup did not run"
echo "3. $after"
[ "$(last_field "$after")" = restored ] ||
  fail "the event loop found the loop the turn left ($(last_field "$after"))"

# The same pointer left null.
s=$(status "$victim/__gk_loop_clear")
echo "4. GET $victim/__gk_loop_clear -> $s (the in-turn clear of the pointer must land)"
[ "$s" = 200 ] || fail "the request that cleared the event loop pointer did not complete"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the turn cleared the event loop pointer"
over=$(wait_line "rewrote this thread's event loop pointer from inside the JS turn" 2)
[ -n "$over" ] || fail "the event loop pointer was not cleared"
echo "   $over"
[ "$(field 'addr(readBack)' "$over")" = 0x0 ] ||
  fail "the in-turn clear of the event loop pointer did not land"
held=$(wait_line "the event loop pointer on the host after the turn" 2)
[ -n "$held" ] || fail "the host-side lookup after the clearing turn did not run"
echo "5. $held"
[ "$(last_field "$held")" = restored ] ||
  fail "host code holding the lock after the turn found no loop ($(last_field "$held"))"
after=$(wait_line "the event loop pointer on the event loop" 2)
[ -n "$after" ] || fail "the event-loop lookup after the clearing turn did not run"
echo "6. $after"
[ "$(last_field "$after")" = restored ] ||
  fail "the event loop found no loop of its own ($(last_field "$after"))"

# The event the thread is firing, the other half of the same state.
s=$(status "$victim/__gk_loop_firing")
echo "7. GET $victim/__gk_loop_firing -> $s (the in-turn rewrite of the firing event must land)"
[ "$s" = 200 ] || fail "the request that rewrote the firing event did not complete"
over=$(wait_line "rewrote the firing event from inside the JS turn" 1)
[ -n "$over" ] || fail "the firing event was not rewritten"
echo "   $over"
[ "$(field 'addr(FIRING_SENTINEL_FOR_TEST)' "$over")" = "$(field 'addr(readBack)' "$over")" ] ||
  fail "the in-turn write of the firing event did not land"
held=$(wait_line "the firing event on the host after the turn" 1)
[ -n "$held" ] || fail "the host-side read of the firing event did not run"
echo "8. $held"
[ "$(last_field "$held")" = restored ] ||
  fail "host code in the fire() that ran the turn read the event the turn left ($(last_field "$held"))"

grep -q "No event loop is running on this thread" "$log" &&
  fail "a host-side lookup of the thread's event loop failed"
grep -q "Promise callback destroyed itself" "$log" &&
  fail "an event was destroyed while the thread was recorded as firing it"
grep -q "isolate condemned" "$log" && fail "an isolate was condemned"
s=$(status "$victim/")
echo "9. GET $victim/ -> $s (the worker keeps serving after the rewrites)"
[ "$s" = 200 ] || fail "the victim stopped serving"
s=$(status "$hello/")
echo "10. GET $hello/ -> $s (the other worker keeps serving)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a turn that left this thread's event loop pointer naming a loop of its own, one that left it null, and one that left a sentinel in the event the thread is firing, did not reach the host: the host-side lookups after each turn found the loop the turn ran on and the event it ran under, and the process kept serving"
