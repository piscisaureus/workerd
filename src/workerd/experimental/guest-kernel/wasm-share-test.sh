#!/usr/bin/env bash
# Checks, against a built workerd, that two workers can instantiate, call and trap in the same
# Wasm modules from their own turns at guest ring 3. V8 holds two kinds of process-wide wasm state
# that a second worker reaches, and with a per-isolate heap arena behind every C++ allocation each
# one faults on what the first worker placed:
#
# The compiled module itself. V8 keeps one NativeModule per distinct set of wire bytes for the
# whole process, so two workers that supply the same module get the same NativeModule, the same
# wire bytes and the same shared_ptr owning them, in the arena of whichever compiled first. The
# second worker's instantiation faults incrementing that refcount, in
# wasm::InstanceBuilder::InstanceBuilder under WasmEngine::SyncInstantiate. Under heap arenas
# workerd turns the sharing off (--no-wasm-native-module-cache), so each worker compiles its own
# copy into its own arena.
#
# The index of code ranges. WasmCodeManager::lookup_map_ maps each code space region to its
# NativeModule, and LookupCode() descends it from whichever isolate is running to resolve a
# program counter for a trap, for stack iteration and for the deoptimizer. A second worker's trap
# faults on a node the first worker's compile placed. That map is placed in the shared heap
# (patches/v8/0051-Index-wasm-code-ranges-under-an-embedder-shared-allocation-scope.patch).
#
# Steps 1 to 6 drive the first, steps 7 and 8 the second, each in both directions. A negative
# build fails at one named step and not the other: --copt=-DWORKERD_GK_NEG_WASM_MODULE_CACHE at
# step 2 and --copt=-DWORKERD_GK_NEG_WASM_CODE_MANAGER at step 7.
#
# Usage: wasm-share-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_WASM_SHARE_TEST_HELLO_PORT / _FIRST_PORT / _SECOND_PORT override the ports
#   (8885 / 8886 / 8887).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_WASM_SHARE_TEST_HELLO_PORT:-8885}
first_port=${GK_WASM_SHARE_TEST_FIRST_PORT:-8886}
second_port=${GK_WASM_SHARE_TEST_SECOND_PORT:-8887}
hello=http://127.0.0.1:$hello_port
first=http://127.0.0.1:$first_port
second=http://127.0.0.1:$second_port
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
fail() {
  echo "FAIL: $*"
  echo "--- workerd log ---"
  cat "$log"
  exit 1
}
# Prints the body of a GET, or "no-response" if the connection failed or timed out.
body() {
  curl -s --max-time 30 "$1" || echo no-response
}
status() {
  curl -s --max-time 30 -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
# Requires a step's response to hold $2, and reports a condemnation as what it is.
expect() {  # $1 what, $2 needle, $3 body
  case "$3" in
    *"$2"*) return 0 ;;
  esac
  grep -q "isolate condemned" "$log" &&
    fail "$1 faulted inside its turn and its isolate was condemned: $3"
  fail "$1 did not answer with $2: $3"
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
  "$workerd" serve "$here/wasm-share-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "first=127.0.0.1:$first_port" \
  --socket-addr "second=127.0.0.1:$second_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

# Every worker's modules are evaluated at startup, so first and second have both compiled by the
# time the first request arrives; only hello and first are polled here because second answering
# is step 2's business.
for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$first/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, first on $first, second on $second"

b=$(body "$first/instantiate?mod=add")
echo "1. GET $first/instantiate?mod=add  -> ${b%$'\n'} (first compiles the module)"
expect "first's instantiation" '"result":3' "$b"

b=$(body "$second/instantiate?mod=add")
echo "2. GET $second/instantiate?mod=add -> ${b%$'\n'} (the same module, from second's turn)"
expect "second's instantiation of the module first compiled" '"result":3' "$b"
kill -0 "$pid" 2>/dev/null || fail "workerd died on second's instantiation"

b=$(body "$second/instantiate?mod=oob")
echo "3. GET $second/instantiate?mod=oob -> ${b%$'\n'} (the second module, from second's turn)"
expect "second's instantiation of the other module" '"result":0' "$b"

b=$(body "$first/instantiate?mod=oob")
echo "4. GET $first/instantiate?mod=oob  -> ${b%$'\n'} (back in first's turn)"
expect "first's instantiation of the other module" '"result":0' "$b"

b=$(body "$second/lazy?mod=add&n=200")
echo "5. GET $second/lazy?mod=add&n=200  -> ${b%$'\n'} (200 instantiations and calls in one turn)"
expect "second's repeated instantiation" '"sum":600' "$b"

b=$(body "$first/lazy?mod=add&n=200")
echo "6. GET $first/lazy?mod=add&n=200   -> ${b%$'\n'} (and 200 in first's)"
expect "first's repeated instantiation" '"sum":600' "$b"

b=$(body "$second/trap")
echo "7. GET $second/trap            -> ${b%$'\n'} (a trap in second's turn, resolved through the code manager)"
expect "second's trap" '"caught":"RuntimeError"' "$b"
expect "second's trapping stack" '"stack":"has-wasm-frame"' "$b"

b=$(body "$first/trap")
echo "8. GET $first/trap             -> ${b%$'\n'} (and one in first's turn)"
expect "first's trap" '"caught":"RuntimeError"' "$b"
expect "first's trapping stack" '"stack":"has-wasm-frame"' "$b"

s=$(status "$hello/")
echo "9. GET $hello/ -> $s (the bystander isolate must still serve)"
[ "$s" = 200 ] || fail "the bystander isolate stopped serving"

grep -q "isolate condemned" "$log" && fail "an isolate was condemned during the run"
kill -0 "$pid" 2>/dev/null || fail "workerd died"

echo "PASS: two isolates instantiated, called and trapped in the same two Wasm modules from their own turns, in both orders, with no isolate condemned: no part of a module crosses an arena boundary and the index of code ranges is reachable from every isolate"
