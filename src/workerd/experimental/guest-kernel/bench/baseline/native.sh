#!/bin/bash
# native.sh <workerd-binary> <N>: request rate per worker without tracing.
set -u; WD=$1; N=$2; cd "$(dirname "$0")"
"$WD" serve config.capnp --experimental >/dev/null 2>native.err & pid=$!
for i in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:18080/ && break; sleep 0.2; done
for w in hello:18080 fetcher:18081 cpu:18082; do
  name=${w%%:*}; port=${w##*:}
  python3 client.py $port 500 >/dev/null
  echo "== $name: $(python3 client.py $port $N)"
done
kill -TERM $pid; wait $pid
