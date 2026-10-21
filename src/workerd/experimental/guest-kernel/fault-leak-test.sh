#!/usr/bin/env bash
# Checks that two pieces of thread-wide state a faulted turn's frames hold cost no more than the
# turn: the scope that disallows destroying kj::AsyncObjects, and the flag that says this thread
# is inside a CppgcShim destructor. Both are entered by frames inside a turn -- the first by
# Wrappable::maybeDeferDestruction around every destruction of a JS heap object under the isolate
# lock, the second by Wrappable::CppgcShim's destructor -- and both are read by the host after
# the turn. See the leak list on runInGuest in src/workerd/jsg/guest-run.h.
#
# fault-test.sh faults in a fetch handler before the turn has taken any host state, and
# fault-timeout-test.sh faults with a timeout's running mark held. This one faults with each of
# these two held instead, through stand-in routes (see maybeForceGuestFaultForTest in
# src/workerd/api/global-scope.c++), and requires that the process keep serving and that the host
# read the flag clear afterwards.
#
# What the two leaks cost without the save and restore in runTrampolineInGuest:
#   - the disallow scope is reached through a thread-local pointer to the scope object on the
#     turn's stack, so the fault leaves it naming an abandoned frame; AsyncObject's destructor
#     dereferences it from a noexcept function, so the next promise node this thread destroys --
#     which the fault's own unwinding does -- takes the process down;
#   - the flag stays set for the rest of the process on this thread, so every Data the host
#     destroys afterwards skips resetting its TracedReference, which leaks the node, and
#     HeapTracer::clearWrappers() asserts the flag is clear when any isolate on this thread is
#     torn down.
#
# Negative builds (never shipped), each failing this script:
#   --copt=-DWORKERD_GK_NEG_DISALLOW_DTORS  the pointer the turn left stands; step 2 fails on the
#       dead process
#   --copt=-DWORKERD_GK_NEG_CPPGC_FLAG      the flag the turn left stands; step 4 reads it leaked
#
# Usage: fault-leak-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_FAULT_LEAK_TEST_PORT overrides the first port (8871); dtors and gcflag take the next two.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
keeper_port=${GK_FAULT_LEAK_TEST_PORT:-8871}
dtors_port=$((keeper_port + 1))
gcflag_port=$((keeper_port + 2))
keeper=http://127.0.0.1:$keeper_port
dtors=http://127.0.0.1:$dtors_port
gcflag=http://127.0.0.1:$gcflag_port
log=$(mktemp)

# Stops workerd without waiting for it indefinitely: it drains in-flight requests on SIGTERM, so
# a process that still holds a hung request -- what a failed probe can leave behind -- is killed
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
  echo "--- faults, condemnations and aborts ---"
  grep -aE "forcing a guest fault|isolate condemned|being destroyed when not allowed|recursive initialization|cppgc-destructor flag" "$log" | head -n 10
  echo "--- workerd log ---"
  grep -avE "^\[gk\] vcpu [0-9]+(:? (syscall|.*: (wall|root switch)))" "$log" | tail -n 40
  exit 1
}
# Prints the HTTP status of a GET, or "no-response" if the connection failed.
status() {
  curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$1" || echo no-response
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_FAULT=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/fault-leak-test.capnp" \
  --socket-addr "keeper=127.0.0.1:$keeper_port" --socket-addr "dtors=127.0.0.1:$dtors_port" \
  --socket-addr "gcflag=127.0.0.1:$gcflag_port" >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$keeper/")" = 200 ] && [ "$(status "$dtors/")" = 200 ] &&
    [ "$(status "$gcflag/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
s=$(status "$keeper/")
[ "$s" = 200 ] || fail "keeper did not come up (got $s)"
echo "workerd pid $pid serving keeper on $keeper, dtors on $dtors and gcflag on $gcflag"

echo "1. all three isolates serve before any fault"
for u in "$keeper" "$dtors" "$gcflag"; do
  s=$(status "$u/")
  [ "$s" = 200 ] || fail "$u did not serve before the fault (got $s)"
done

s=$(status "$dtors/__gk_fault_disallow_dtors")
echo "2. GET $dtors/__gk_fault_disallow_dtors -> $s (request must fail: the turn faulted)"
[ "$s" != 200 ] || fail "the request that faulted inside the disallow scope succeeded"
grep -aq "forcing a guest fault inside a scope that disallows async destructors" "$log" ||
  fail "the fault was not taken inside the disallow scope"
grep -aq "isolate condemned" "$log" || fail "no condemnation logged"
grep -aq "KJ async object being destroyed when not allowed" "$log" &&
  fail "an async object was destroyed in the scope the faulted turn abandoned"
kill -0 "$pid" 2>/dev/null ||
  fail "workerd died on the fault taken inside the disallow scope"
# The faulted request's unwinding and its IoContext's teardown destroy promise nodes on this
# thread, which is what an abandoned disallow scope turns fatal. Keep the other isolates busy
# across it and require the process to be alive throughout.
for i in $(seq 1 20); do
  s=$(status "$keeper/")
  [ "$s" = 200 ] || fail "keeper stopped serving after the disallow fault (attempt $i: $s)"
  kill -0 "$pid" 2>/dev/null ||
    fail "workerd died during the teardown of the request that faulted in the disallow scope"
  sleep 0.1
done
echo "   the process outlived that request's teardown and keeper answered 20 times"

s=$(status "$gcflag/__gk_fault_gc_dtor")
echo "3. GET $gcflag/__gk_fault_gc_dtor       -> $s (request must fail: the turn faulted)"
[ "$s" != 200 ] || fail "the request that faulted with the cppgc flag set succeeded"
grep -aq "forcing a guest fault with the cppgc-destructor flag set" "$log" ||
  fail "the fault was not taken with the cppgc-destructor flag set"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the fault taken with the cppgc flag set"

s=$(status "$keeper/__gk_gc_dtor_check")
echo "4. GET $keeper/__gk_gc_dtor_check       -> $s (host reads the flag on the event loop)"
[ "$s" = 200 ] || fail "keeper did not serve the check (got $s)"
for _ in $(seq 1 50); do
  grep -aq "the cppgc-destructor flag on the event loop" "$log" && break
  sleep 0.1
done
verdict=$(grep -a "the cppgc-destructor flag on the event loop" "$log" | tail -n 1)
[ -n "$verdict" ] || fail "the host never reported the cppgc-destructor flag"
echo "   $verdict"
case "$verdict" in
  *"cppgcFlag = clear") ;;
  *"cppgcFlag = leaked") fail "the host read the flag the faulted turn left set" ;;
  *) fail "unrecognised verdict for the cppgc-destructor flag" ;;
esac

echo "5. the condemned isolates must not run again and keeper must still serve"
for u in "$dtors" "$gcflag"; do
  s=$(status "$u/")
  [ "$s" != 200 ] || fail "a condemned isolate served a request ($u)"
done
s=$(status "$keeper/")
[ "$s" = 200 ] || fail "keeper stopped serving (got $s)"
kill -0 "$pid" 2>/dev/null || fail "workerd died before the end of the run"

echo "PASS: faults inside a disallow-async-destructors scope and with the cppgc-destructor flag set failed one request each, condemned one isolate each, left the process serving, and left the host reading the flag clear"
