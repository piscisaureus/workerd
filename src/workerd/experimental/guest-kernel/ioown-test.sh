#!/usr/bin/env bash
# Checks, against a built workerd, that an IoOwn (io/io-own.h) names its object by a handle the
# host validates rather than by a pointer: a JS turn that rewrites the handle inside its own
# object leaves the runtime refusing the later uses -- the next turn's dereference, and the
# host's unlink when the object's holder is destroyed -- rather than reaching the address the
# turn chose; and that a handle rewritten to another isolate's live handle names that isolate's
# object, which is the ownership residual the table cannot close. With WORKERD_GK_TEST_PROBE set
# three request paths drive this from inside the turn (see maybeForceGuestFaultForTest in
# src/workerd/api/global-scope.c++):
#
# /__gk_ioown_corrupt creates an IoOwn in the turn, inside a heap holder that stands in for the
# JS wrapper's C++ object. A later turn of the same request first dereferences it as it is (the
# control: the handle resolves to the object) and then overwrites the handle with the address of
# a buffer the turn chose, laid out as the object the handle named and carrying a magic of its
# own. The turn after that dereferences the IoOwn and logs what it reached: it must be refused,
# not read the chosen magic. The holder is then destroyed on the event loop, outside any turn, which hands the
# handle to the delete queue, and a further turn makes the host drain that queue, host-side,
# before entering the guest: the host must refuse the handle rather than unlink through the
# chosen address.
#
# /__gk_ioown_node_corrupt, on the node worker, aims at the record behind the handle instead: the
# owned-list node in host memory, whose links the host's walk follows and whose disposer the host
# calls. The turn creates an IoOwn; the host, on the event loop, finds the node behind its handle
# and announces its address; a later turn writes a slot and a disposer of its choosing into the
# node's link and disposer fields, as a write primitive would. The write must fault at the node --
# the turn is abandoned and its isolate condemned -- and the context's teardown, host-side, must
# then dispose of the object through the disposer the node recorded, never calling the one the
# turn chose. (Where the write lands, as in the WORKERD_GK_NEG_IOOWN_NODE build, the holder is
# destroyed on the event loop and the next turn's host-side drain follows the rewritten node:
# the chosen disposer logs that the host called it, and this script fails.)
#
# /__gk_ioown_announce, on the other worker, creates an IoOwn and publishes its handle; the
# worker's JS keeps that request open for three seconds, so its context, which owns the object
# and retires the entry when it ends, outlives the next step. /__gk_ioown_steal, on the victim,
# rewrites one of its own IoOwns to that handle and dereferences it in a later turn. The table
# validates the handle and finds it live, so the use reads the other isolate's magic: the
# residual, stated here so that a change which closes it (an owner check the host decides) is a
# deliberate change to this expectation. Once the announcing request has ended,
# /__gk_ioown_steal_again shows the same stolen handle refused: the entry went with the context.
#
# Usage: ioown-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_IOOWN_TEST_HELLO_PORT / GK_IOOWN_TEST_VICTIM_PORT / GK_IOOWN_TEST_OTHER_PORT /
#   GK_IOOWN_TEST_NODE_PORT override the ports (8885 / 8884 / 8883 / 8882).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_IOOWN_TEST_HELLO_PORT:-8885}
victim_port=${GK_IOOWN_TEST_VICTIM_PORT:-8884}
other_port=${GK_IOOWN_TEST_OTHER_PORT:-8883}
node_port=${GK_IOOWN_TEST_NODE_PORT:-8882}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
other=http://127.0.0.1:$other_port
node=http://127.0.0.1:$node_port
log=$(mktemp)
chosen_magic=badbadbadbadbad0
other_magic=e7e70e7e70e7e70

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
  "$workerd" serve "$here/ioown-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  --socket-addr "other=127.0.0.1:$other_port" --socket-addr "node=127.0.0.1:$node_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] &&
    [ "$(status "$other/")" = 200 ] && [ "$(status "$node/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, victim on $victim, other on $other, node on $node"

s=$(status "$victim/__gk_ioown_corrupt")
echo "1. GET $victim/__gk_ioown_corrupt -> $s (the in-turn overwrite of the handle must land)"
[ "$s" = 200 ] || fail "the request that overwrote the handle did not complete"
created=$(grep -o "created an IoOwn inside the JS turn.*" "$log" | head -n1)
[ -n "$created" ] || fail "no IoOwn was created"
handle=$(echo "$created" | grep -o '0x[0-9a-f]*' | sed -n 1p)
object=$(echo "$created" | grep -o '0x[0-9a-f]*' | sed -n 2p)
wait_for "IoOwn use after the overwrite; what = intact" || fail "the control turn did not use the IoOwn"
use=$(grep -o "IoOwn use after the overwrite; what = intact.*" "$log" | head -n1)
echo "   control: the next turn's use of the intact handle: ${use#*verdict = }"
echo "$use" | grep -q "read magic 0x600d600d600d600d at $object" || fail "the intact handle did not resolve to the object in a later turn: $use"
wait_for "overwrote the IoOwn's handle from inside the JS turn; " || fail "the handle was not overwritten"
over=$(grep -o "overwrote the IoOwn's handle from inside the JS turn; .*" "$log" | head -n1)
chosen=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 2p)
chosen_obj=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 3p)
echo "   handle $handle (object at $object) overwritten with $chosen (a buffer the turn chose, whose object at $chosen_obj carries magic 0x$chosen_magic)"
[ -n "$handle" ] && [ -n "$chosen" ] && [ "$chosen" != "$handle" ] || fail "no handle values in: $over"

