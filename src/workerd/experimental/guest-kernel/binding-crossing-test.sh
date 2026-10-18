#!/usr/bin/env bash
# Checks, against a workerd built with per-isolate heap arenas (the guest kernel plus jemalloc:
# `--//src/workerd/server:allocator=jemalloc`), the crossings of an in-process binding that carry
# no body: what a request brings beside the HTTP interface, what a call that fails hands back,
# what a body given up on mid-stream tears down, and what reaches a worker by a route other than
# a service binding's fetch -- a Durable Object namespace, the Cache API, a worker loader, a tail
# worker.
#
# body-arena-test.sh builds every subrequest from a URL string and every callee of it succeeds,
# so none of this crosses there. A request's `cf` blob travels as
# IoChannelFactory::SubrequestMetadata rather than through kj's HttpClient adapter, whose clones
# of the url and headers are made in the shared heap; it is serialized by the caller's turn and
# read by the callee's (CfProperty in api/global-scope.c++), and a request forwarded as a proxy
# forwards it always carries one, since the listener makes a `{"clientIp": ...}` blob for every
# connection. A callee that throws hands the caller a kj::Exception made under the callee's lock,
# which the caller's turn reads in jsg::exceptionToJs(); a body that errors mid-stream reaches
# the caller the same way, through the callee's failed request; and a JS RPC call that rejects
# drops the caller's exported capabilities inside the callee's turn, where the caller's
# IoContext::PendingEvent destructor schedules its hang-abort task, which the caller's next turn
# cancels. WorkerEntrypoint (io/worker-entrypoint.c++) copies the blob and the exception into
# the shared heap, and ~PendingEvent (io/io-context.c++) allocates its task there. An RPC
# exception needs no copy: capnp forks the call's promise, and each branch of a fork gets a clone
# of the exception made on the event loop.
#
# A response body the caller drops mid-stream -- reader.cancel(), an AbortController, an
# AbortSignal.timeout() -- is tied by kj's HttpClient adapter to the callee's request, so the
# drop cancels that request and destroys the callee's suspended request coroutine, in the
# caller's turn; ReadRelay (io/worker-interface.c++) hands the body to the event loop instead. A
# body that comes back on the WebSocket path, when the callee answers an upgrade request with a
# plain response, gets the relay every other body gets. A loaded worker's entrypoint with props
# reads a copy of them its loader's turn made (server.c++, EntrypointService::startRequest()),
# now on the shared heap, as is the self-token factory the same turn makes for every request,
# which the callee's turn reads when it calls ctx.restore(). A Durable Object whose class is in another worker is looked up in that
# worker's class table and made, with its container and the namespace's table of containers,
# from the turn of whichever worker addresses it (io/worker.c++, server.c++, the
# WORKERD_GK_NEG_ACTOR_HEAP sites); all of that is on the shared heap. A buffered tail worker
# reads a trace the traced worker's turns filled; server.c++ copies it through its serialization
# on the event loop before handing it over (placeTraceForTailWorkers()).
#
# The cdn pair of bench/bench.capnp is served as is: a front that forwards the incoming Request
# to an origin over a binding and returns `new Response(r.body, r)`. The script fails if any
# route answers wrongly, if any isolate is condemned, or if the guest kernel reports an
# unmappable fault.
#
# A connect() tunnel through a binding is a two-way pipe, each direction of which crosses as a
# body does; IoStreamRelay (io/worker-interface.c++) relays the reads at both ends and drops the
# caller's end on the event loop, since kj's adapter attaches the callee's whole connect() call
# to it and its ~TwoWayPipeEnd aborts the pipe the callee writes. A WebSocket through a binding
# is a kj WebSocket pipe whose directions copy each message in whichever of send() and receive()
# arrives second, into an array the second operation makes; WebSocketRelay relays the receives,
# sends under a shared-heap scope so that a copy made in the sender's turn is readable by the
# receiver's, and aborts and drops its end on the loop. The tunnel routes go through back's
# Tunnel entrypoint, the WebSocket routes through back's fetch handler and through the Durable
# Object, accepted and hibernatable; the last two WebSocket steps hand the callee's WebSocket
# to this script's own client (binding-crossing-ws-client.py), so that front's end is pumped by
# the server on the event loop, once closed cleanly and once dropped. /ws-leave bursts its sends
# and then closes and returns without awaiting the close reply, so the caller's turn destroys an
# event the loop armed before the turn began: the event-disarm shape README.md's event-loop
# hazard section describes, which the run queue's links being the loop's own is what makes safe.
# It is the one step here that depends on that, and the one that aborts the whole process when it
# does not hold, so it runs six times.
#
# Negative builds (never shipped), each failing this script at the step named:
#   --copt=-DWORKERD_GK_NEG_CF_BLOB_HEAP      the cf blob stays in the caller's arena; step 1,
#       the benchmark's forwarded request, faults the origin's turn
#   --copt=-DWORKERD_GK_NEG_EXCEPTION_HEAP    a callee's exception stays in its arena; the
#       first throwing callee faults the caller's turn, as does the body that errors
#   --copt=-DWORKERD_GK_NEG_BODY_DROP_DEFER   a body dropped mid-stream is dropped in the
#       dropping turn; /cancel-mid faults the caller's turn
#   --copt=-DWORKERD_GK_NEG_BODY_RELAY        no body is relayed; the first body read faults,
#       and so does /ws-reject
#   --copt=-DWORKERD_GK_NEG_PROPS_HEAP        a loaded entrypoint's props are copied in the
#       loader's arena; /props faults the loaded worker's turn on its second request
#   --copt=-DWORKERD_GK_NEG_SELF_TOKEN_HEAP   a request's self-token factory stays in the
#       caller's arena; /restore faults the callee's turn, /do-restore the object's
#   --copt=-DWORKERD_GK_NEG_ACTOR_HEAP        the actor class table, the namespace's table and
#       the actor are made where they are made; /do faults the addressing turn
#   --copt=-DWORKERD_GK_NEG_TRACE_HEAP        a trace is handed over as is; the buffered tail
#       worker's turn faults on the first traced request
#   --copt=-DWORKERD_GK_NEG_TUNNEL_RELAY      no tunnel end is relayed; /tunnel-echo faults the
#       callee's turn on its first read
#   --copt=-DWORKERD_GK_NEG_TUNNEL_DROP_DEFER the caller's end of a tunnel is dropped in the
#       dropping turn; /tunnel-close-parked faults the caller's turn
#   --copt=-DWORKERD_GK_NEG_WS_RELAY          no WebSocket end is relayed; /ws-echo faults the
#       caller's turn on its first send
#   --copt=-DWORKERD_GK_NEG_WS_SEND_HEAP      a send's copy is made in the sender's arena;
#       /ws-echo faults the callee's turn on its first message
#   --copt=-DWORKERD_GK_NEG_WS_DROP_DEFER     a WebSocket end is dropped and aborted in the
#       dropping turn; /ws-echo faults the caller's turn on its close, where it is the last
#       holder of the pipe, and /ws-abort on the callee's request the end carries
#
# Usage: binding-crossing-test.sh [path/to/workerd]
#   defaults to bazel-bin/src/workerd/server/workerd in the repo root. Needs /dev/kvm and curl.
#   GK_BINDING_CROSSING_PORT overrides front's port (8882); cdn takes the port below it, the
#   plain worker the one below that, and the two tail workers the two above it.
#   GK_BINDING_CROSSING_ONLY names the step groups to run (cdn cf fail cancel cache props
#   restore actor tail rpc-stream tunnel ws), for checking that a negative build fails at the
#   step named for it.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
workerd=${1:-$root/bazel-bin/src/workerd/server/workerd}
port=${GK_BINDING_CROSSING_PORT:-8882}
cdn_port=$((port - 1))
plain_port=$((port - 2))
tail_port=$((port + 1))
tail_stream_port=$((port + 2))
front=http://127.0.0.1:$port
cdn=http://127.0.0.1:$cdn_port
only=${GK_BINDING_CROSSING_ONLY:-cdn cf fail cancel cache props restore actor tail rpc-stream tunnel ws}
log=$(mktemp)

