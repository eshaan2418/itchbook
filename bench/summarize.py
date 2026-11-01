#!/usr/bin/env python3
"""Aggregate raw replay JSON runs into a Markdown summary (median and min/max across runs)."""
import glob, json, os, re, statistics, sys
from collections import defaultdict

d = sys.argv[1]
groups = defaultdict(list)
for f in sorted(glob.glob(os.path.join(d, "*.run*.json"))):
    name = re.sub(r"\.run\d+\.json$", "", os.path.basename(f))
    with open(f) as fh:
        groups[name].append(json.load(fh))

def spread(vals):
    m = statistics.median(vals)
    return m, min(vals), max(vals), (statistics.pstdev(vals) / statistics.mean(vals) * 100 if len(vals) > 1 else 0.0)

print(f"# Replay benchmark summary ({os.path.basename(d)})\n")
print("| config | runs | messages | book Mmsg/s (median) | min-max | CV % | ns/msg | end-to-end Mmsg/s | allocs after warm-up | peak RSS MB |")
print("|---|---|---|---|---|---|---|---|---|---|")
for name, runs in groups.items():
    rates = [r["book_msgs_per_sec"] / 1e6 for r in runs]
    m, lo, hi, cv = spread(rates)
    e2e = statistics.median(r["end_to_end_msgs_per_sec"] / 1e6 for r in runs)
    allocs = max(r["heap_allocs_after_first_chunk"] for r in runs)
    rss = max(r["peak_rss_bytes"] for r in runs) / 2**20
    print(f"| {name} | {len(runs)} | {runs[0]['messages']:,} | {m:.1f} | {lo:.1f}-{hi:.1f} | {cv:.1f} | {1e3/m:.2f} | {e2e:.1f} | {allocs} | {rss:.0f} |")

lat = {k: v for k, v in groups.items() if "latency_ns" in v[0]}
if lat:
    print("\n## Per-message latency (ns; median across runs, [min-max])\n")
    print("| config | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |")
    print("|---|---|---|---|---|---|---|---|")
    for name, runs in lat.items():
        cells = []
        for q in ["p50", "p90", "p99", "p99.9", "p99.99", "max", "mean"]:
            vals = [r["latency_ns"][q] for r in runs]
            m = statistics.median(vals)
            cells.append(f"{m:.0f} [{min(vals):.0f}-{max(vals):.0f}]" if len(vals) > 1 else f"{m:.0f}")
        print(f"| {name} | " + " | ".join(cells) + " |")
    tfile = os.path.join(d, "timer.json")
    if os.path.exists(tfile):
        t = json.load(open(tfile))
        print(f"\nTimer: reported {t['reported_ns_per_tick']:.2f} ns/tick; observed step median "
              f"{t['median_step_ns']:.1f} ns (min {t['min_step_ns']:.1f} ns); read overhead {t['read_overhead_ns']:.1f} ns. "
              "Two reads bracket each message, so per-message values include ~one read of overhead.")

pub = {k: v for k, v in groups.items() if "handoff_latency_ns" in v[0]}
if pub:
    print("\n## BBO hand-off to strategy thread (SPSC ring; ns, median across runs)\n")
    print("| config | BBO events | stalls | p50 | p90 | p99 | p99.9 | p99.99 | max |")
    print("|---|---|---|---|---|---|---|---|---|")
    for name, runs in pub.items():
        cells = [f"{statistics.median(r['handoff_latency_ns'][q] for r in runs):.0f}" for q in ["p50", "p90", "p99", "p99.9", "p99.99", "max"]]
        print(f"| {name} | {runs[0]['bbo_consumed']:,} | {max(r['producer_stalls'] for r in runs)} | " + " | ".join(cells) + " |")
