#!/usr/bin/env bash
# Checks, against a built workerd, that a JS turn rewriting the directory descriptor SQLite's
# native VFS uses for its path syscalls (currentVfsRoot in src/workerd/util/sqlite.c++, a
# thread-local the turn can write) does not redirect where a Durable Object's database files
# land. The VFS makes its root the guest kernel's turn directory around each native call that
# names a file (through the VFS root hostcall, whose handler takes the descriptor from host
# memory), and with path confinement on, a path syscall whose dirfd is not that directory is
# refused. With WORKERD_GK_TEST_PROBE set, two request paths on the "victim" worker
# (disk-sqlite-test.js, see maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++)
# drive the rewrite:
#
# /__gk_vfs_rewrite arms it: from then on every native VFS call runs with currentVfsRoot
# rewritten to the descriptor of a directory this script created, elsewhere on disk, as a turn
# with a write primitive would rewrite it. /__gk_vfs_restore lifts it. In between, a request to
# an object not yet opened makes SQLite open a new database: that open must fail, no file may
# appear in the chosen directory, and the server's log must show the guest kernel refusing the
# open under the turn directory. An object opened before the rewrite, and one opened after the
# restore, must work and keep their files in the data directory, and no isolate may be
# condemned.
#
# The second half runs the same rewrite with the confinement switched off (WORKERD_GK_TEST_
# UNCONFINED, a test-only switch): the database then lands in the chosen directory, which is
# what the confinement prevents.
#
# Usage: vfs-root-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_VFS_TEST_HELLO_PORT / GK_VFS_TEST_VICTIM_PORT override the ports (8845 / 8844).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_VFS_TEST_HELLO_PORT:-8845}
victim_port=${GK_VFS_TEST_VICTIM_PORT:-8844}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
log=$(mktemp)
data=$(mktemp -d)
elsewhere=$(mktemp -d)
pid=

cleanup() {
  [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
  rm -rf "$log" "$data" "$elsewhere"
}
fail() {
  echo "FAIL: $*"
  echo "--- workerd log (without the guest kernel's syscall trace) ---"
  grep -v '^\[gk\] vcpu [0-9]* syscall' "$log"
  exit 1
}
status() {
  curl -s -o /dev/null -w '%{http_code}' "$1" || echo no-response
}
body() {
  curl -s "$1"
}
# Starts workerd with the given extra environment; GK_DEBUG makes the guest kernel log each
# forwarded syscall, including the ones the turn directory refused.
start() {
  env "$@" WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 WORKERD_GK_TEST_PROBE=1 GK_DEBUG=1 \
    WORKERD_GK_TEST_VFS_DIR="$elsewhere" \
    "$workerd" serve "$here/disk-sqlite-test.capnp" --directory-path "disk=$data" \
    --socket-addr "hello=127.0.0.1:$hello_port" --socket-addr "victim=127.0.0.1:$victim_port" \
    >>"$log" 2>&1 &
  pid=$!
  for _ in $(seq 1 100); do
    [ "$(status "$hello/")" = 200 ] && [ "$(status "$victim/")" = 200 ] && break
    kill -0 "$pid" 2>/dev/null || fail "workerd exited during startup"
    sleep 0.1
  done
}
stop() {
  kill "$pid"
  wait "$pid" 2>/dev/null
  pid=
}
# Database files (relative names) in a directory.
dbfiles() {
  (cd "$1" && find . -name '*.sqlite*' | sort | sed 's|^\./||' | tr '\n' ' ')
}
# The number of distinct object databases in the data directory.
objects() {
  find "$data" -name '*.sqlite' ! -name 'metadata.sqlite' | wc -l
}

trap cleanup EXIT
start
echo "workerd pid $pid serving hello on $hello, victim on $victim"
echo "   data directory $data; a directory of the turn's choosing: $elsewhere"

p=$(body "$victim/put?k=a&v=1&o=one")
echo "1. /put a=1 on object one -> $p; $(objects) object database(s) in the data directory"
[ "$p" = ok ] && [ "$(objects)" = 1 ] || fail "the first object did not store its row on disk"

s=$(status "$victim/__gk_vfs_rewrite")
echo "2. GET /__gk_vfs_rewrite -> $s (arms the rewrite of currentVfsRoot for every native VFS call)"
sleep 0.3
grep -q "currentVfsRoot rewritten to a directory of the turn's choosing" "$log" ||
  fail "the rewrite was not armed"

s=$(status "$victim/put?k=b&v=2&o=two")
echo "3. /put b=2 on object two (a new database, opened with the rewritten descriptor) -> $s (must fail)"
[ "$s" != 200 ] || fail "SQLite opened a database with the rewritten descriptor"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
e=$(dbfiles "$elsewhere")
echo "4. files in the chosen directory: '${e:-none}'; object databases in the data directory: $(objects)"
[ -z "$e" ] || fail "the database landed in the directory of the turn's choosing"
[ "$(objects)" = 1 ] || fail "the database landed in the data directory despite the rewrite"
refused=$(grep -c "syscall 257(.*) under the turn directory -> -1$" "$log")
echo "5. openat calls the guest kernel refused under the turn directory (EPERM): $refused"
[ "$refused" -ge 1 ] || fail "the guest kernel did not refuse the open with the rewritten descriptor"
grep -q "isolate condemned" "$log" && fail "an isolate was condemned"

s=$(status "$victim/__gk_vfs_restore")
sleep 0.3
g=$(body "$victim/get?k=a&o=one"); p=$(body "$victim/put?k=c&v=3&o=three"); g3=$(body "$victim/get?k=c&o=three")
echo "6. GET /__gk_vfs_restore -> $s; then /get a on object one -> $g; object three: /put c=3 -> $p, /get c -> $g3"
[ "$g" = 1 ] && [ "$p" = ok ] && [ "$g3" = 3 ] || fail "the objects did not work once the rewrite was lifted"
echo "7. object databases in the data directory: $(objects); in the chosen directory: '$(dbfiles "$elsewhere")'"
[ "$(objects)" = 2 ] && [ -z "$(dbfiles "$elsewhere")" ] || fail "the files did not land in the data directory"
s=$(status "$hello/")
echo "8. GET $hello/ -> $s (other worker must still serve)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
grep -q "isolate condemned" "$log" && fail "an isolate was condemned"
stop

echo "--- the same rewrite with the confinement off (WORKERD_GK_TEST_UNCONFINED) ---"
rm -rf "${data:?}"/* "${elsewhere:?}"/*
start WORKERD_GK_TEST_UNCONFINED=1
grep -q "WORKERD_GK_TEST_UNCONFINED" "$log" || fail "the confinement was not switched off"
p=$(body "$victim/put?k=a&v=1&o=one")
status "$victim/__gk_vfs_rewrite" >/dev/null; sleep 0.3
s=$(status "$victim/put?k=b&v=2&o=two")
e=$(dbfiles "$elsewhere")
echo "9. /put on object one -> $p; after /__gk_vfs_rewrite, /put on object two -> $s; files in the chosen directory: '${e:-none}'"
echo "$e" | grep -q '\.sqlite' || fail "with the confinement off, the rewrite did not redirect the database (so the check above was not testing the confinement)"
stop

echo "PASS: a turn's rewrite of currentVfsRoot left SQLite's files in the data directory and the open refused by the guest kernel; with the confinement off the same rewrite put the database in the directory of the turn's choosing"
