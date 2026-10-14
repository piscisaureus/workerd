#!/bin/bash
# locks.sh <workerd-binary> <N>: count v8::Locker::Initialize calls while N
# requests are served to each worker, and how many of them re-lock the
# isolate the same thread locked last.
set -u; WD=$1; N=$2; cd "$(dirname "$0")"
"$WD" serve config.capnp --experimental >/dev/null 2>locks.err & pid=$!
for i in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:18080/ && break; sleep 0.2; done
for w in hello:18080 fetcher:18081 cpu:18082; do
  name=${w%%:*}; port=${w##*:}
  python3 client.py $port 500 >/dev/null
  sudo bpftrace -e "
    uprobe:$WD:_ZN2v86Locker10InitializeEPNS_7IsolateE {
      @locks = count();
      if (@last[tid] == arg1) { @same = count(); }
      @last[tid] = arg1;
      @isolates[arg1] = count();
    }
    END { clear(@last); }" > bpf-$name.txt 2>&1 & bpid=$!
  sleep 2
  python3 client.py $port $N > /dev/null
  sudo kill -INT $bpid; wait $bpid
  echo "== $name (N=$N)"; grep -E "^@(locks|same)" bpf-$name.txt; grep -c "^@isolates" bpf-$name.txt
done
kill -TERM $pid; wait $pid
