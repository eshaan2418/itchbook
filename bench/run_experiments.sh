#!/usr/bin/env bash
# Optimisation experiments referenced in docs/PERF_LOG.md (full-day replay, 3 runs each),
# plus Google Benchmark micro-benchmarks. Run on an idle machine.
set -euo pipefail
ITCH="${ITCH:?set ITCH}"; BUILD="${BUILD:-build}"; RUNS="${RUNS:-3}"
OUT="bench/results/experiments-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"
TO="gtimeout 2700"; command -v gtimeout >/dev/null || TO="timeout 2700"
run() { local name="$1"; shift; for i in $(seq 1 "$RUNS"); do echo "[$name] $i/$RUNS" >&2
  $TO "$BUILD/itchbook" replay "$ITCH" "$@" > "$OUT/$name.run$i.json"; done; }
"$BUILD/itchbook" timer > "$OUT/timer.json"
for d in 8 32 64; do run "opt_prefetch2_d$d" --impl opt --prefetch "$d" --prefetch-stages 2; done
run opt_prefetch3_d16 --impl opt --prefetch 16 --prefetch-stages 3
run opt_prefetch3_d32 --impl opt --prefetch 32 --prefetch-stages 3
run opt_side_reserve256  --impl opt --prefetch 16 --prefetch-stages 2 --side-reserve 256
run opt_side_reserve1024 --impl opt --prefetch 16 --prefetch-stages 2 --side-reserve 1024
"$BUILD/itchbook-bench" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
  --benchmark_out="$OUT/micro.json" --benchmark_out_format=json > "$OUT/micro.txt"
python3 bench/summarize.py "$OUT" > "$OUT/SUMMARY.md"
echo "$OUT"
