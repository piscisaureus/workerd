#!/usr/bin/env bash
# Checks, against a built workerd, that the isolate's lock slot -- the V8 data slot jsg::Lock::from
# reads, which sits in the C++ isolate object in the shared runtime heap and so is writable from
# any isolate's JS turn at guest ring 3 -- does not hand the host a lock of a turn's choosing. The
# host puts the slot back from the stack frame that entered the guest once the turn returns
# (runTrampolineInGuest in src/workerd/jsg/guest-run.c++), the lock's destructor clears it rather
# than restoring a value kept in the heap-allocated lock, and the lock's constructor decides
# whether the lock is recursive from V8's thread manager rather than from the slot.
#
# With WORKERD_GK_TEST_PROBE set, four request paths drive this (see maybeForceGuestFaultForTest
# in src/workerd/api/global-scope.c++):
#
# /__gk_lock_slot on the "victim" worker overwrites, from inside the turn, the victim isolate's
# own lock slot with a sentinel (0x1), as the turn's last act in the guest (the rest of the turn
# calls jsg::Lock::from itself, at ring 3, and would fault on the sentinel first). Once the turn
# is over, host code that still holds the lock
# (Worker::Lock::runAtNextReleaseForTest, run from ~Worker::Lock) logs what jsg::Lock::from
# returns against the lock it holds: it must be that lock ("restored"), not the sentinel
# ("leaked"). An event-loop callback then logs the slot once the lock is gone: it must be 0x0.
#
# The same turn also overwrites the count of jsg::Locks this thread holds -- thread-local
# storage, which a turn can write as it can the slot, and which the lock's constructor reads to
# tell a second lock on an isolate this thread already has one for from a bare v8::Locker an
# embedder holds. The host saves and puts that back too (runTrampolineInGuest), so the same
# host-side report must find 1, the lock it holds, rather than the turn's sentinel.
#
# /__gk_lock_announce on the victim records the victim's isolate; /__gk_lock_poison_other on the
# "attacker" worker then overwrites that isolate's lock slot from inside the attacker's own turn,
# and an event-loop callback logs the slot to show the write landed (the isolate object is in the
# shared heap, which every arena maps writable). /__gk_lock_check on the victim then takes the
# victim's lock again: the process must survive it (a lock that trusted the slot would refuse the
# lock as recursive, which aborts a debug build), the host-side report must again find the host's
# own lock, and the slot must again be 0x0 afterwards.
#
# Usage: lock-slot-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_LOCK_TEST_HELLO_PORT / GK_LOCK_TEST_VICTIM_PORT / GK_LOCK_TEST_ATTACKER_PORT override the
#   ports (8835 / 8834 / 8833).
#
# Negative builds, never shipped, which this script must fail:
#   --copt=-DWORKERD_GK_NEG_LOCK_DEPTH  the count of locks the thread holds is not put back
#       after a turn, so the host reads the count the turn left.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_LOCK_TEST_HELLO_PORT:-8835}
victim_port=${GK_LOCK_TEST_VICTIM_PORT:-8834}
attacker_port=${GK_LOCK_TEST_ATTACKER_PORT:-8833}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
attacker=http://127.0.0.1:$attacker_port
log=$(mktemp)

