#!/usr/bin/env bash
# Checks what the Rust memory cache (MEMORY_CACHE_V2, api/memory-cache-v2.c++ over
# src/rust/memory-cache) does under per-isolate heap arenas, and how it is selected.
#
# The crate forbids unsafe code and allocates through the process allocator, so the cache it
# keeps for a cache id is allocated by whichever isolate's lock was held when the id was first
# bound -- `Namespace::bind()` runs host-side under the binder's isolate lock, from the globals
# phase of that worker's isolate -- and under heap arenas that puts it in the first binder's
# arena, which no other isolate's ring 3 can address. This script runs four workers: "owner"
# and "sharer", configured with one cache id and listed in that order, and "solo-a" and
# "solo-b", whose bindings have no id and so get a cache each.
#
# On a build with per-isolate heap arenas (the guest kernel plus
# --//src/workerd/server:allocator=jemalloc) the sharer's first cache read -- taken before the
# owner has touched the cache, so what it faults on is fixed by bind order and not by use --
# faults inside the guest at the cache the owner's arena holds, which fails that request and
# condemns only the sharer's isolate. The owner keeps serving the same id, the two workers with
# a cache of their own keep serving, and workerd stays up. Without heap arenas (the guest
# kernel on the plain binary) the same configuration shares the cache across the two isolates,
# which is the behaviour the id is documented to have.
#
# How V2 is selected is part of what this checks, because the obvious route does not work.
# `autogates` in the config, which memory-cache-v2-arena-test.capnp carries, is installed by
# `Autogate::initAutogate()` while the config is parsed, and the config is parsed by the
# CliMain the `serve` command builds -- whose constructor has already built the Server and with
# it the single MemoryCacheProvider (server.c++, `memoryCacheProvider(...)` in the initializer
# list), which is where the gate is read and `namespaceV2` is or is not created. So by the time
# the gate list exists the decision has been made, and the config entry, like `--all-autogates`
# on the command line, selects nothing. What does select V2 is the presence of
# WORKERD_ALL_AUTOGATES in the environment -- any value, including 0, since
# `Autogate::isEnabled()` falls back to `getenv(...) != nullptr` while no gate list is
# installed, which at provider construction time is still the case.
#
# So the script runs the same configuration twice against the same binary: once with
# WORKERD_ALL_AUTOGATES in the environment, which gets the Rust cache, and once with nothing
# but the config's own `autogates` entry, which gets the C++ one. Both arms must reach the same
# verdict. On a heap-arena build the two arms must also fault at different places in the arena,
# which is what shows from outside that the first arm faulted on the Rust cache and the second
# on the C++ one: the implementations have no behaviour a request can tell apart, by design, so
# the object the sharer's turn could not address is the witness. An arena is a run of 512 GiB
# page-table slots and takes whichever run is free (gk_arena.c), so which slot the first
# binder's arena lands in varies from run to run and the raw fault addresses move by whole
# slots; the address modulo the slot size does not, and is what the arms are compared on.
#
# Usage: memory-cache-v2-arena-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_MEMORY_CACHE_V2_TEST_PORT overrides the first of the four ports (8820).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
base_port=${GK_MEMORY_CACHE_V2_TEST_PORT:-8820}
owner_port=$base_port
sharer_port=$((base_port + 1))
solo_a_port=$((base_port + 2))
solo_b_port=$((base_port + 3))
owner=http://127.0.0.1:$owner_port
sharer=http://127.0.0.1:$sharer_port
solo_a=http://127.0.0.1:$solo_a_port
solo_b=http://127.0.0.1:$solo_b_port
log=$(mktemp)
pid=
arm_addr=

