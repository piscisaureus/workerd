#!/bin/bash
# How much resident memory each additional idle isolate costs, with
# guest-kernel isolation off and on.
#
#   WORKERD=/path/to/workerd ./iso-ramp.sh
#
# For each N, starts one workerd holding N distinct trivial workers, sends one
# request to each so every isolate is actually created and warm, and records
# the process's resident size. iso-fit.py then fits RSS = base + per_isolate*N
# over the series; the per-isolate slope is the number that decides how many
# isolates a host can hold, and the intercept is the process baseline.
#
# Under guest-kernel isolation there is also a hard ceiling: each arena takes a
# whole 512 GiB PML4 slot, so the process runs out of address-space slots at
# roughly 185 isolates, and the failure is fatal to the process rather than to
# the isolate ("failed to create an isolate arena"). Keep the largest N below
# that; with isolation off there is no such limit.
set -euo pipefail
cd "$(dirname "$0")"
. ./common.sh
require_tools
mkdir -p "$OUT"

BASE_PORT=${BASE_PORT:-9400}
NS=${NS:-"1 10 25 50 100 150"}
REPEATS=${REPEATS:-2}

csv="$OUT/iso-ramp.csv"
echo "mode,rep,n,vmrss_kb,rss_anon_kb,anon_huge_kb" > "$csv"

for mode in off on; do
  for rep in $(seq "$REPEATS"); do
    for n in $NS; do
      cfg="$OUT/iso-$n.capnp"
      python3 ./iso-gen.py "$n" "$BASE_PORT" > "$cfg"

      if [ "$mode" = on ]; then envp=(env WORKERD_EXPERIMENTAL_GUEST_KERNEL=1)
      else envp=(env -u WORKERD_EXPERIMENTAL_GUEST_KERNEL); fi
      "${envp[@]}" taskset -c "$SERVER_CPUS" "$WORKERD" serve "$cfg" \
        >"$OUT/iso-server.log" 2>&1 &
      pid=$!

      ok=1
      for i in $(seq "$n"); do
        port=$((BASE_PORT + i - 1))
        up=0
        for try in $(seq 100); do
          if curl -fsS -o /dev/null --max-time 5 "http://127.0.0.1:$port/"; then
            up=1; break
          fi
          kill -0 "$pid" 2>/dev/null || break
          sleep 0.1
        done
        if [ "$up" = 0 ]; then ok=0; break; fi
      done

      if [ "$ok" = 1 ] && kill -0 "$pid" 2>/dev/null; then
        vmrss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")
        anon=$(awk '/^Rss:/{print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null || echo 0)
        huge=$(awk '/^AnonHugePages:/{print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null || echo 0)
        echo "$mode,$rep,$n,$vmrss,$anon,$huge" | tee -a "$csv"
      else
        echo "$mode rep$rep n=$n: failed to come up; see $OUT/iso-server.log" >&2
        tail -3 "$OUT/iso-server.log" >&2
      fi

      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    done
  done
done

echo
python3 ./iso-fit.py "$csv"
