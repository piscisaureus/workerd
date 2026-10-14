#!/bin/bash
# cputime.sh <workerd-binary> <N>: CPU microseconds workerd spends per request.
set -u; WD=$1; N=$2; cd "$(dirname "$0")"
"$WD" serve config.capnp --experimental >/dev/null 2>cputime.err & pid=$!
for i in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:18080/ && break; sleep 0.2; done
ticks() { awk '{print $14+$15}' /proc/$pid/stat; }   # utime+stime, all threads
hz=$(getconf CLK_TCK)
for w in hello:18080 fetcher:18081 cpu:18082; do
  name=${w%%:*}; port=${w##*:}
  python3 client.py $port 500 >/dev/null
  a=$(ticks); python3 client.py $port $N >/dev/null; b=$(ticks)
  echo "== $name: $(( (b-a) * 1000000 / hz / N )) us CPU per request (user+sys), $(( (b-a) * 1000000 / hz )) us total"
done
kill -TERM $pid; wait $pid