# Stops workerd without waiting for it indefinitely: it drains in-flight requests on SIGTERM, so
# a process that still holds a hung request -- what a failed probe can leave behind -- is killed
# after a moment rather than waited for.
stop_workerd() {
  [ -n "$pid" ] || return 0
  kill "$pid" 2>/dev/null
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  pid=
}
cleanup() {
  stop_workerd
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- workerd log ---"
  cat "$log"
  exit 1
}
# Prints the HTTP status of a GET, or "no-response" if the connection failed or timed out.
status() {
  curl -s --max-time 10 -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
# Prints the body of a GET, or "no-response" if the connection failed or timed out.
body() {
  b=$(curl -s --max-time 10 "$1") || b=no-response
  echo "${b%$'\n'}"
}
# The address of the first guest fault the log reports, or the empty string if it reports none.
fault_addr() {
  grep -o 'gk_fault_addr() = [0-9]*' "$log" | head -1 | grep -o '[0-9]*$'
}

trap cleanup EXIT

# Starts workerd on the four sockets and waits for all of them. $1 selects how V2 is asked for:
# "env" puts WORKERD_ALL_AUTOGATES in the environment, "config" leaves the config's own
# `autogates` entry as the only route.
start_workerd() {  # $1 how
  : >"$log"
  if [ "$1" = env ]; then
    WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_ALL_AUTOGATES=1 \
      "$workerd" serve --experimental "$here/memory-cache-v2-arena-test.capnp" \
      --socket-addr "owner=127.0.0.1:$owner_port" \
      --socket-addr "sharer=127.0.0.1:$sharer_port" \
      --socket-addr "soloA=127.0.0.1:$solo_a_port" \
      --socket-addr "soloB=127.0.0.1:$solo_b_port" >"$log" 2>&1 &
  else
    WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
      "$workerd" serve --experimental "$here/memory-cache-v2-arena-test.capnp" \
      --socket-addr "owner=127.0.0.1:$owner_port" \
      --socket-addr "sharer=127.0.0.1:$sharer_port" \
      --socket-addr "soloA=127.0.0.1:$solo_a_port" \
      --socket-addr "soloB=127.0.0.1:$solo_b_port" >"$log" 2>&1 &
  fi
  pid=$!
  for _ in $(seq 1 100); do
    [ "$(status "$owner/")" = 200 ] && [ "$(status "$sharer/")" = 200 ] &&
      [ "$(status "$solo_a/")" = 200 ] && [ "$(status "$solo_b/")" = 200 ] && break
    kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
    sleep 0.1
  done
}

# Runs one arm's steps. $1 names it for the output, $2 is the key the arm reads (a fresh one per
# arm, so neither arm inherits the other's entry), and $3 is how V2 is asked for.
run_arm() {  # $1 name  $2 key  $3 how
  local name=$1 key=$2 how=$3
  start_workerd "$how"
  echo "== $name: workerd pid $pid, cache id gk-v2-shared on $owner (owner) and $sharer (sharer)"

  # The sharer asks first, so the cache it reaches is the one bind order gave the owner rather
  # than one the owner's own read created.
  b=$(body "$sharer/read?key=$key")
  echo "1. GET $sharer/read?key=$key   -> $b"
  if [ "$arenas" = yes ]; then
    [ "$b" != "sharer read: $key from sharer" ] ||
      fail "$name: the sharer read the shared cache that is in the owner's arena"
    [ "$b" != no-response ] || fail "$name: the sharer's read neither answered nor failed"
    grep -q "JS turn faulted inside the guest" "$log" ||
      fail "$name: the sharer's read failed without a guest fault"
    [ -n "$(fault_addr)" ] || fail "$name: the guest fault reported no address"
    echo "   the sharer's turn faulted at $(fault_addr) and its isolate was condemned"
  else
    [ "$b" = "sharer read: $key from sharer" ] ||
      fail "$name: the sharer did not run its own fallback: $b"
  fi

  b=$(body "$owner/read?key=$key")
  echo "2. GET $owner/read?key=$key   -> $b"
  if [ "$arenas" = yes ]; then
    [ "$b" = "owner read: $key from owner" ] ||
      fail "$name: the owner stopped serving the cache in its own arena: $b"
  else
    [ "$b" = "owner read: $key from sharer" ] ||
      fail "$name: the two workers do not share the cache: $b"
  fi

  # A worker whose cache binding has no id gets an instance of its own, allocated in its own
  # arena, which the arena does not get in the way of.
  b=$(body "$solo_a/read?key=$key")
  echo "3. GET $solo_a/read?key=$key   -> $b (no cache id: an instance of its own)"
  [ "$b" = "solo-a read: $key from solo-a" ] || fail "$name: solo-a's own cache did not serve: $b"
  b=$(body "$solo_b/read?key=$key")
  echo "   GET $solo_b/read?key=$key   -> $b"
  [ "$b" = "solo-b read: $key from solo-b" ] || fail "$name: solo-b's own cache did not serve: $b"

  s=$(status "$sharer/")
  echo "4. GET $sharer/                -> $s"
  if [ "$arenas" = yes ]; then
    [ "$s" != 200 ] || fail "$name: the condemned isolate served a request"
  else
    [ "$s" = 200 ] || fail "$name: the sharer stopped serving: $s"
  fi

  n=$(grep -c "isolate condemned" "$log")
  echo "5. isolates condemned: $n"
  if [ "$arenas" = yes ]; then
    [ "$n" = 1 ] || fail "$name: expected exactly one condemnation, got $n"
    grep "isolate condemned" "$log" | grep -q "id = sharer" ||
      fail "$name: the condemned isolate is not the sharer's"
  else
    [ "$n" = 0 ] || fail "$name: an isolate was condemned without heap arenas"
  fi
  kill -0 "$pid" 2>/dev/null || fail "$name: workerd died"
  echo "   workerd pid $pid is still alive"

  arm_addr=$(fault_addr)
  stop_workerd
}

# Which build this is, from what it says about the allocator at startup. The arms' outcomes
# depend on it, so it is read from the binary rather than passed in.
arenas=unknown
start_workerd env
if grep -q "per-isolate heap arenas enabled" "$log"; then
  arenas=yes
  echo "build: the guest kernel with per-isolate heap arenas"
elif grep -q "built without per-isolate heap arenas" "$log"; then
  arenas=no
  echo "build: the guest kernel without per-isolate heap arenas (the plain allocator)"
else
  fail "the binary did not say whether it has per-isolate heap arenas; is it a guest-kernel build?"
fi
stop_workerd

run_arm "arm V2 (WORKERD_ALL_AUTOGATES set: the Rust cache)" v2key env
v2_addr=$arm_addr
run_arm "arm V1 (the config's autogates entry alone: the C++ cache)" v1key config
v1_addr=$arm_addr

if [ "$arenas" = yes ]; then
  [ -n "$v2_addr" ] && [ -n "$v1_addr" ] || fail "an arm faulted without reporting an address"
  # The offset within the arena's 512 GiB slot, which the slot the arena landed in does not
  # move (see the head of this file).
  slot=549755813888
  v2_off=$((v2_addr % slot))
  v1_off=$((v1_addr % slot))
  echo "6. where each arm faulted, within the arena's slot: V2 $v2_off / V1 $v1_off"
  [ "$v2_off" != "$v1_off" ] ||
    fail "both arms faulted at the same place in the arena: the environment variable selected no\
 other implementation, so either the gate no longer reaches the provider or the two caches now\
 share the object the sharer faults on"
  echo "   they differ, so the first arm faulted on the Rust cache and the second on the C++ one,"
  echo "   and the config's autogates entry selected neither"
  echo "PASS: under per-isolate heap arenas the Rust memory cache takes the same verdict as the"
  echo "C++ one: a cache id shared by two workers lives in the first binder's arena, every other"
  echo "sharer's first cache read faults inside the guest and condemns only that isolate, a cache"
  echo "binding with no id is served from the worker's own arena, and workerd keeps serving. The"
  echo "config's autogates entry does not select the Rust cache; WORKERD_ALL_AUTOGATES in the"
  echo "environment does."
else
  echo "6. no arm faulted, as neither can without heap arenas; which cache each arm ran is"
  echo "   therefore not witnessed here, the two having no behaviour a request can tell apart"
  echo "PASS: without per-isolate heap arenas two workers configured with one memory cache id"
  echo "share the cache across their isolates, a binding with no id gets an instance of its own,"
  echo "and no isolate is condemned."
fi
