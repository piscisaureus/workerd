#!/usr/bin/env bash
# Checks, against a built workerd, that a Durable Object namespace's HMAC key lives in guest-kernel
# host memory and that the ids a JS turn mints with it are still correct: newUniqueId() and
# idFromName() are synchronous to JS, so the HMAC runs host-side through a hostcall (see
# util/hostcall.h) from inside the turn. With WORKERD_GK_TEST_PROBE set the server logs where the
# key lives and whether it is in host memory, and the "victim" worker (actor-id-test.js) serves
# request paths that exercise the namespace from inside a turn:
#
# /mint returns newUniqueId(); three must differ, so the random half is not a constant.
# /do?id= reaches the Durable Object addressed by an id; the object counts its requests, so two
# requests to one minted id must return 1 then 2, and another id's object must start at 1 again:
# the server, host-side, validated the turn's id against the same key.
# /name?n= returns idFromName(); it must equal the id this script derives from the unique key in
# actor-id-test.capnp, so the key in host memory is the right key and not merely a consistent
# one; /long-name derives an id from a name longer than one hostcall frame carries, checked the
# same way, and /do-name reaches the object by name.
# /forged asks idFromString() to accept an id whose second half is not the HMAC of its first,
# which it must refuse.
# /__gk_probe_actor_key reads one byte of the key from inside the turn (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++): the read must fault at the
# byte's own address, failing only that request and condemning only that isolate, while the other
# worker keeps serving.
#
# Usage: actor-id-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm, curl and
#   python3. GK_ACTOR_TEST_HELLO_PORT / GK_ACTOR_TEST_VICTIM_PORT override the ports (8885 / 8884).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_ACTOR_TEST_HELLO_PORT:-8885}
victim_port=${GK_ACTOR_TEST_VICTIM_PORT:-8884}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
unique_key=actor-id-test-unique-key
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
body() {
  curl -s "$1"
}
# The id the namespace must derive from a name: the key is SHA-256 of the unique key, the first
# half HMAC(key, name) and the second half HMAC(key, first half), 16 bytes each.
expected_id() {
  python3 -I -c '
import hashlib, hmac, sys
key = hashlib.sha256(sys.argv[1].encode()).digest()
name = sys.argv[2].encode() if len(sys.argv) < 4 else (sys.argv[2] * (int(sys.argv[3]) // len(sys.argv[2]) + 1))[:int(sys.argv[3])].encode()
base = hmac.new(key, name, hashlib.sha256).digest()[:16]
mac = hmac.new(key, base, hashlib.sha256).digest()[:16]
print((base + mac).hex())
' "$@"
}
is_hex_id() {
  [ "${#1}" = 64 ] && [ -z "${1//[0-9a-f]/}" ]
}

WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 \
  "$workerd" serve "$here/actor-id-test.capnp" \
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

line=$(grep -o "WORKERD_GK_TEST_PROBE: actor-id key.*" "$log" | head -n1)
[ -n "$line" ] || fail "the server did not log where the actor-id key lives"
key=$(echo "$line" | grep -o '0x[0-9a-f]*' | head -n1)
inhost=$(echo "$line" | grep -o 'contains([a-z]*) = [a-z]*' | sed 's/.*= //')
echo "1. actor-id key at $key, in host memory: $inhost"
[ "$inhost" = true ] || fail "the actor-id key is not in host memory: $line"

id1=$(body "$victim/mint"); id2=$(body "$victim/mint"); id3=$(body "$victim/mint")
echo "2. newUniqueId() x3 from inside turns: $id1 $id2 $id3"
is_hex_id "$id1" && is_hex_id "$id2" && is_hex_id "$id3" || fail "a minted id is not 64 hex digits"
[ "$id1" != "$id2" ] && [ "$id2" != "$id3" ] && [ "$id1" != "$id3" ] || fail "minted ids repeat"

c1=$(body "$victim/do?id=$id1"); c2=$(body "$victim/do?id=$id1"); c3=$(body "$victim/do?id=$id2")
echo "3. Durable Object by minted id: $id1 -> $c1, again -> $c2; $id2 -> $c3"
[ "$c1" = 1 ] && [ "$c2" = 2 ] || fail "the object addressed by the minted id did not count 1 then 2"
[ "$c3" = 1 ] || fail "a second minted id did not reach a different object"

n1=$(body "$victim/name?n=alice"); n2=$(body "$victim/name?n=alice"); want=$(expected_id "$unique_key" alice)
echo "4. idFromName(\"alice\"): $n1, again $n2; derived from the unique key: $want"
[ "$n1" = "$n2" ] || fail "idFromName is not stable"
[ "$n1" = "$want" ] || fail "idFromName does not derive with the configured key"
nl=$(body "$victim/long-name?n=abcdefghij&len=10000"); wantl=$(expected_id "$unique_key" abcdefghij 10000)
echo "5. idFromName(10000-byte name): $nl; derived: $wantl"
[ "$nl" = "$wantl" ] || fail "idFromName of a long name does not derive with the configured key"
d1=$(body "$victim/do-name?n=alice"); d2=$(body "$victim/do-name?n=alice")
echo "6. Durable Object by name: alice -> $d1, again -> $d2"
[ "$d1" = 1 ] && [ "$d2" = 2 ] || fail "the object addressed by name did not count 1 then 2"

f=$(body "$victim/forged")
echo "7. idFromString(forged id) -> $f"
case "$f" in rejected:*) ;; *) fail "a forged id was accepted" ;; esac

s=$(status "$victim/__gk_probe_actor_key")
echo "8. GET $victim/__gk_probe_actor_key -> $s (the in-turn read of the key must fail the request)"
[ "$s" != 200 ] || fail "the request that read the key succeeded"
kill -0 "$pid" 2>/dev/null || fail "workerd died on the guest fault"
grep "reading host memory from inside the JS turn" "$log" | grep -q "actor-id-key" ||
  fail "the key probe did not run"
faultline=$(grep -o "isolate condemned.*id = victim;.*JS turn faulted inside the guest.*" "$log" | head -n1)
[ -n "$faultline" ] || fail "no guest fault condemned the victim isolate"
faultaddr=$(echo "$faultline" | grep -o 'gk_fault_addr() = [0-9]*' | sed 's/.*= //')
echo "9. guest fault at $(printf '0x%x' "$faultaddr") (key at $key)"
[ "$faultaddr" = "$((key))" ] || fail "the fault was not at the key's address"

s=$(status "$victim/")
echo "10. GET $victim/           -> $s (condemned isolate must not run again)"
[ "$s" != 200 ] || fail "the faulted isolate served a request"
s=$(status "$hello/")
echo "11. GET $hello/            -> $s (other isolate must still serve)"
[ "$s" = 200 ] || fail "the other isolate stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died after the guest fault"

echo "PASS: the actor-id key is in host memory; ids minted from inside turns are distinct, address working Durable Objects, and derive from the configured key, for short and long names; a forged id is refused; a ring-3 read of the key faulted at the address read, failed one request and condemned one isolate; process kept serving"