# workerd drains in-flight requests on SIGTERM, so the stop is bounded: a few seconds, then
# SIGKILL. A probe that hangs instead of failing is worse than no probe.
stop_workerd() {
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -KILL "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}
cleanup() {
  stop_workerd
  rm -f "$log"
}
fail() {
  echo "FAIL: $*"
  echo "--- faults and condemnations ---"
  grep -E "unmappable fault|isolate condemned" "$log" | head -n 6
  echo "--- workerd log, without the guest kernel's per-turn chatter ---"
  grep -vE "^\[gk\] vcpu [0-9]+(:? (syscall|.*: (wall|root switch)))" "$log" | tail -n 40
  exit 1
}
body() {
  curl -s --max-time 30 "$1" || echo no-response
}
selected() {
  case " $only " in *" $1 "*) return 0;; esac
  return 1
}

GK_DEBUG=1 WORKERD_EXPERIMENTAL_GUEST_KERNEL=1 \
  "$workerd" serve --experimental "$here/binding-crossing-test.capnp" \
  --socket-addr "plain=127.0.0.1:$plain_port" --socket-addr "cdn=127.0.0.1:$cdn_port" \
  --socket-addr "front=127.0.0.1:$port" --socket-addr "tail=127.0.0.1:$tail_port" \
  --socket-addr "tail-stream=127.0.0.1:$tail_stream_port" >"$log" 2>&1 &
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
  echo "SKIP: this workerd has no per-isolate heap arenas, so nothing a binding carries has an"
  echo "      arena wall to cross. Build with --//src/workerd/server:allocator=jemalloc to run it."
  exit 0
