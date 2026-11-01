#!/usr/bin/env bash
# Interleaved A/B runs within one session (cross-session drift on a laptop is ~5%, larger than
# the effects being compared, so variants are alternated run by run).
set -euo pipefail
ITCH="${ITCH:?set ITCH}"; BUILD="${BUILD:-build}"; ROUNDS="${ROUNDS:-3}"
OUT="bench/results/ab-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"
TO="gtimeout 2700"; command -v gtimeout >/dev/null || TO="timeout 2700"
"$BUILD/itchbook" timer > "$OUT/timer.json"
for r in $(seq 1 "$ROUNDS"); do
  for v in "d16:--prefetch 16 --prefetch-stages 2" "d8:--prefetch 8 --prefetch-stages 2" \
           "d8_reserve256:--prefetch 8 --prefetch-stages 2 --side-reserve 256" \
           "d8_hybrid:--impl opt-hybrid --prefetch 8 --prefetch-stages 2"; do
    name="${v%%:*}"; args="${v#*:}"
    echo "[$name] round $r" >&2
    # shellcheck disable=SC2086
    $TO "$BUILD/itchbook" replay "$ITCH" $args > "$OUT/$name.run$r.json"
  done
done
"$BUILD/itchbook-bench" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
  --benchmark_out="$OUT/micro.json" --benchmark_out_format=json > "$OUT/micro.txt"
python3 bench/summarize.py "$OUT" > "$OUT/SUMMARY.md"
echo "$OUT"
