#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), that nothing a turn registers through
# __cxa_atexit leaves process-wide state in that turn's heap arena.
#
# Every function-local static with a non-trivial destructor registers its destructor through
# __cxa_atexit the first time it is initialized, and glibc keeps those registrations in a chain
# of 32-entry blocks whose first block is static. __new_exitfn walks the chain from its head on
# every registration and allocates another block once the head block is full. A registration
# made inside a turn therefore puts a block of process-wide state in one isolate's arena, and
# every later registration -- from any isolate, for any static -- faults *reading* that block on
# its way down the chain. Scoping the registering code is no remedy: what faults is the read.
#
# ICU reaches __cxa_atexit from SimpleDateFormat::subFormat, so formatting a date is what drives
# it here. "plain" serves first and holds the first arena, so the blocks any turn allocates are
# not the formatters' own; "fmt" and "fmt2" then format in turn, each registering and each
# walking whatever the other left. The script fails if any request fails, if any isolate is
# condemned, if the guest kernel reports an unmappable fault, or if the process aborts.
#
# The abort is the second thing this covers. A turn that faults is abandoned with its frames
# unwound by nobody, so the guard of the static it was initializing stays owned by the thread
# (see the leak list on runInGuest in jsg/guest-run.h); the next turn on that thread to reach
# the same static dies on libc++abi's recursive-initialization check and takes the process with
# it. That is a cost of the fault, not a cause, so it has no fix of its own here -- but it is
# what makes a single misplaced block fatal rather than one failed request, and the script says
# so when it sees it.
#
# Negative build (never shipped), failing this script:
#   --copt=-DWORKERD_GK_NEG_ATEXIT_SHARED_HEAP  registrations are left in whichever arena the
#       registering thread allocates from, so the first turn to register puts a block of the
#       chain in its own isolate's arena and the next isolate's registration faults on it
#
# Usage: atexit-arena-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_ATEXIT_TEST_PORT overrides the first port (8868); the two formatters take the next two.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
plain_port=${GK_ATEXIT_TEST_PORT:-8868}
fmt_port=$((plain_port + 1))
fmt2_port=$((plain_port + 2))
rounds=${GK_ATEXIT_TEST_ROUNDS:-10}
log=$(mktemp)

cleanup() {
  kill "$pid" 2>/dev/null
  # A condemned isolate can leave the server not exiting on SIGTERM, which is a cost of the
  # fault and not something to wait out here.
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- faults, condemnations and aborts ---"
  grep -E "unmappable fault|isolate condemned|recursive initialization" "$log" | head -n 6
  echo "--- workerd log, without the guest kernel's per-turn chatter ---"
  grep -vE "^\[gk\] vcpu [0-9]+(:? (syscall|.*: (wall|root switch)))" "$log" | tail -n 40
  exit 1
}
body() {
  curl -s --max-time 120 "$1" || echo no-response
}

GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
  "$workerd" serve --experimental "$here/atexit-arena-test.capnp" \
  --socket-addr "plain=127.0.0.1:$plain_port" --socket-addr "fmt=127.0.0.1:$fmt_port" \
  --socket-addr "fmt2=127.0.0.1:$fmt2_port" >"$log" 2>&1 &
pid=$!
trap cleanup EXIT

for _ in $(seq 1 200); do
  [ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] && break
  kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
  sleep 0.1
done
[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] || fail "the plain worker never served"
grep -q "guest-kernel isolation enabled" "$log" ||
  fail "the server is not running with guest-kernel isolation; the test would prove nothing"

if ! grep -q "per-isolate heap arenas enabled" "$log"; then
  echo "SKIP: this workerd has no per-isolate heap arenas, so a registration a turn makes is in"
  echo "      the one shared heap whatever happens. Build with"
  echo "      --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
echo "workerd pid $pid, with guest-kernel isolation and per-isolate heap arenas;"
echo "the plain worker holds the first arena"

# The plain worker again, so that whatever its own turns register is registered before either
# formatter's isolate exists.
for _ in $(seq 1 "$rounds"); do
  [ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] || fail "the plain worker stopped serving"
done

# The formatter joins its results with "|", so a response without one is an error page.
expected=$(body "http://127.0.0.1:$fmt_port/")
case $expected in
  *"|"*) ;;
  *) fail "GET the fmt worker -> ${expected:-no-response}" ;;
esac
for round in $(seq 1 "$rounds"); do
  for port in "$fmt_port" "$fmt2_port"; do
    got=$(body "http://127.0.0.1:$port/")
    [ "$got" = "$expected" ] ||
      fail "round $round: GET http://127.0.0.1:$port/ -> ${got:-no-response}, wanted $expected"
  done
done
echo "1. two isolates formatted the same date $((rounds * 2 + 1)) times and agreed on all of it"

faults=$(grep -c "unmappable fault" "$log")
echo "2. unmappable faults reported by the guest kernel: $faults"
[ "$faults" = 0 ] || fail "$(grep -m1 "unmappable fault" "$log")"

condemned=$(grep -c "isolate condemned" "$log")
echo "3. isolates condemned: $condemned"
[ "$condemned" = 0 ] || fail "$(grep -m1 "isolate condemned" "$log")"

recursive=$(grep -c "recursive initialization" "$log")
echo "4. aborts on a guard a condemned turn left owned: $recursive"
[ "$recursive" = 0 ] || fail "the process aborted on a static's guard"

[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] || fail "the plain worker stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
echo "5. all three workers still serve"

echo "PASS: $((rounds * 2 + 1)) date formattings across two isolates, each registering through __cxa_atexit behind an isolate that had already registered of its own, left no block of glibc's exit-function chain in any isolate's heap arena: no turn faulted, no isolate was condemned, and the process is still serving"