wait_for "IoOwn use after the overwrite; what = rewritten" || fail "the later turn did not use the IoOwn"
use=$(grep -o "IoOwn use after the overwrite; what = rewritten.*" "$log" | head -n1)
echo "2. the next turn's use of the IoOwn: ${use#*verdict = }"
echo "$use" | grep -q "verdict = refused" || fail "the use was not refused: $use"
echo "$use" | grep -q "$chosen_magic" && fail "the use read the magic at the address the turn chose"
echo "$use" | grep -q "does not name a live owned object" || fail "the refusal is not the handle check: $use"

wait_for "the turn after the holder's destruction ran" || fail "the holder was not destroyed and drained"
drain=$(grep -o "IoOwn handle does not name a live owned object; nothing is released.*" "$log" | head -n1)
echo "3. host-side drain of the destroyed holder's handle: ${drain:-NOT REFUSED}"
[ -n "$drain" ] || fail "the host did not refuse the handle when draining the delete queue"
echo "$drain" | grep -qi "${chosen#0x}" || fail "the refused handle is not the chosen value: $drain"
grep -q "destroying the IoOwn's holder on the event loop" "$log" || fail "the holder was not destroyed on the event loop"

s=$(status "$victim/")
echo "4. GET $victim/            -> $s (the isolate must still serve: the refusal failed nothing but the use)"
[ "$s" = 200 ] || fail "the victim stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

curl -s -o /dev/null "$other/__gk_ioown_announce" &
announce_pid=$!
wait_for "announced this isolate's IoOwn for another worker's turn" || fail "the announce request did not run"
echo "5. GET $other/__gk_ioown_announce (in flight: the worker holds the request open for 3 s)"
ann=$(grep -o "announced this isolate's IoOwn for another worker's turn.*" "$log" | head -n1)
ann_handle=$(echo "$ann" | grep -o '0x[0-9a-f]*' | sed -n 1p)
ann_obj=$(echo "$ann" | grep -o '0x[0-9a-f]*' | sed -n 2p)
[ -n "$ann_handle" ] || fail "no announced handle in: $ann"
wait_for "IoOwn use after the overwrite; what = announced-own" || fail "other's later turn did not use its own IoOwn"
use=$(grep -o "IoOwn use after the overwrite; what = announced-own.*" "$log" | head -n1)
echo "   control: other's own later use of it: ${use#*verdict = }"
echo "$use" | grep -q "read magic 0x$other_magic at $ann_obj" || fail "other's own handle did not resolve in its later turn: $use"
echo "   other's IoOwn handle $ann_handle names its object at $ann_obj (magic 0x$other_magic)"
s=$(status "$victim/__gk_ioown_steal")
echo "6. GET $victim/__gk_ioown_steal -> $s (victim rewrites one of its IoOwns to other's handle)"
[ "$s" = 200 ] || fail "the steal request failed"
wait_for "IoOwn use after the overwrite; what = cross-isolate" || fail "the later turn did not use the stolen handle"
use=$(grep -o "IoOwn use after the overwrite; what = cross-isolate.*" "$log" | head -n1)
echo "7. the next turn's use of the stolen handle: ${use#*verdict = }"
echo "$use" | grep -q "read magic 0x$other_magic at $ann_obj" ||
  fail "the cross-isolate rewrite did not name the other isolate's object (the documented residual): $use"

