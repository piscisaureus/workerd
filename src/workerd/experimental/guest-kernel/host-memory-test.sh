#!/usr/bin/env bash
# Checks, against a built workerd, that the channel token key and the host entropy buffer live in
# guest-kernel host memory, that the handle through which the host reaches the key cannot be
# redirected from a JS turn, that a JS turn at guest ring 3 can read neither, and that the nonce
# of a token the host mints does not come from the thread-local entropy buffer a turn can
# overwrite. With WORKERD_GK_TEST_PROBE set the server logs each tenant's address and whether it
# is in host memory, and four request paths exercise them from inside the turn (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++):
#
# /__gk_corrupt_handle overwrites the handler's stored key handle, from inside the turn, with
# the address of a buffer the turn chose, as a write primitive would a pointer; the write lands,
# since the handler is in the ordinary heap, and the turn completes. The host then uses the key
# on the event loop, as the real uses do: that use must be refused -- the guest kernel reports
# the handle as not naming a live slot and the use throws -- and must not resolve the key at the
# chosen address. With the handle restored, the use must resolve the key where it was.
#
# /__gk_poison_entropy overwrites the whole of the thread's getEntropy() buffer with 0x5a from
# inside the turn and marks it unconsumed. The host then mints an RPC token on the event loop and
# logs its nonce, and draws twelve bytes from getEntropy() on the same thread: the draw must be
# twelve bytes of 0x5a, showing the turn's write decides what that path hands the host next, and
# the nonce must not be, showing the token's nonce is drawn from elsewhere.
#
# /__gk_probe_host reads one byte of the key, and /__gk_probe_host_entropy one byte of the host
# entropy buffer, from inside the turn. Each read must fault at the byte's own address, failing
# only that request and condemning only that isolate, while the other workers keep serving.
#
# Usage: host-memory-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_HOST_TEST_HELLO_PORT / GK_HOST_TEST_VICTIM_PORT / GK_HOST_TEST_VICTIM2_PORT override the
#   ports (8895 / 8894 / 8893).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_HOST_TEST_HELLO_PORT:-8895}
victim_port=${GK_HOST_TEST_VICTIM_PORT:-8894}
victim2_port=${GK_HOST_TEST_VICTIM2_PORT:-8893}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
victim2=http://127.0.0.1:$victim2_port
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
# The address and host-memory status of a tenant, as the server logged them at startup.
tenant() {
  line=$(grep -o "WORKERD_GK_TEST_PROBE: $1.*" "$log" | head -n1)
  [ -n "$line" ] || fail "the server did not log where the $1 lives"
  addr=$(echo "$line" | grep -o '0x[0-9a-f]*' | head -n1)
  inhost=$(echo "$line" | grep -o 'contains([a-z]*) = [a-z]*' | sed 's/.*= //')
  [ -n "$addr" ] || fail "no address in: $line"
  [ "$inhost" = true ] || fail "the $1 is not in host memory: $line"
}
# The address the guest fault that condemned isolate $1 was reported at: the condemnation line
# carries the fault, with gk_fault_addr() in decimal.
fault_addr() {
  faultline=$(grep -o "isolate condemned.*id = $1;.*JS turn faulted inside the guest.*" "$log" | head -n1)
  [ -n "$faultline" ] || fail "no guest fault condemned isolate $1"
  echo "$faultline" | grep -o 'gk_fault_addr() = [0-9]*' | sed 's/.*= //'
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/host-memory-test.capnp" \
  --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
  --socket-addr "victim2=127.0.0.1:$victim2_port" \
  >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 100); do
  [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] &&
    [ "$(status "$victim2/")" = 200 ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
echo "workerd pid $pid serving hello on $hello, victim on $victim, victim2 on $victim2"

tenant "channel token key"
key=$addr
echo "1. token key at $key, in host memory: $inhost"
tenant "host entropy buffer"
buf=$addr
echo "2. host entropy buffer at $buf, in host memory: $inhost"

# From inside a turn, overwrite the handle with the address of a guest-chosen buffer. The write
# itself succeeds (the handler is heap memory the turn can write) and the turn completes.
s=$(status "$victim/__gk_corrupt_handle")
echo "3. GET $victim/__gk_corrupt_handle -> $s (the in-turn overwrite of the handle must land)"
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
echo "4. host use through the rewritten handle: ${refused:-NOT REFUSED}"
echo "   guest kernel: ${gkline:-no report}"
[ -n "$refused" ] || fail "the host used the key through the rewritten handle"
[ -n "$gkline" ] || fail "the guest kernel did not report the rewritten handle"
if grep "resolved it at" "$log" | grep -q "$chosen"; then
  fail "the host resolved the key at the guest-chosen address $chosen"
fi
# The first "resolved" line is the use after the restore: the key is back where it was.
resolved=$(grep -o "resolved it at.*" "$log" | head -n1 | grep -o '0x[0-9a-f]*' | head -n1)
echo "5. host use after the restore resolved the key at ${resolved:-nothing} (key at $key)"
[ "$resolved" = "$key" ] || fail "after the restore the key did not resolve where it was"
[ "$(grep -c 'resolved it at' "$log")" = 1 ] || fail "the key resolved more than once"

# From inside a turn, fill the thread's getEntropy() buffer with a pattern; the host then mints
# a token and draws from getEntropy() on the same thread.
pattern=5a5a5a5a5a5a5a5a5a5a5a5a
s=$(status "$victim/__gk_poison_entropy")
echo "6. GET $victim/__gk_poison_entropy -> $s (the in-turn overwrite of the entropy buffer must land)"
[ "$s" = 200 ] || fail "the request that poisoned the entropy buffer did not complete"
grep -q "overwrote this thread's entropy buffer from inside the JS turn" "$log" ||
  fail "the entropy buffer was not overwritten"
for _ in $(seq 1 100); do
  grep -q "getEntropy() on the host after the turn returned" "$log" && break
  sleep 0.1
done
drawn=$(grep -o "getEntropy() on the host after the turn returned.*" "$log" | head -n1 |
  grep -o '[0-9a-f]\{24\}' | head -n1)
nonce=$(grep -o "minted a token on the host with nonce.*" "$log" | head -n1 |
  grep -o '[0-9a-f]\{24\}' | head -n1)
echo "   getEntropy() on the host afterwards returned ${drawn:-nothing} (the pattern is $pattern)"
echo "   the token the host minted carries nonce  ${nonce:-nothing}"
[ "$drawn" = "$pattern" ] || fail "the turn's write did not reach the host's getEntropy() draw"
[ -n "$nonce" ] || fail "no token was minted on the host"
[ "$nonce" != "$pattern" ] || fail "the token's nonce is the pattern the turn wrote"
[ "$nonce" != "$drawn" ] || fail "the token's nonce is what getEntropy() returned"

s=$(status "$victim/__gk_probe_host")
echo "7. GET $victim/__gk_probe_host -> $s (the in-turn read of the key must fail the request)"
[ "$s" != 200 ] || fail "the request that read the key succeeded"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
grep "reading host memory from inside the JS turn" "$log" | grep -q "token-key" ||
  fail "the key probe did not run"
faultaddr=$(fault_addr victim)
echo "8. guest fault at $(printf '0x%x' "$faultaddr") (key at $key)"
[ "$faultaddr" = "$((key))" ] || fail "the fault was not at the key's address"

s=$(status "$victim/")
echo "9. GET $victim/            -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"

s=$(status "$victim2/__gk_probe_host_entropy")
echo "10. GET $victim2/__gk_probe_host_entropy -> $s (the in-turn read of the entropy buffer must fail the request)"
[ "$s" != 200 ] || fail "the request that read the host entropy buffer succeeded"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
grep "reading host memory from inside the JS turn" "$log" | grep -q "host-entropy" ||
  fail "the entropy buffer probe did not run"
faultaddr=$(fault_addr victim2)
echo "11. guest fault at $(printf '0x%x' "$faultaddr") (host entropy buffer at $buf)"
[ "$faultaddr" = "$((buf))" ] || fail "the fault was not at the entropy buffer's address"

s=$(status "$victim2/")
echo "12. GET $victim2/           -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the second faulted isolate served a request"

s=$(status "$hello/")
echo "13. GET $hello/             -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest faults"

echo "PASS: the token key and the host entropy buffer are in host memory; a handle rewritten from inside a turn resolved to nothing and the host refused the use; a turn's overwrite of the thread-local entropy buffer reached the host's next getEntropy() draw but not the nonce of the token the host minted; ring-3 reads of the key and of the buffer each faulted at the address read, failed one request and condemned one isolate; process kept serving"