cleanup() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$log"; }
fail() { echo "FAIL: $*"; echo "--- workerd log ---"; cat "$log"; exit 1; }
status() { curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response; }
# Waits for the Nth line matching a pattern and prints it, stripped of the log prefix.
wait_line() {  # $1 pattern, $2 occurrence
  for _ in $(seq 1 100); do
    [ "$(grep -c "$1" "$log")" -ge "$2" ] && break
    sleep 0.1
  done
  grep -o "$1.*" "$log" | sed -n "${2}p" | sed 's/^WORKERD_GK_TEST_PROBE: //'
}
last_field() { echo "$1" | sed -n 's/.*= //p'; }

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/lock-slot-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  --socket-addr "attacker=127.0.0.1:$attacker_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] &&
    [ "$(status "$attacker/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, victim on $victim, attacker on $attacker"

# Same isolate: the turn overwrites its own lock slot.
s=$(status "$victim/__gk_lock_slot")
echo "1. GET $victim/__gk_lock_slot -> $s (the in-turn overwrite of the lock slot must land)"
[ "$s" = 200 ] || fail "the request that overwrote its lock slot did not complete"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the turn overwrote its lock slot"
over=$(wait_line "overwrote this isolate's lock slot from inside the JS turn" 1)
[ -n "$over" ] || fail "the lock slot was not overwritten"
echo "   $over"
from=$(wait_line "jsg::Lock::from on the host after the turn" 1)
[ -n "$from" ] || fail "the host-side check after the turn did not run"
echo "2. $from"
[ "$(last_field "$from")" = restored ] ||
  fail "host code holding the lock after the turn got the turn's value from jsg::Lock::from ($(last_field "$from"))"
depth=$(wait_line "the lock count on the host after the turn" 1)
[ -n "$depth" ] || fail "the host-side lock-count check after the turn did not run"
echo "   $depth"
[ "$(last_field "$depth")" = restored ] ||
  fail "host code after the turn read the lock count the turn left ($(last_field "$depth"))"
after=$(wait_line "the lock slot on the event loop after the lock" 1)
[ -n "$after" ] || fail "the event-loop check did not run"
echo "3. $after"
[ "$(last_field "$after")" = 0x0 ] || fail "the lock slot held $(last_field "$after") after the lock was released"

# Cross-isolate: the attacker's turn overwrites the victim's lock slot.
s=$(status "$victim/__gk_lock_announce")
echo "4. GET $victim/__gk_lock_announce -> $s"
[ "$s" = 200 ] || fail "the announce request did not complete"
announced=$(wait_line "announced this isolate for another worker's turn" 1)
[ -n "$announced" ] || fail "the victim isolate was not announced"
echo "   $announced"
s=$(status "$attacker/__gk_lock_poison_other")
echo "5. GET $attacker/__gk_lock_poison_other -> $s (the attacker's in-turn overwrite of the victim's slot must land)"
[ "$s" = 200 ] || fail "the attacker's request did not complete"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the attacker overwrote the victim's lock slot"
over=$(wait_line "overwrote another isolate's lock slot from inside the JS turn" 1)
[ -n "$over" ] || fail "the victim's lock slot was not overwritten"
echo "   $over"
landed=$(wait_line "the other isolate's lock slot on the event loop" 1)
[ -n "$landed" ] || fail "the event-loop check of the victim's slot did not run"
echo "6. $landed"
[ "$(last_field "$landed")" = 0x1 ] || fail "the attacker's write did not land in the victim's slot ($(last_field "$landed"))"

s=$(status "$victim/__gk_lock_check")
echo "7. GET $victim/__gk_lock_check -> $s (the victim takes its lock with the slot holding the attacker's value)"
kill -0 "$pid" 2>/dev/null || fail "workerd died when the victim took its lock after the attacker's write"
[ "$s" = 200 ] || fail "the victim did not serve after the attacker's write"
from=$(wait_line "jsg::Lock::from on the host after the turn" 2)
[ -n "$from" ] || fail "the host-side check after the victim's turn did not run"
echo "8. $from"
[ "$(last_field "$from")" = restored ] ||
  fail "host code holding the victim's lock got the attacker's value from jsg::Lock::from ($(last_field "$from"))"
after=$(wait_line "the lock slot on the event loop after the lock" 2)
[ -n "$after" ] || fail "the event-loop check after the victim's lock did not run"
echo "9. $after"
[ "$(last_field "$after")" = 0x0 ] || fail "the victim's lock slot held $(last_field "$after") after its lock was released"

grep -q "recursive isolate lock" "$log" && fail "a lock was refused as recursive on the strength of the slot"
grep -q "isolate condemned" "$log" && fail "an isolate was condemned"
s=$(status "$victim/")
echo "10. GET $victim/ -> $s (the victim keeps serving)"
[ "$s" = 200 ] || fail "the victim stopped serving"
s=$(status "$attacker/")
echo "11. GET $attacker/ -> $s (the attacker keeps serving)"
[ "$s" = 200 ] || fail "the attacker stopped serving"
s=$(status "$hello/")
echo "12. GET $hello/ -> $s (the other worker keeps serving)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a turn's overwrite of its own lock slot and of its thread's count of held locks, and another isolate's turn's overwrite of the slot, did not reach the host: jsg::Lock::from on the host after each turn returned the lock the host held, the count it read was its own, the slot was clear once each lock was released, no lock was refused as recursive, and the process kept serving"
