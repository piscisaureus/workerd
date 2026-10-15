#!/usr/bin/env bash
# Runs every measurement behind findings.md, in the order the document reports
# them, one at a time so that nothing overlaps with a measurement.
#
# Usage: run-all.sh [results-dir]
#   WORKERD must point at an optimized workerd built from this tree
#   (bazel build -c opt //src/workerd/server:workerd). Needs /dev/kvm for the
#   guest-kernel arrangements and passwordless sudo for the KSM and perf steps;
#   the steps that need sudo are skipped, with a note, where it is not granted.
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/results}
mkdir -p "$out"
export WORKERD=${WORKERD:?set WORKERD to an optimized workerd binary}
export GKM_RUN_DIR=${GKM_RUN_DIR:-${TMPDIR:-/tmp}/gkmeasure-run}
mkdir -p "$GKM_RUN_DIR"
cc -O2 -pthread -o "$here/httpbench" "$here/httpbench.c"
cc -O2 -shared -fPIC -o "$here/ksmshim.so" "$here/ksmshim.c"

have_sudo() { sudo -n true 2>/dev/null; }
ksm() { have_sudo && sudo sh -c "echo $1 > /sys/kernel/mm/ksm/$2"; }

counts="1 2 4 8 16 32 64 128 256"

echo "== 1. marginal memory =="
for m in shared procs; do
  python3 "$here/memory.py" $m "$out/mem-$m-opt.json" $counts
done
# The guest kernel runs out of page-table slots before 256; see the ceiling.
python3 "$here/memory.py" gk "$out/mem-gk-opt.json" 1 2 4 8 16 32 64 96 128
python3 "$here/ceiling.py" gk 128 256 | tee "$out/ceiling-gk.log"

if have_sudo; then
  ksm 2000 pages_to_scan; ksm 20 sleep_millisecs; ksm 1 run
  python3 "$here/memksm.py" "$out/mem-procs-ksm-opt.json" 1 2 4 8 16 32 64 128
  python3 "$here/memdiverse.py" "$out/mem-diverse.json" 1 64
  ksm 2 run; sleep 2; ksm 0 run
else
  echo "skipping the deduplicated arrangements: they need sudo for KSM"
fi

echo "== 2. cross-tenant dispatch =="
for rep in 1 2; do
  for m in shared gk procs procs-uds; do
    GKM_DUR=20 python3 "$here/dispatch.py" $m "$out/disp-$m-opt-$rep.json"
  done
done

echo "== cost and rate =="
for m in shared procs gk; do
  GKM_DUR=15 python3 "$here/cputime.py" $m "$out/cpu2-$m-opt.json" 3
done
for m in shared gk procs; do
  GKM_DUR=15 python3 "$here/throughput.py" $m 16 "$out/thr2-$m-16.json"
done
python3 "$here/diag.py" "$out/diag.json"

echo "== 3. cold start =="
for m in shared gk; do
  python3 "$here/coldstart.py" loader $m "$out/cs-loader-$m-opt.json" 40
  python3 "$here/coldstart.py" spawn $m "$out/cs-spawn-$m-opt.json" 30
done

echo "== the published hello-world figure, on a build without optimization =="
if [ -n "${WORKERD_FASTBUILD:-}" ]; then
  for m in shared gk; do
    WORKERD=$WORKERD_FASTBUILD GKM_DUR=10 python3 "$here/dispatch.py" $m \
      "$out/disp-$m-fastbuild.json"
  done
fi

echo "== the library's own microbenchmarks =="
make -C "$here/.." run > "$out/gk-test.log" 2>&1 || echo "gk_test exited nonzero"
grep -o "[0-9.]* ns per [a-z ]*" "$out/gk-test.log" | sort -u

python3 "$here/report.py" "$out"