wait "$announce_pid"
sleep 0.5
echo "8. the announcing request has ended; its context retires the entry with its owned list"
s=$(status "$victim/__gk_ioown_steal_again")
echo "9. GET $victim/__gk_ioown_steal_again -> $s"
[ "$s" = 200 ] || fail "the second steal request failed"
wait_for "IoOwn use after the overwrite; what = cross-isolate-after-release" || fail "the later turn did not use the stolen handle again"
use=$(grep -o "IoOwn use after the overwrite; what = cross-isolate-after-release.*" "$log" | head -n1)
echo "   the stolen handle after other's context ended: ${use#*verdict = }"
echo "$use" | grep -q "verdict = refused" || fail "the stolen handle still resolved after the other context ended: $use"

s=$(status "$hello/")
echo "10. GET $hello/            -> $s (other isolates must still serve)"
[ "$s" = 200 ] || fail "hello stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

s=$(status "$node/__gk_ioown_node_corrupt")
echo "11. GET $node/__gk_ioown_node_corrupt -> $s (creates an IoOwn whose node a later turn rewrites)"
[ "$s" = 200 ] || fail "the node probe request did not complete"
wait_for "the owned node behind the IoOwn, found host-side" || fail "the host did not find the node"
found=$(grep -o "the owned node behind the IoOwn, found host-side.*" "$log" | head -n1)
node_handle=$(echo "$found" | grep -o '0x[0-9a-f]*' | sed -n 1p)
node_addr=$(echo "$found" | grep -o '0x[0-9a-f]*' | sed -n 2p)
[ -n "$node_addr" ] && [ "$node_addr" != "0x0" ] || fail "no node address in: $found"
echo "    the node behind handle $node_handle is at $node_addr, found host-side through the entry's companion"
wait_for "rewriting the owned node from inside the JS turn" || fail "the later turn did not attempt the rewrite"
rewrite=$(grep -o "rewriting the owned node from inside the JS turn.*" "$log" | head -n1)
chosen_slot=$(echo "$rewrite" | grep -o '0x[0-9a-f]*' | sed -n 2p)
chosen_disposer=$(echo "$rewrite" | grep -o '0x[0-9a-f]*' | sed -n 3p)
echo "    the turn writes link = $chosen_slot and disposer = $chosen_disposer into the node at $node_addr"
wait_for "isolate condemned" || fail "the in-turn write to the node did not fault"
condemned=$(grep -o "isolate condemned.*" "$log" | head -n1)
echo "$condemned" | grep -q "JS turn faulted inside the guest" || fail "the isolate was condemned for another reason: $condemned"
fault_dec=$(echo "$condemned" | grep -o "gk_fault_addr() = [0-9]*" | grep -o "[0-9]*$")
[ -n "$fault_dec" ] || fail "no fault address in: $condemned"
fault_addr=$(printf '0x%x' "$fault_dec")
node_dec=$((node_addr))
# The write lands on the node's link field, inside the node.
[ "$fault_dec" -ge "$node_dec" ] && [ "$fault_dec" -lt $((node_dec + 64)) ] ||
  fail "the fault is not at the node ($node_addr): $condemned"
echo "12. the write faulted at $fault_addr, inside the node at $node_addr (offset $((fault_dec - node_dec))): ${condemned%%;*}"
grep -q "the owned node was rewritten from inside the JS turn; the write landed" "$log" && fail "the write to the node landed"
wait_for "disposed of through its own disposer; .*0xde0de0de0de0de0" || fail "the context's teardown did not dispose of the node probe's object"
echo "13. the context's teardown, host-side, walked the list and disposed of the object through the disposer the node recorded"
grep -q "the chosen disposer was called by the host" "$log" && fail "the host called the disposer the turn chose: $(grep -o 'the chosen disposer was called by the host.*' "$log" | head -n1)"
echo "    the chosen disposer was never called"

s=$(status "$hello/")
echo "14. GET $hello/            -> $s (other isolates must still serve)"
[ "$s" = 200 ] || fail "hello stopped serving after the node fault"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: an IoOwn handle rewritten from inside a turn to an address the turn chose was refused by the next turn's dereference and by the host's drain of the delete queue, reaching neither the chosen buffer nor its magic; the isolate kept serving; a handle rewritten to another isolate's live handle named that isolate's object (the ownership residual, not closed by the handle check) and was refused once that isolate's context had ended; a turn's write into the owned node behind a handle faulted at the node, and the host's teardown walk disposed of the object through the node's own disposer, never the chosen one"