fi
[ "$(body "$front/")" = "front" ] || fail "front never served"
echo "workerd pid $pid serving front on $front and the benchmark's cdn pair on $cdn, with"
echo "guest-kernel isolation and per-isolate heap arenas; the plain worker holds the first arena"
echo "step groups: $only"

step=0
faulted() {
  if grep -q "unmappable fault" "$log"; then
    fail "$1 faulted: $(grep -m1 "unmappable fault" "$log")"
  fi
}
check() {  # route expected-line
  step=$((step + 1))
  got=$(body "$front/$1")
  echo "$step. GET $front/$1 -> ${got:-no-response}"
  [ "$got" = "$2" ] || fail "GET /$1 answered '${got:-no-response}', wanted '$2'"
  faulted "GET /$1"
}
check_prefix() {  # route expected-prefix
  step=$((step + 1))
  got=$(body "$front/$1")
  echo "$step. GET $front/$1 -> ${got:-no-response}"
  case "$got" in "$2"*) ;; *) fail "GET /$1 answered '${got:-no-response}', wanted '$2...'";; esac
  faulted "GET /$1"
}

if selected cdn; then
  # The benchmark's cdn pair: the incoming request forwarded to the origin, whose response is
  # passed through as the init of a new one. 4096 x's, 200, and the origin's header.
  step=$((step + 1))
  headers=$(curl -s --max-time 30 -D - -o /dev/null "$cdn/" || echo no-response)
  got=$(body "$cdn/")
  echo "$step. GET $cdn/ -> $(echo "$headers" | head -n 1 | tr -d '\r'), ${#got} bytes"
  echo "$headers" | head -n 1 | grep -q " 200 " || fail "the cdn front did not answer 200: $headers"
  echo "$headers" | grep -qi "^cache-control: max-age=60" ||
    fail "the origin's cache-control header did not pass through: $headers"
  [ "${#got}" = 4096 ] && [ "$(echo -n "$got" | tr -d x | wc -c)" = 0 ] ||
    fail "the origin's body did not pass through intact (${#got} bytes)"
  faulted "GET $cdn/"
  echo "   the benchmark's forwarded request reached the origin and its response came back whole"
fi

if selected cf; then
  # The cf blob, from the listener's blob on a forwarded request and from a fetch's init.
  step=$((step + 1))
  got=$(body "$front/forward")
  echo "$step. GET $front/forward -> ${got:-no-response}"
  echo "$got" | grep -q '^forward status=200 cf={"clientIp":"127.0.0.1' ||
    fail "the listener's cf blob did not reach the callee: ${got:-no-response}"
  faulted "GET /forward"
  check "cf" 'cf status=200 cf={"colo":"XYZ","n":1}'
  echo "   a forwarded request's cf blob and a fetch init's reached the callee's handler"
fi

if selected fail; then
  # Callees that fail, over fetch, over a tunnel and over RPC, each twice: the second request
  # after a failure is the caller's next turn, which cancels the hang-abort task the first one
  # scheduled. The body that errors is the same exception, through the callee's failed request.
  check "throw" "throw caught=Error:boom"
  check "reject" "reject caught=TypeError:rejected"
  check "throw" "throw caught=Error:boom"
  check "nonresponse" \
    "nonresponse caught=TypeError:Incorrect type for Promise: the Promise did not resolve to 'Response'."
  check "body-error" "body-error caught=Error:mid"
  check_prefix "body-error-up" "body-error-up "
  check "connect-none" \
    "connect-none caught=Error:jsg.Error: proxy request failed, cannot connect to the specified address"
  check "connect-throw" "connect-throw caught=Error:Network connection lost."
  check "rpc-ok" "rpc-ok 42"
  check "rpc-throw" "rpc-throw caught=RangeError:rpc boom"
  check "rpc-reject" "rpc-reject caught=TypeError:rpc rejected"
  check "rpc-throw" "rpc-throw caught=RangeError:rpc boom"
  check "rpc-ok" "rpc-ok 42"
  echo "   a callee's thrown and rejected errors reached the caller over fetch, connect and RPC"
