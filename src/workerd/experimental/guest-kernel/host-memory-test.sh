#!/usr/bin/env bash
# Checks, against a built workerd, that the channel token key lives in guest-kernel host memory
# and that a JS turn at guest ring 3 cannot read it: with WORKERD_GK_TEST_PROBE set the server
# logs the key's address and whether it is in host memory, and a request to /__gk_probe_host
# reads one byte of it from inside the turn (see maybeForceGuestFaultForTest in
# src/workerd/api/global-scope.c++). That read must fault at the key's address, failing only that
# request and condemning only that isolate, while the other worker keeps serving.
#
# Usage: host-memory-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_HOST_TEST_HELLO_PORT / GK_HOST_TEST_VICTIM_PORT override the ports (8895 / 8894).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_HOST_TEST_HELLO_PORT:-8895}
victim_port=${GK_HOST_TEST_VICTIM_PORT:-8894}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
log=$(mktemp)

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

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/fault-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello and victim on $victim"

# The key's address and host-memory status, as the server logged them at startup.
line=$(grep -o 'WORKERD_GK_TEST_PROBE: channel token key.*' "$log" | head -n1)
[ -n "$line" ] || fail "the server did not log the token key's location"
key=$(echo "$line" | grep -o '0x[0-9a-f]*' | head -n1)
inhost=$(echo "$line" | grep -o 'contains(key) = [a-z]*' | sed 's/.*= //')
echo "1. token key at $key, in host memory: $inhost"
[ -n "$key" ] || fail "no key address in: $line"
[ "$inhost" = true ] || fail "the token key is not in host memory"

s=$(status "$victim/__gk_probe_host")
echo "2. GET $victim/__gk_probe_host -> $s (the in-turn read of the key must fail the request)"
[ "$s" != 200 ] || fail "the request that read the key succeeded"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
grep -q "reading host memory from inside the JS turn" "$log" || fail "the probe did not run"

# The turn's fault was at the key's address: runInGuest reports gk_fault_addr() in decimal.
keydec=$((key))
faultline=$(grep -o 'JS turn faulted inside the guest.*' "$log" | head -n1)
[ -n "$faultline" ] || fail "no guest fault was reported"
faultaddr=$(echo "$faultline" | grep -o 'gk_fault_addr() = [0-9]*' | sed 's/.*= //')
echo "3. guest fault at $(printf '0x%x' "$faultaddr") (key at $key)"
[ "$faultaddr" = "$keydec" ] || fail "the fault was not at the key's address: $faultline"
grep -q "isolate condemned\|has been condemned" "$log" || fail "no condemnation logged"

s=$(status "$victim/")
echo "4. GET $victim/            -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"

s=$(status "$hello/")
echo "5. GET $hello/             -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest fault"

echo "PASS: the token key is in host memory; a ring-3 read of it faulted at its address, failed one request, condemned one isolate, process kept serving"
