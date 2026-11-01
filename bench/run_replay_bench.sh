#!/usr/bin/env bash
# Full-day replay benchmark suite. Run on an otherwise idle machine; nothing else is built or run
# concurrently. Writes raw JSON for every run to bench/results/<stamp>/.
set -euo pipefail
ITCH="${ITCH:?set ITCH to the NASDAQ sample path}"
BUILD="${BUILD:-build}"
RUNS="${RUNS:-5}"
BIN="$BUILD/itchbook"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="bench/results/$STAMP"
TO="gtimeout 2700"; command -v gtimeout >/dev/null || TO="timeout 2700"
mkdir -p "$OUT"

machine() {
  {
    echo "date: $(date -u +%FT%TZ)"
    echo "git: $(git rev-parse --short HEAD 2>/dev/null || echo none)"
    if [[ "$(uname)" == Darwin ]]; then
      echo "cpu: $(sysctl -n machdep.cpu.brand_string) ncpu=$(sysctl -n hw.ncpu) pcores=$(sysctl -n hw.perflevel0.physicalcpu) mem=$(sysctl -n hw.memsize) line=$(sysctl -n hw.cachelinesize)"
      echo "os: $(sw_vers -productVersion)"
      echo "thermal: $(pmset -g therm 2>&1 | tr '\n' ' ')"
      echo "memory_pressure: $(memory_pressure | tail -1)"
    else
      echo "cpu: $(lscpu | grep 'Model name' | sed 's/.*: *//') ncpu=$(nproc)"
    fi
    echo "compiler: $(c++ --version | head -1)"
  } > "$OUT/machine.txt"
}

run() {  # name, args...
  local name="$1"; shift
  for i in $(seq 1 "$RUNS"); do
    echo "[$name] run $i/$RUNS" >&2
    $TO "$BIN" replay "$ITCH" "$@" > "$OUT/${name}.run$i.json"
    if [[ "$(uname)" == Darwin ]]; then pmset -g therm | grep -i "warning level" >> "$OUT/thermal.log" || true; fi
  done
}

machine
BEST=(--prefetch 16 --prefetch-stages 2)
run opt_noprefetch            --impl opt
run opt_prefetch1             --impl opt --prefetch 16
run opt                       --impl opt "${BEST[@]}"
RUNS=3 run opt_binary         --impl opt-binary "${BEST[@]}"
RUNS=3 run opt_hybrid         --impl opt-hybrid "${BEST[@]}"
run opt_latency               --impl opt "${BEST[@]}" --latency --hist-out "$OUT/opt_latency.hist.csv"
RUNS=3 run opt_publish        --impl opt "${BEST[@]}" --publish
RUNS="${REF_RUNS:-5}" run reference --impl ref
RUNS=1 run reference_latency  --impl ref --latency --hist-out "$OUT/reference_latency.hist.csv"
machine; mv "$OUT/machine.txt" "$OUT/machine_end.txt"; machine
python3 bench/summarize.py "$OUT" | tee "$OUT/SUMMARY.md"
