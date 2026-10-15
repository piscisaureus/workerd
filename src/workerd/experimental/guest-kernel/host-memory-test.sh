#!/usr/bin/env bash
# Checks, against a built workerd, that the channel token key lives in guest-kernel host memory,
# that the handle through which the host reaches it cannot be redirected from a JS turn, and that
# a JS turn at guest ring 3 cannot read the key. With WORKERD_GK_TEST_PROBE set the server logs
# the key's address and whether it is in host memory, and two request paths exercise it from
# inside the turn (see maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++):
#
# /__gk_corrupt_handle overwrites the handler's stored key handle, from inside the turn, with
# the address of a buffer the turn chose, as a write primitive would a pointer; the write lands,
# since the handler is in the ordinary heap, and the turn completes. The host then uses the key
# on the event loop, as the real uses do: that use must be refused -- the guest kernel reports
# the handle as not naming a live slot and the use throws -- and must not resolve the key at the
# chosen address. With the handle restored, the use must resolve the key where it was.
#
# /__gk_probe_host reads one byte of the key from inside the turn. That read must fault at the
# key's address, failing only that request and condemning only that isolate, while the other
# worker keeps serving.
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

# From inside a turn, overwrite the handle with the address of a guest-chosen buffer. The write
# itself succeeds (the handler is heap memory the turn can write) and the turn completes.
s=$(status "$victim/__gk_corrupt_handle")
echo "2. GET $victim/__gk_corrupt_handle -> $s (the in-turn overwrite of the handle must land)"
[ "$s" = 200 ] || fail "the request that overwrote the handle did not complete"
over=$(grep -o "overwrote the tenant's handle from inside the JS turn.*" "$log" | head -n1)
[ -n "$over" ] || fail "the handle was not overwritten"
oldhandle=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 1p)
chosen=$(echo "$over" | grep -o '0x[0-9a-f]*' | sed -n 2p)
echo "   handle $oldhandle overwritten with $chosen (a buffer the turn chose)"
[ -n "$oldhandle" ] && [ -n "$chosen" ] || fail "no handle values in: $over"
[ "$chosen" != "$oldhandle" ] || fail "the chosen value equals the handle"

# The host's use runs after the turn, on the event loop; wait for it and the restore.
for _ in $(seq 1 100); do
  grep -q "restored the tenant's handle" "$log" && break
  sleep 0.1
done
grep -q "restored the tenant's handle" "$log" || fail "the host-side use of the key did not run"
refused=$(grep -o "host use of the token key was refused.*" "$log" | head -n1)
gkline=$(grep -o "gk_host_slot_get: $chosen is not the handle of a live host slot.*" "$log" | head -n1)
echo "3. host use through the rewritten handle: ${refused:-NOT REFUSED}"
echo "   guest kernel: ${gkline:-no report}"
[ -n "$refused" ] || fail "the host used the key through the rewritten handle"
[ -n "$gkline" ] || fail "the guest kernel did not report the rewritten handle"
if grep "resolved it at" "$log" | grep -q "$chosen"; then
  fail "the host resolved the key at the guest-chosen address $chosen"
fi
# The first "resolved" line is the use after the restore: the key is back where it was.
resolved=$(grep -o "resolved it at.*" "$log" | head -n1 | grep -o '0x[0-9a-f]*' | head -n1)
echo "4. host use after the restore resolved the key at ${resolved:-nothing} (key at $key)"
[ "$resolved" = "$key" ] || fail "after the restore the key did not resolve where it was"
[ "$(grep -c 'resolved it at' "$log")" = 1 ] || fail "the key resolved more than once"

s=$(status "$victim/__gk_probe_host")
echo "5. GET $victim/__gk_probe_host -> $s (the in-turn read of the key must fail the request)"
[ "$s" != 200 ] || fail "the request that read the key succeeded"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
grep -q "reading host memory from inside the JS turn" "$log" || fail "the probe did not run"

# The turn's fault was at the key's address: runInGuest reports gk_fault_addr() in decimal.
keydec=$((key))
faultline=$(grep -o 'JS turn faulted inside the guest.*' "$log" | head -n1)
[ -n "$faultline" ] || fail "no guest fault was reported"
faultaddr=$(echo "$faultline" | grep -o 'gk_fault_addr() = [0-9]*' | sed 's/.*= //')
echo "6. guest fault at $(printf '0x%x' "$faultaddr") (key at $key)"
[ "$faultaddr" = "$keydec" ] || fail "the fault was not at the key's address: $faultline"
grep -q "isolate condemned\|has been condemned" "$log" || fail "no condemnation logged"

s=$(status "$victim/")
echo "7. GET $victim/            -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"

s=$(status "$hello/")
echo "8. GET $hello/             -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest fault"

echo "PASS: the token key is in host memory; a handle rewritten from inside a turn resolved to nothing and the host refused the use; a ring-3 read of the key faulted at its address, failed one request, condemned one isolate, process kept serving"