fi

if selected cancel; then
  # Bodies the caller gives up on mid-stream, a client that goes away mid-response, and a body
  # on the WebSocket path.
  check "cancel-mid" "cancel-mid first=1000"
  check "abort-mid" "abort-mid first=1000 then=Error:mid-abort"
  check "timeout-mid" "timeout-mid then=TimeoutError:The operation was aborted due to timeout"
  step=$((step + 1))
  got=$(curl -s --max-time 0.7 "$front/slow" | wc -c)
  echo "$step. GET $front/slow, client gone after 0.7 s -> $got bytes received"
  [ "$got" -gt 0 ] && [ "$got" -lt 5000 ] ||
    fail "the client should have left mid-response, but received $got bytes"
  sleep 2
  faulted "GET /slow with the client gone"
  check "ws-reject" "ws-reject status=400 body=no websocket here"
  echo "   a body cancelled, aborted, timed out and abandoned mid-stream tore nothing foreign down"
fi

if selected cache; then
  check "cache" "cache match=cached-body"
  echo "   the Cache API reached its worker over cacheApiOutbound"
fi

if selected props; then
  check "props" 'props {"greeting":"twice","n":2} {"greeting":"twice","n":2} {"greeting":"thrice","n":3}'
  check "loopback-props" "loopback-props got=hello via-back"
  echo "   props reached a loaded worker's entrypoint three times and a loopback stub through RPC"
fi

if selected restore; then
  # Callees that restore themselves: the self-token factory their request carries was made by
  # the caller's turn.
  check "restore" "restore restored pong:r"
  check "do-restore" "do-restore restored pong:d"
  echo "   a callee and a Durable Object each restored themselves through the caller's factory"
fi

if selected actor; then
  # The Durable Object in do-worker, addressed first from front's turn, then from back's.
  check "do" "do count=1 name=shared"
  check "do-via-back" "do-via-back back-do count=2 name=shared"
  check "do" "do count=3 name=shared"
  echo "   a Durable Object in a third worker served two workers' turns"
fi

if selected tail; then
  # The traced worker, reached through front's binding so that its event is the caller's; its
  # buffered and streaming tail workers must each have received the log line and the exception.
  check "traced-log" "traced-log status=200"
  check "traced-throw" "traced-throw caught=Error:traced boom"
  sleep 1
  step=$((step + 1))
  tail_got=$(body "http://127.0.0.1:$tail_port/")
  echo "$step. GET tail -> $(echo "$tail_got" | wc -l) trace(s)"
  echo "$tail_got" | grep -q '^{"url":"http://traced/log","logs":\[\["traced /log"\]\],"exceptions":\[\],"outcome":"ok"}$' ||
    fail "the buffered tail worker did not receive the logged request's trace: $tail_got"
  echo "$tail_got" | grep -q '^{"url":"http://traced/throw","logs":\[\["traced /throw"\]\],"exceptions":\["traced boom"\],"outcome":"exception"}$' ||
    fail "the buffered tail worker did not receive the throwing request's trace: $tail_got"
  faulted "the buffered tail worker"
  step=$((step + 1))
  stream_got=$(body "http://127.0.0.1:$tail_stream_port/")
  echo "$step. GET tail-stream -> $(echo "$stream_got" | wc -l) event(s)"
  echo "$stream_got" | grep -q '^{"onset":{"type":"fetch".*"url":"http://traced/log"' &&
    echo "$stream_got" | grep -q '"type":"log".*"message":\["traced /log"\]' &&
    echo "$stream_got" | grep -q '"type":"exception".*"message":"traced boom"' ||
    fail "the streaming tail worker did not receive the events: $stream_got"
  faulted "the streaming tail worker"
  echo "   both tail workers received what the traced worker's turns made"
fi

if selected rpc-stream; then
  check "rpc-stream" "rpc-stream n=70000"
  check "rpc-stream-ret" "rpc-stream-ret n=70000"
  echo "   streams crossed as an RPC argument and as an RPC result"
fi

