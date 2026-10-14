#!/bin/bash
# run.sh <workerd-binary> <N>
# For each worker: start workerd under strace -c, warm up, send N requests on
# one keep-alive connection, then stop it and print the syscall summary.
set -u
WD=$1; N=$2; cd "$(dirname "$0")"
for w in hello:18080 fetcher:18081 cpu:18082; do
  name=${w%%:*}; port=${w##*:}
  strace -f -c -o strace-$name-warm.txt "$WD" serve config.capnp --experimental >/dev/null 2>workerd-$name.err &
  pid=$!
  for i in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:$port/ && break; sleep 0.2; done
  python3 client.py $port 200 >/dev/null   # warm-up
  pkill -TERM -P $pid; wait $pid
  strace -f -c -o strace-$name.txt "$WD" serve config.capnp --experimental >/dev/null 2>workerd-$name.err &
  pid=$!
  for i in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:$port/ && break; sleep 0.2; done
  python3 client.py $port 200 >/dev/null   # warm-up again so JIT and caches are hot
  python3 client.py $port $N > client-$name.txt
  pkill -TERM -P $pid; wait $pid
  echo "== $name: $(cat client-$name.txt)"
done
