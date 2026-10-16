#!/usr/bin/env bash
# Checks, against a built workerd, that a worker cannot drive the process's log output by forcing
# the refusal paths. The words and handles a turn hands the host are the turn's own memory, so a
# turn with a write primitive chooses what they say, and everything they can say that the host
# refuses is reported: unbudgeted, one worker can make the host write hundreds of megabytes a
# second, which costs the host its disk and every other tenant its diagnostics. The reports are
# therefore budgeted per site and per second, out of counters in gk's control block that ring 3
# can neither read nor write (gk_log_budget in gk.h, hostMemory::logBudget in
# util/host-memory.h). What must survive the budget is the first report at each site: these
# messages are the evidence that a tenant is attacking the runtime, and a cap that could hide the
# first one would trade a noisy denial of service for a silent compromise.
#
# With WORKERD_GK_TEST_PROBE set two request paths drive this from inside the turn (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++):
#
# /__gk_log_flood journals 20000 notes of a kind the host does not know. A note spends no
# reserved entry and the journal refills itself, so this is the cheapest refusal there is to
# force and the only one a turn can make for as long as it runs; the host reads each note and
# refuses it, host-side, after the turn has returned.
#
# /__gk_log_flood_turn is the same flood from inside the turn: 5000 holders released at ring 3
# whose handle names an entry of another class, each refused in the turn's own code. No entry is
# spent and the turn is not ended, so the turn can make the refusal as often as it can make a
# holder. Ring 3 cannot reach the budget, so the verdict on each of these comes from the host
# through the hostcall the turn's journal already uses.
#
# Usage: log-flood-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_LOG_FLOOD_TEST_PORT overrides the port (8879).
#
# Not for the handler-allocation guard build
# (--//src/workerd/experimental/guest-kernel:handler_alloc_guard=True). A report the budget
# lets through from inside the refill hostcall is still a KJ_LOG, which formats and so
# allocates, and a handler must not take the heap -- the guest chose the moment of the call and
# may hold the allocator. The budget bounds how often that happens; it does not remove it.
# rpc-context-test.sh's /__gk_refused_in_refill aborts that build for the same reason. Moving
# the reports a handler makes to the end of the turn is the fix, and is not this one.
#
# The negative build, which shows the flood this bounds:
#   bazel build -c opt --platform_suffix=_neg_logcap \
#     --copt=-DWORKERD_GK_NEG_LOG_CAP //src/workerd/server:workerd
#   ./log-flood-test.sh bazel-out/k8-opt-_neg_logcap/bin/src/workerd/server/workerd
# Every report is made there, and this script must fail on the volume. The suffix keeps the
# negative build in a tree of its own, but bazel-bin still points at whichever tree was built
# last, so name the binary rather than taking the default.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_LOG_FLOOD_TEST_PORT:-8879}
flood=http://127.0.0.1:$port
log=$(mktemp)
# What one request forces, as the probes above count it.
notes=20000
drops=5000
# What the budget allows: GK_LOG_BURST reports per site per second, plus room for the windows a
# run of this length can cross and for the few extra that racing callers let through.
allowed=64

cleanup() {
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- workerd log (first 40 lines) ---"
  head -n 40 "$log"
  exit 1
}
status() {
  curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
errors() {  # $1 pattern: ERROR lines carrying it
  grep -c "error: $1" "$log"
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve --experimental "$here/log-flood-test.capnp" \
  --socket-addr "flood=127.0.0.1:$port" >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$flood/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
[ "$(status "$flood/")" = 200 ] || fail "the worker never started serving"
grep -q "guest-kernel isolation enabled" "$log" ||
  fail "the server is not running with guest-kernel isolation; the test would prove nothing"
echo "workerd pid $pid serving the flood worker on $flood, with guest-kernel isolation"

note_msg="a turn's note carries an unknown kind"
drop_msg="an IoOwn handle names an object of another class; nothing is released"

base=$(wc -c <"$log")
s=$(status "$flood/__gk_log_flood")
echo "1. GET $flood/__gk_log_flood -> $s ($notes notes of a kind the host does not know,"
echo "   journaled in one turn and refused by the host after it)"
[ "$s" = 200 ] || fail "the flood request did not complete"
made=$(grep -o "notes of an unknown kind journaled; made = [0-9]*" "$log" | tail -n1 |
  sed 's/.*= //')
[ "${made:-0}" -ge "$notes" ] || fail "the turn journaled ${made:-0} notes, not $notes"
n=$(errors "$note_msg")
echo "   the turn forced $made refusals; the host reported $n of them"
[ "$n" -ge 1 ] || fail "no refusal was reported: the first one must always be"
[ "$n" -le "$allowed" ] || fail "$n reports for $made refusals; the budget did not hold"

s=$(status "$flood/__gk_log_flood_turn")
echo "2. GET $flood/__gk_log_flood_turn -> $s ($drops holders released at ring 3 naming an"
echo "   entry of another class, each refused in the turn's own code)"
[ "$s" = 200 ] || fail "the in-turn flood request did not complete"
grep -q "holders dropped naming another class" "$log" || fail "the in-turn probe did not run"
d=$(errors "$drop_msg")
echo "   the turn forced $drops refusals at ring 3; the host let $d be reported"
[ "$d" -ge 1 ] || fail "no in-turn refusal was reported: the first one must always be"
[ "$d" -le "$allowed" ] || fail "$d reports for $drops refusals at ring 3; the budget did not hold"

# A second window, so that a report carries what the first one held back. Nothing of the turn's
# can reset that count: it is in gk's control block, which ring 3 faults on.
sleep 1.2
curl -s -o /dev/null "$flood/__gk_log_flood"
sleep 0.2
held=$(grep "error: $note_msg" "$log" | grep -o "suppressedSinceLast = [0-9]*" | sort -t= -k2 -n |
  tail -n1 | sed 's/.*= //')
echo "3. after a second window: the largest report says $held were held back since the last one"
[ "${held:-0}" -ge 1000 ] || fail "the reports held back were not counted: ${held:-0}"

total=$(($(wc -c <"$log") - base))
forced=$((notes * 2 + drops))
echo "4. $forced refusals forced in all; $(grep -c "error:" "$log") reports written,"
echo "   $total bytes of log in total (the probes' own lines included)"
[ "$total" -lt 200000 ] ||
  fail "$forced refusals drew $total bytes of log; the budget did not hold"

s=$(status "$flood/")
echo "5. GET $flood/           -> $s (the worker must still serve)"
[ "$s" = 200 ] || fail "the worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a worker that forced $forced refusals -- $notes journaled notes of a kind the host does not know, twice over, and $drops holders released at ring 3 naming an entry of another class -- drew $(grep -c "error:" "$log") reports and $total bytes of log; the first refusal of each kind was reported, and a report in the second window carried the $held that were held back"
