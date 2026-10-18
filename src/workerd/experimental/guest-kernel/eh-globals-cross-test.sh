#!/usr/bin/env bash
# Checks, against a built workerd, that a JS turn can poison the C++ exception runtime's
# per-thread caughtExceptions pointer of a thread that throws and catches a C++ exception while
# holding no isolate lock, and that the thread's __cxa_end_catch then calls through a forged
# cleanup pointer -- an indirect call to a guest-chosen address, the sharpest technique-A
# thread-local steer (memory-classes.md R7, the eh_globals entry).
#
# The victim thread with that shape in a workerd serve is the module fallback service's thread
# (FallbackServiceClient::threadMain, server/fallback-service.c++), which throws and catches on
# every failed fetch. This config starts the real fallback thread (moduleFallback plus
# --experimental), and /__gk_fallback_poke drives a real failed resolution so it throws and
# catches for real. But the server's services share one event-loop thread, and that thread blocks
# in FallbackServiceClient::tryResolve for the whole of a real resolution, so no concurrent JS
# turn can write the fallback thread's caughtExceptions in the window between __cxa_begin_catch and
# __cxa_end_catch. So, as event-loop-cross-test.sh does for the loop pointer, the steer is driven
# against a stand-in: a thread that throws and catches a real kj::Exception and holds inside the
# real catch handler, so the turn writes the word in-window; the consumer is the real libc++abi
# __cxa_end_catch either way.
#
# With WORKERD_GK_TEST_PROBE set, three request paths drive this (see maybeForceGuestFaultForTest
# in src/workerd/api/global-scope.c++):
#
# /__gk_eh_arm on "victim" starts the stand-in host-side, identifies its copy of eh_globals by
# constant delta from a thread-local of workerd's own (eh_globals sits at the same offset in every
# thread's static TLS block), and builds a forged __cxa_exception whose unwindHeader.exception_
# class is zero (foreign) and whose exception_cleanup is a sentinel in unmapped memory.
#
# /__gk_eh_poison on "victim" writes the forged pointer into the stand-in's caughtExceptions from
# inside its JS turn, at guest ring 3. The stand-in then exits its catch; __cxa_end_catch takes the
# Itanium ABI foreign-exception path and _Unwind_DeleteException calls through the forged cleanup,
# transferring control to the sentinel, which faults at that exact rip. A SIGSEGV handler on the
# stand-in records the faulting rip.
#
# /__gk_eh_check on "victim" reports the outcome host-side.
#
# A pass requires the faulting rip to be the sentinel, not any incidental fault. A build that
# refused moduleFallback under the guest kernel (the candidate remedy) would fail to start this
# config, failing the probe, which is the point.
#
# Usage: eh-globals-cross-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_EH_TEST_HELLO_PORT / GK_EH_TEST_VICTIM_PORT override the ports (8819 / 8818).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_EH_TEST_HELLO_PORT:-8819}
victim_port=${GK_EH_TEST_VICTIM_PORT:-8818}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
sentinel=5e5e5e5e5e5e
log=$(mktemp)

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

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/eh-globals-cross-test.capnp" --experimental \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup (a build that refused moduleFallback under the guest kernel would do this)"
  sleep 0.1
done
[ "$(status "$hello/")" = 200 ] || fail "hello never started"
[ "$(status "$victim/")" = 200 ] || fail "victim never started: moduleFallback plus --experimental did not come up"
echo "workerd pid $pid serving hello on $hello, victim (moduleFallback) on $victim"

s=$(status "$victim/__gk_fallback_poke")
echo "1. GET $victim/__gk_fallback_poke -> $s (a real failed resolution, so the real fallback thread throws and catches)"
[ "$s" = 200 ] || fail "the fallback poke did not complete"
poke=$(wait_line "Fallback service failed to fetch module" 1)
[ -n "$poke" ] || fail "the real fallback thread did not log a failed fetch: the moduleFallback path is not live"
echo "   the real fallback thread threw and caught a C++ exception on the failed fetch"

s=$(status "$victim/__gk_eh_arm")
echo "2. GET $victim/__gk_eh_arm -> $s (start the stand-in and identify its eh_globals)"
[ "$s" = 200 ] || fail "the arm request did not complete"
armed=$(wait_line "the fallback-thread stand-in's exception runtime" 1)
[ -n "$armed" ] || fail "the stand-in was not armed"
echo "   $armed"
[ "$(field deltaVerdict "$armed")" = match ] ||
  fail "eh_globals was not identified at a constant delta from the anchor (reach not established)"
[ "$(field forgedVerdict "$armed")" = forged ] ||
  fail "the forged exception was not built (no native header to derive the unwindHeader offset from)"
hoff=$(field headerOffset "$armed")
[ -n "$hoff" ] && [ "$hoff" -ge 0 ] 2>/dev/null ||
  fail "the unwindHeader offset was not derived"

s=$(status "$victim/__gk_eh_poison")
echo "3. GET $victim/__gk_eh_poison -> $s (the in-turn write of the stand-in's caughtExceptions)"
kill -0 "$pid" 2>/dev/null || fail "workerd died when the turn touched the stand-in's eh_globals"
[ "$s" = 200 ] ||
  fail "the turn did not complete, so the word is no longer in its reach: the residual the memory-classes.md R7 row records has been closed, and it and this probe must be rewritten"
wrote=$(wait_line "wrote the fallback-thread stand-in's caughtExceptions from inside the JS turn" 1)
[ -n "$wrote" ] || fail "the in-turn write was not logged"
echo "   $wrote"
[ "$(field afterAt "$wrote")" = "$(field forgedAt "$wrote")" ] ||
  fail "the turn's write of caughtExceptions did not read back inside the turn"

echo "4. waiting for the stand-in's __cxa_end_catch to call through the forged cleanup pointer"
steer=$(wait_line "the C++ unwinder called through the forged cleanup pointer" 1)
[ -n "$steer" ] ||
  fail "__cxa_end_catch did not reach the forged cleanup pointer: the forged value was not consumed on the stand-in thread"
echo "   $steer"
rip=$(echo "$steer" | sed -n 's/.*rip=0x\([0-9a-f]*\).*/\1/p')
case "$rip" in
  *$sentinel) echo "   the faulting rip is the sentinel 0x$rip: control reached the guest-chosen address" ;;
  *) fail "the fault's rip 0x$rip is not the sentinel ($sentinel): an incidental fault, not the steer" ;;
esac

s=$(status "$victim/__gk_eh_check")
echo "5. GET $victim/__gk_eh_check -> $s (the outcome host-side)"
[ "$s" = 200 ] || fail "the check request did not complete"
held=$(wait_line "the steer's outcome on the stand-in thread" 1)
[ -n "$held" ] || fail "the host-side check did not run"
echo "   $held"
[ "$(field verdict "$held")" = sentinel ] ||
  fail "the host reads the outcome as $(field verdict "$held"), not the sentinel"

s=$(status "$hello/")
echo "6. GET $hello/ -> $s (the other worker keeps serving: the steer crashed no one else)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: a JS turn poisoned a non-isolate-lock-holding thread's caughtExceptions pointer and the"
echo "      real libc++abi __cxa_end_catch called through the forged cleanup pointer, reaching the"
echo "      sentinel address 0x$rip -- an indirect call to a guest-chosen address, driven against a"
echo "      stand-in for the module fallback service's thread for the reason stated at the top"
