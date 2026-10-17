#!/usr/bin/env bash
# Checks, against a built workerd, that a SQLite-backed Durable Object stored on local disk works
# with its JS turns at guest ring 3: SQLite's native VFS runs inside the turn, so its file
# syscalls are forwarded through the guest kernel's syscall filter. The "victim" worker
# (disk-sqlite-test.js) serves a Store object whose database lives in a directory this script
# creates:
#
# /put and /get write a row and read it back, across requests and across a restart of the
# process, and the database files must be in the directory.
# /throw makes a statement fail inside SQLite and catches the error in JS; /throw-uncaught lets
# the same error leave the turn uncaught. Then an object whose database file this script has
# filled with garbage is requested: SQLite refuses the file inside the turn, and the resulting
# error is not one JS can see, so the runtime logs it inside the turn with its stack trace --
# which in KJ's full stack-trace mode would run the symbolizer, a child process the filter can
# never allow (pipe2, fork and execve are denied), and a denied syscall on that path must not
# recurse. All three requests must complete without a guest fault, the other objects must keep
# serving afterwards, no isolate may be condemned, and the process must keep serving its other
# worker.
#
# Usage: disk-sqlite-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm, curl and
#   python3. GK_DISK_TEST_HELLO_PORT / GK_DISK_TEST_VICTIM_PORT override the ports (8855 / 8854).
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
hello_port=${GK_DISK_TEST_HELLO_PORT:-8855}
victim_port=${GK_DISK_TEST_VICTIM_PORT:-8854}
hello=http://127.0.0.1:$hello_port
victim=http://127.0.0.1:$victim_port
log=$(mktemp)
data=$(mktemp -d)
pid=

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
  [ -n "$pid" ] && stop_workerd "$pid"
  rm -rf "$log" "$data"
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
start() {
  WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
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
  stop_workerd "$pid"
  pid=
}
alive() {
  kill -0 "$pid" 2>/dev/null || fail "workerd died: $*"
}
not_condemned() {
  grep -q "isolate condemned" "$log" && fail "an isolate was condemned: $*"
  grep -q "JS turn faulted inside the guest" "$log" && fail "a guest fault was logged: $*"
  return 0
}
# The id the namespace derives from a name, which names the object's database file: the key is
# SHA-256 of the unique key, the first half HMAC(key, name) and the second HMAC(key, first half).
id_of() {
  python3 -I -c '
import hashlib, hmac, sys
key = hashlib.sha256(sys.argv[1].encode()).digest()
base = hmac.new(key, sys.argv[2].encode(), hashlib.sha256).digest()[:16]
mac = hmac.new(key, base, hashlib.sha256).digest()[:16]
print((base + mac).hex())
' disk-sqlite-test-unique-key "$1"
}

trap cleanup EXIT
start
echo "workerd pid $pid serving hello on $hello, victim on $victim; database directory $data"

p=$(body "$victim/put?k=a&v=1"); g=$(body "$victim/get?k=a")
echo "1. /put a=1 -> $p; /get a -> $g"
[ "$p" = ok ] && [ "$g" = 1 ] || fail "the row was not written and read back"
files=$(cd "$data" && find . -name '*.sqlite*' | sort | tr '\n' ' ')
echo "2. database files on disk: $files"
echo "$files" | grep -q '\.sqlite' || fail "no database file was created in the directory"

t=$(body "$victim/throw")
echo "3. /throw (error raised inside SQLite, caught in JS) -> $t"
case "$t" in caught:*no_such_table*) ;; *) fail "the SQLite error was not caught in JS" ;; esac
alive "after /throw"; not_condemned "after /throw"

s=$(status "$victim/throw-uncaught")
echo "4. /throw-uncaught (error leaves the turn uncaught) -> $s (request must fail, nothing else)"
[ "$s" = 500 ] || fail "the uncaught SQLite error did not fail the request with 500"
alive "after /throw-uncaught"; not_condemned "after /throw-uncaught"

garbage="$data/disk-sqlite-test-unique-key/$(id_of garbage).sqlite"
printf 'not a database: %0160d' 0 > "$garbage"
s=$(status "$victim/get?k=a&o=garbage")
echo "5. /get on an object whose database file is garbage (SQLite refuses it inside the turn; the error is logged there with its stack) -> $s (request must fail, nothing else)"
[ "$s" = 500 ] || fail "the request on the garbage database did not fail with 500"
alive "after the garbage database"; not_condemned "after the garbage database"

g=$(body "$victim/get?k=a"); p=$(body "$victim/put?k=b&v=2&o=two"); g2=$(body "$victim/get?k=b&o=two")
echo "6. /get a -> $g (first object, after the errors); second object: /put b=2 -> $p, /get b -> $g2"
[ "$g" = 1 ] && [ "$p" = ok ] && [ "$g2" = 2 ] || fail "the objects did not keep working after the errors"

stop
start
g=$(body "$victim/get?k=a"); g2=$(body "$victim/get?k=b&o=two")
echo "7. after a restart of workerd (pid $pid): /get a -> $g, /get b (second object) -> $g2"
[ "$g" = 1 ] && [ "$g2" = 2 ] || fail "the rows did not survive a restart"

s=$(status "$hello/")
echo "8. GET $hello/ -> $s (other worker must still serve)"
[ "$s" = 200 ] || fail "the other worker stopped serving"
alive "at the end"; not_condemned "at the end"

echo "PASS: a disk-backed SQLite Durable Object under the guest kernel writes and reads back across requests and a restart, and errors raised inside its SQLite turns -- caught, uncaught, and one the runtime logs inside the turn with its stack -- fail only their own request"
