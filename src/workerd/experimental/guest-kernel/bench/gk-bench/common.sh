# Shared settings and helpers for the drivers in this directory. Source, don't
# run.
#
# Core pinning matters more than anything else here: the guest-kernel overhead
# is almost pure CPU, so leaving the server and the load generator to compete
# for the same cores measures scheduling, not the runtime. SERVER_CPUS and
# CLIENT_CPUS must be disjoint sets on the same NUMA node, and SERVER_CPUS
# should be whole physical cores (no two SMT siblings of one core).

WORKERD=${WORKERD:?set WORKERD to the path of the workerd binary to measure}
PORT=${PORT:-9300}
SERVER_CPUS=${SERVER_CPUS:-40-43}
CLIENT_CPUS=${CLIENT_CPUS:-20-23,44-47}
DUR=${DUR:-5s}
REPS=${REPS:-3}
OUT=${OUT:-./results}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORKLOADS=${WORKLOADS:-$HERE/workloads}
SRV_PID=

# start_server <off|on> <config.capnp>
start_server() {
  local mode=$1 cfg=$2
  local -a env=(env)
  if [ "$mode" = on ]; then
    env+=(WORKERD_EXPERIMENTAL_GUEST_KERNEL=1)
  else
    env+=(-u WORKERD_EXPERIMENTAL_GUEST_KERNEL)
  fi
  "${env[@]}" taskset -c "$SERVER_CPUS" \
    "$WORKERD" serve "$cfg" --socket-addr "http=127.0.0.1:$PORT" \
    >"$OUT/server.log" 2>&1 &
  SRV_PID=$!
  local i
  for i in $(seq 100); do
    if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/"; then return 0; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
      echo "server died on startup; see $OUT/server.log" >&2
      tail -5 "$OUT/server.log" >&2
      return 1
    fi
    sleep 0.1
  done
  echo "server did not answer on port $PORT" >&2
  return 1
}

stop_server() {
  [ -n "$SRV_PID" ] || return 0
  kill -TERM "$SRV_PID" 2>/dev/null
  wait "$SRV_PID" 2>/dev/null
  SRV_PID=
}

# Warm the isolate: the first requests into a fresh isolate are parsing and
# JIT, and under the guest kernel they also fault in the arena's pages.
warm() {
  taskset -c "$CLIENT_CPUS" oha -z 2s -c 2 --no-tui "http://127.0.0.1:$PORT/" \
    >/dev/null 2>&1 || true
}

require_tools() {
  local t
  for t in oha curl taskset python3 "$@"; do
    command -v "$t" >/dev/null || { echo "missing required tool: $t" >&2; exit 1; }
  done
}
