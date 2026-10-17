#!/usr/bin/env bash
# Drives the allocation census's phase experiment (see memory-classes.md).
#
#   census-run.sh <census-workerd> <rounds> <outdir> [gk]
#
# `census-workerd` is a binary built with --//src/workerd/util:alloc_census=True
# (util/alloc-census.h has the full build line); `gk` turns the guest kernel on.
# Writes four dumps into <outdir>: p0 after startup with no request served, p1
# after `rounds` requests to the first worker, p2 after the same number to the
# second, and p3 after a mixed round. The two workers run the same module, so a
# site that grew between p0 and p1 and not between p1 and p2 is state the first
# isolate established for the whole process.
#
# Neither isolate exists at p0: a worker's isolate is created by its first
# request, so creating it, running its module and compiling its wasm all fall
# inside that worker's own phase, which is where the comparison needs them.
set -u
bin=$1; rounds=$2; out=$3; gk=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
# The wasm module the workers instantiate, which the config embeds.
[ -f "$here/census-types.wasm" ] || python3 "$here/census-types.py" "$here/census-types.wasm"
pa=$((8800 + RANDOM % 100)); pb=$((pa + 100))
vars=(WD_ALLOC_CENSUS_OUT="$out/current.txt")
[ -n "$gk" ] && vars+=(WORKERD_EXPERIMENTAL_GUEST_KERNEL=1)
env "${vars[@]}" "$bin" serve --experimental "$here/census-workload.capnp" \
  --socket-addr "wa=127.0.0.1:$pa" --socket-addr "wb=127.0.0.1:$pb" \
  > "$out/server.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT

die() { echo "$1"; grep -v "^stack:" "$out/server.log" | tail -20; exit 1; }

# A plain TCP connect, which must not run a worker.
listening() {
  for _ in $(seq 1 600); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || die "workerd exited during startup"
    sleep 0.2
  done
  die "nothing listening on $1"
}
snap() {
  kill -0 "$pid" 2>/dev/null || die "workerd is gone before dump $1"
  kill -USR2 "$pid"
  for _ in $(seq 1 200); do [ -s "$out/current.txt" ] && break; sleep 0.1; done
  sleep 1
  cp "$out/current.txt" "$out/$1.txt" || die "no dump for $1"
  echo "$1: $(grep -c '^S ' "$out/$1.txt") sites, $(grep '^# ' "$out/$1.txt" | tr '\n' ' ')"
}
drive() {
  local port=$1 n=$2 ok=0
  for _ in $(seq 1 "$n"); do
    [ "$(curl -s -o /dev/null -w %{http_code} --max-time 300 "http://127.0.0.1:$port/")" = 200 ] \
      && ok=$((ok + 1))
  done
  echo "$ok/$n"
}

listening "$pa"
listening "$pb"
snap p0
echo "phase 1 (wa): $(drive "$pa" "$rounds")"
snap p1
echo "phase 2 (wb): $(drive "$pb" "$rounds")"
snap p2
echo "phase 3 (both): $(drive "$pa" 5) $(drive "$pb" 5)"
snap p3
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
echo done