if selected tunnel; then
  # Tunnels through back's Tunnel entrypoint: the caller writing first, the callee writing
  # first, two hundred round trips, a mebibyte each way across a half-close, a close with the
  # callee's write parked, and a callee that throws mid-stream.
  check "tunnel-echo" "tunnel-echo got=echo:ping rest=0"
  check "tunnel-greet" "tunnel-greet first=hello then=got:ping"
  check "tunnel-pingpong" "tunnel-pingpong n=200"
  check "tunnel-bulk" "tunnel-bulk up=1048576 down=1048576"
  check "tunnel-close-parked" "tunnel-close-parked first=a closed=ok"
  check "tunnel-throw-mid" "tunnel-throw-mid then=eof"
  echo "   a tunnel's bytes crossed in both orders, in bulk, across a half-close and a close"
fi

if selected ws; then
  # WebSockets through back's fetch handler and the Durable Object, and two handed to this
  # script's client through front.
  # The caller's close is answered as the plain build answers it: the callee's close reply
  # finds the connection gone.
  check "ws-echo" "ws-echo got=echo:one,echo:two,echo:three close=error:Uncaught Error: Network connection lost."
  check "ws-server-first" "ws-server-first first=hello then=echo:x"
  check "ws-many" "ws-many n=1000"
  # A burst of messages, then close() and return without awaiting the close reply, six times
  # over: the caller's turn destroys an event the loop armed before the turn began. Each attempt
  # is its own request, since what it exercises is the end of one.
  step=$((step + 1))
  for i in $(seq 1 6); do
    got=$(body "$front/ws-leave")
    [ "$got" = "ws-leave n=100" ] ||
      fail "GET /ws-leave answered '${got:-no-response}' on attempt $i, wanted 'ws-leave n=100'"
    kill -0 "$pid" 2>/dev/null || fail "workerd died on /ws-leave, attempt $i"
  done
  echo "$step. GET $front/ws-leave, six times -> ws-leave n=100"
  faulted "GET /ws-leave"
  check "ws-big" "ws-big text=1048581 bytes=1048576"
  check "ws-callee-close" "ws-callee-close first=echo:a close=close:4000:callee-close"
  check "ws-abort" "ws-abort first=echo:a then=error:Uncaught Error: ws-abort"
  check "ws-do" "ws-do got=do:hi,do:there"
  check "ws-do-hib" "ws-do-hib got=hib:hi,hib:there"
  step=$((step + 1))
  got=$(python3 -I "$here/binding-crossing-ws-client.py" "$port" /ws-proxy clean p1 p2 p3 2>&1 | tr '\n' ' ')
  echo "$step. WS $front/ws-proxy, closed cleanly -> $got"
  [ "$got" = "1:echo:p1 1:echo:p2 1:echo:p3 close:1000:done " ] ||
    fail "the proxied WebSocket did not echo and close: $got"
  faulted "WS /ws-proxy"
  step=$((step + 1))
  got=$(python3 -I "$here/binding-crossing-ws-client.py" "$port" /ws-proxy abrupt q1 q2 2>&1 | tr '\n' ' ')
  echo "$step. WS $front/ws-proxy, connection dropped -> $got"
  [ "$got" = "1:echo:q1 1:echo:q2 dropped " ] || fail "the proxied WebSocket did not echo: $got"
  sleep 1
  faulted "WS /ws-proxy with the client gone"
  echo "   WebSockets crossed in both orders, in bulk, through a Durable Object and out to a client"
fi

faults=$(grep -c "unmappable fault" "$log")
step=$((step + 1))
echo "$step. unmappable faults reported by the guest kernel: $faults"
[ "$faults" = 0 ] || fail "$(grep -m1 "unmappable fault" "$log")"

condemned=$(grep -c "isolate condemned" "$log")
step=$((step + 1))
echo "$step. isolates condemned: $condemned"
[ "$condemned" = 0 ] || fail "$(grep -m1 "isolate condemned" "$log")"

[ "$(body "http://127.0.0.1:$plain_port/")" = "plain" ] ||
  fail "the plain worker stopped serving"
[ "$(body "$front/")" = "front" ] || fail "front stopped serving"
[ "$(body "$cdn/" | wc -c)" = 4096 ] || fail "the cdn front stopped serving"
kill -0 "$pid" 2>/dev/null || fail "workerd died"
step=$((step + 1))
echo "$step. every worker still serves"

echo "PASS: a request's cf blob, a callee's errors, bodies given up on mid-stream, a loaded worker's props, a callee's self-token factory, a Durable Object two workers address, the Cache API, a traced worker's trace, tunnels and WebSockets all crossed between isolates with per-isolate heap arenas: no turn faulted, no isolate was condemned"
