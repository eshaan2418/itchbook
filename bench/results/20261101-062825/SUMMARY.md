# Replay benchmark summary (20261101-062825)

| config | runs | messages | book Mmsg/s (median) | min-max | CV % | ns/msg | end-to-end Mmsg/s | allocs after warm-up | peak RSS MB |
|---|---|---|---|---|---|---|---|---|---|
| opt | 5 | 368,366,634 | 21.1 | 21.0-21.2 | 0.4 | 47.45 | 12.7 | 4100 | 315 |
| opt_binary | 3 | 368,366,634 | 16.8 | 16.8-16.9 | 0.4 | 59.46 | 11.0 | 4100 | 315 |
| opt_hybrid | 3 | 368,366,634 | 20.1 | 19.9-20.2 | 0.8 | 49.68 | 12.3 | 4100 | 315 |
| opt_latency | 5 | 368,366,634 | 14.9 | 13.6-15.7 | 5.3 | 67.24 | 10.1 | 4100 | 315 |
| opt_noprefetch | 5 | 368,366,634 | 15.1 | 14.4-15.4 | 3.0 | 66.01 | 10.3 | 4100 | 315 |
| opt_prefetch1 | 5 | 368,366,634 | 19.3 | 19.2-19.6 | 0.8 | 51.73 | 12.0 | 4100 | 315 |
| opt_publish | 3 | 368,366,634 | 15.7 | 15.4-15.7 | 1.0 | 63.51 | 10.5 | 4100 | 319 |
| reference | 5 | 368,366,634 | 3.8 | 3.5-4.1 | 5.3 | 262.55 | 3.4 | 440688711 | 317 |
| reference_latency | 1 | 368,366,634 | 4.0 | 4.0-4.0 | 0.0 | 247.18 | 3.6 | 440688711 | 317 |

## Per-message latency (ns; median across runs, [min-max])

| config | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |
|---|---|---|---|---|---|---|---|
| opt_latency | 42 [42-42] | 125 [125-125] | 250 [209-292] | 417 [375-542] | 959 [834-13888] | 1280238 [297134-1336858] | 65 [61-71] |
| reference_latency | 167 | 500 | 916 | 1250 | 4416 | 119213202 | 245 |

Timer: reported 1.00 ns/tick; observed step median 42.0 ns (min 17.0 ns); read overhead 8.7 ns. Two reads bracket each message, so per-message values include ~one read of overhead.

## BBO hand-off to strategy thread (SPSC ring; ns, median across runs)

| config | BBO events | stalls | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| opt_publish | 174,890,436 | 0 | 125 | 166 | 250 | 11328 | 37472 | 293824 |
