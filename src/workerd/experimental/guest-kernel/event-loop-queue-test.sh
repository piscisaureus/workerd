#!/usr/bin/env bash
# Checks, against a built workerd under the guest kernel, that a JS turn's write to the handle its
# event loop names its run queue by (kj::EventLoop::queueHandle, patches/capnp-cpp/0005) cannot
# redirect what EventLoop::turn()/head() dereferences. The run queue -- the sentinels, free list and
# links -- lives in host memory no arena maps; the loop reaches it through a handle resolved on
# every host-side use against the guest kernel's slot table, not a pointer. The handle sits in the
# shared-heap loop object, which a turn can write (the probe shows the write landing at ring 3), but
# a value a turn forges names no live slot and resolves to nothing, so the host walks nothing the
# turn chose. Before the relocation the queue head was in the shared heap and a turn that wrote it
# aimed turn() straight at an event and vtable of its choosing; this is the proof that no longer
# holds.
#
# With WORKERD_GK_TEST_PROBE set, one request path on the "victim" worker drives this (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++):
#
# /__gk_queue_head_poison builds a fake run queue in the turn's own memory, writes its address into
# the loop's queue handle from inside the turn, reads it back (the write lands, at ring 3), and puts
# the real handle back before the turn returns so the loop keeps working. Host-side after the turn
# it resolves both the real handle and the forged value through the slot table exactly as the loop
# does: the real one names the loop's queue, the forged one resolves to nothing -- "inert".
#
# /__gk_queue_count_forge drives the companion proof for the journal the turn's in-turn arms and
# disarms go through. The metadata the host trusts to apply it -- the record cursor and which
# loop/queue the records belong to -- is in host memory a turn cannot write, and the host applies
# the journal by walking its full capacity rather than the cursor. So a turn that forges the cursor
# to zero cannot drop its own disarm and leave a host-queue link pointing at an event it has freed:
# the disarm still runs -- "inert".
#
# Usage: event-loop-queue-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_LOOPQ_TEST_HELLO_PORT / GK_LOOPQ_TEST_VICTIM_PORT override the ports (8805 / 8804).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_LOOPQ_TEST_HELLO_PORT:-8805}
victim_port=${GK_LOOPQ_TEST_VICTIM_PORT:-8804}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
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
cleanup() { stop_workerd "$pid"; rm -f "$log"; }
fail() { echo "FAIL: $*"; echo "--- workerd log ---"; cat "$log"; exit 1; }
status() { curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$1" || echo no-response; }
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
  "$workerd" serve "$here/event-loop-queue-test.capnp" \
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

s=$(status "$victim/__gk_queue_head_poison")
echo "1. GET $victim/__gk_queue_head_poison -> $s (the in-turn rewrite of the queue handle must land)"
[ "$s" = 200 ] || fail "the request that rewrote the queue handle did not complete"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the turn rewrote the queue handle"

over=$(wait_line "rewrote the loop's queue handle from inside the JS turn" 1)
[ -n "$over" ] || fail "the queue handle was not rewritten"
echo "   $over"
# The turn read the handle back after writing it, at ring 3: the store landed rather than faulting
# the guest, which is what makes the handle a turn-writable value at all.
[ "$(field 'addr(forged)' "$over")" = "$(field 'addr(readBack)' "$over")" ] ||
  fail "the in-turn write of the queue handle did not land"

res=$(wait_line "queue handle resolution on the host" 1)
[ -n "$res" ] || fail "the host-side resolution did not run"
echo "2. $res"
[ "$(last_field "$res")" = inert ] ||
  fail "the forged queue handle was not inert on the host (it resolved to a queue the turn chose)"
# The forged handle resolved to nothing, the real one to the loop's queue.
[ "$(field 'addr(forgedResolved)' "$res")" = 0x0 ] ||
  fail "the forged queue handle resolved to a non-null address"
[ "$(field 'addr(realResolved)' "$res")" != 0x0 ] ||
  fail "the real queue handle resolved to nothing (the loop had no host-memory queue)"

# The loop still works after the turn put its real handle back.
s=$(status "$victim/")
echo "3. GET $victim/ -> $s (the loop still serves after the poisoned turn)"
[ "$s" = 200 ] || fail "the victim stopped serving after the poisoned turn"
s=$(status "$hello/")
[ "$s" = 200 ] || fail "hello stopped serving"

# The journal the in-turn arms and disarms go through. A turn that forges the record cursor to zero
# cannot drop its own disarm: the host applies the journal by walking its full capacity, not the
# cursor, and reaches the queue through a per-thread word a turn cannot write, so the disarm still
# runs and no host-queue link is left on the freed event. /__gk_queue_count_forge drives this on the
# host -- arm two events, apply, record a disarm of the first at a high slot with the cursor forged
# to zero, apply again -- and reports the verdict.
s=$(status "$victim/__gk_queue_count_forge")
echo "4. GET $victim/__gk_queue_count_forge -> $s (the count-forge probe must complete)"
[ "$s" = 200 ] || fail "the count-forge probe request did not complete"
kill -0 "$pid" 2>/dev/null || fail "workerd died running the count-forge probe"

forge=$(wait_line "event journal count forge on the host" 1)
[ -n "$forge" ] || fail "the count-forge probe did not run on the host"
echo "5. $forge"
[ "$(last_field "$forge")" = inert ] ||
  fail "a forged journal cursor dropped a disarm (a host-queue link was left on a freed event)"

echo "PASS: a turn's write to the loop's queue handle is inert and a forged journal cursor cannot drop a disarm; turn() dereferences only the host-memory queue"
