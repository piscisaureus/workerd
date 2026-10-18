#!/usr/bin/env bash
# Checks, against a built workerd, what a JS turn can reach of *another* thread's event loop
# state. The save and restore around a turn (runTrampolineInGuest in src/workerd/jsg/guest-run.c++)
# puts this thread's current-loop pointer back when the guest returns, which covers a turn that
# repoints its own thread's pointer (event-loop-test.sh). It can say nothing about a turn that
# repoints another thread's: that thread's own turns and host-side lookups would run on a loop
# this turn chose, and no frame on this thread would ever put it back. Whether that is reachable
# is a question about the arena's mappings, not about the save. It is reachable: a thread's static
# TLS block is ordinary process memory, which every arena maps writable, so this probe pins the
# residual rather than preventing it. A build that closed it would fail here, which is the point --
# the memory-classes.md rows that record the residual would have to change with it. What the
# residual costs depends on a thread model this tree does not exercise -- the server's services
# share one event loop thread -- so what this probe establishes is the reach, not an exploit here.
#
# With WORKERD_GK_TEST_PROBE set, three request paths drive this (see maybeForceGuestFaultForTest
# in src/workerd/api/global-scope.c++):
#
# /__gk_loop_other_arm on "checker" starts a parked host thread with a kj::EventLoop of its own
# and identifies that thread's copy of kj's current-loop pointer, both host-side -- a turn could
# do neither, being clone() and process_vm_readv(). The slot is found by offset from a
# thread-local of workerd's own: the offset at which this thread's word holds this thread's loop
# and the other thread's holds the other thread's loop names it, and the number of offsets that
# match is logged so a coincidence would show.
#
# /__gk_loop_other_poison on "victim" reads and writes that word from inside its JS turn, at guest
# ring 3, and logs the word before and after. Memory no arena maps would fault the turn there,
# failing the request and condemning the victim isolate; this word is mapped, so both land.
#
# /__gk_loop_other_check on "checker" reports host-side what the word holds -- "intact" or
# "overwritten" -- and puts the other thread's own value back either way.
#
# Usage: event-loop-cross-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_LOOPX_TEST_HELLO_PORT / GK_LOOPX_TEST_VICTIM_PORT / GK_LOOPX_TEST_CHECKER_PORT override the
#   ports (8808 / 8807 / 8806).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_LOOPX_TEST_HELLO_PORT:-8808}
victim_port=${GK_LOOPX_TEST_VICTIM_PORT:-8807}
checker_port=${GK_LOOPX_TEST_CHECKER_PORT:-8806}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
checker=http://127.0.0.1:$checker_port
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
wait_line() {  # $1 pattern, $2 occurrence
  for _ in $(seq 1 100); do
    [ "$(grep -c "$1" "$log")" -ge "$2" ] && break
    sleep 0.1
  done
  grep -o "$1.*" "$log" | sed -n "${2}p" | sed 's/^WORKERD_GK_TEST_PROBE: //'
}
field() { echo "$2" | sed -n "s/.*$1 = \\([^;]*\\).*/\\1/p"; }
last_field() { echo "$1" | sed -n 's/.*= //p'; }

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/event-loop-cross-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  --socket-addr "checker=127.0.0.1:$checker_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] &&
    [ "$(status "$checker/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, victim on $victim, checker on $checker"

s=$(status "$checker/__gk_loop_other_arm")
echo "1. GET $checker/__gk_loop_other_arm -> $s (start the other thread and find its slot)"
[ "$s" = 200 ] || fail "the arm request did not complete"
armed=$(wait_line "another thread's event loop pointer" 1)
[ -n "$armed" ] || fail "the other thread was not armed"
echo "   $armed"
[ "$(field 'theirAnchorAt' "$armed")" != 0x0 ] ||
  fail "the other thread never published its TLS anchor"
matches=$(last_field "$armed")
slot=$(field 'slotAt' "$armed")
if [ "$slot" = 0x0 ]; then
  echo "   note: no offset identified kj's pointer in the other thread's TLS block; the probe"
  echo "         reads and writes the anchor word instead, which is in the same block"
else
  [ "$matches" = 1 ] || fail "$matches offsets matched, so the slot is not identified"
fi

s=$(status "$victim/__gk_loop_other_poison")
echo "2. GET $victim/__gk_loop_other_poison -> $s (the in-turn read and write of that word)"
kill -0 "$pid" 2>/dev/null || fail "workerd died when the turn touched another thread's TLS"
[ "$s" = 200 ] ||
  fail "the turn did not complete, so the word is no longer in its reach: the residual the memory-classes.md rows record has been closed, and they and this probe must be rewritten"
wrote=$(wait_line "read and wrote another thread's TLS from inside the JS turn" 1)
[ -n "$wrote" ] || fail "the in-turn access was not logged"
echo "   $wrote"
theirLoop=$(field 'theirLoopAt' "$armed")
[ "$(field 'wasHex' "$wrote")" = "$theirLoop" ] ||
  fail "the turn read $(field 'wasHex' "$wrote") rather than the other thread's loop $theirLoop"
[ "$(field 'nowHex' "$wrote")" = 0xdeadbeefdeadbeef ] ||
  fail "the turn's write did not read back inside the turn"

s=$(status "$checker/__gk_loop_other_check")
echo "3. GET $checker/__gk_loop_other_check -> $s (what the word holds, host-side)"
[ "$s" = 200 ] || fail "the check request did not complete"
held=$(wait_line "another thread's TLS on the host" 1)
[ -n "$held" ] || fail "the host-side check did not run"
echo "   $held"
[ "$(last_field "$held")" = overwritten ] ||
  fail "the turn logged the write but the host reads the word $(last_field "$held")"
echo "4. another thread's event loop state is writable from this turn: its static TLS block is"
echo "   process memory the arena maps, so the pointer that thread's own turns and lookups start"
echo "   from can be made to name a loop this turn chose, which no save on this thread undoes"

s=$(status "$checker/")
echo "5. GET $checker/ -> $s (the checker keeps serving)"
[ "$s" = 200 ] || fail "the checker stopped serving"
s=$(status "$hello/")
echo "6. GET $hello/ -> $s (the other worker keeps serving)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: another thread's copy of kj's current-loop pointer is readable and writable from a JS turn, which this probe records rather than prevents: the turn read the loop that thread made current and left a sentinel the host read back, the word was put back host-side, and the process kept serving"
