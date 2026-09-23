#!/usr/bin/env bash
# Exercises a broad V8 surface for a worker whose JS turn runs at guest ring 3: Wasm from
# config modules (including an out-of-bounds access), WebCrypto digest and getRandomValues,
# irregexp, and a JIT-heavy loop. Checks the guest kernel runs it all correctly and that a
# Wasm bounds violation surfaces as a WebAssembly.RuntimeError rather than a guest fault that
# would condemn the isolate.
#
# Usage: surface-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_SURFACE_TEST_PORT overrides the port (8897).
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_SURFACE_TEST_PORT:-8897}
base=http://127.0.0.1:$port
log=$(mktemp)
cleanup() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$log"; }
fail() { echo "FAIL: $*"; echo "--- workerd log ---"; cat "$log"; exit 1; }

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 "$workerd" serve "$here/surface-test.capnp" \
  --socket-addr "surf=127.0.0.1:$port" >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(curl -s -o /dev/null -w "%{http_code}" "$base/" || true)" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving on $base"

r=$(curl -s "$base/") || fail "no response"
echo "1. GET / -> $r"
echo "$r" | grep -q "\"wasmAdd\":42" || fail "wasm add wrong"
echo "$r" | grep -q "\"regex\":\"abc,def\"" || fail "regex wrong"
echo "$r" | grep -q "\"sha256\":\"2cf24dba\"" || fail "sha256 wrong"
echo "$r" | grep -q "\"rndLen\":8" || fail "getRandomValues wrong"
echo "$r" | grep -q "\"compute\":3205071072" || fail "jit compute wrong"

r=$(curl -s "$base/wasm-oob") || fail "no response (oob)"
echo "2. GET /wasm-oob -> $r"
echo "$r" | grep -q "\"oob\":\"threw:RuntimeError\"" || fail "wasm OOB did not surface as a JS RuntimeError"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the wasm OOB trap"

s=$(curl -s -o /dev/null -w "%{http_code}" "$base/") || true
echo "3. GET / after OOB -> $s (isolate must not be condemned)"
[ "$s" = 200 ] || fail "isolate was condemned by a wasm bounds trap"

echo "PASS: Wasm/WebCrypto/irregexp/JIT run at ring 3; a Wasm OOB trap is a JS RuntimeError, not a guest fault"
