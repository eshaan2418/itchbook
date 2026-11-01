# Replay benchmark summary (20261101-071441)

| config | runs | messages | book Mmsg/s (median) | min-max | CV % | ns/msg | end-to-end Mmsg/s | allocs after warm-up | peak RSS MB |
|---|---|---|---|---|---|---|---|---|---|
| opt | 5 | 368,366,634 | 20.2 | 19.5-21.8 | 4.6 | 49.46 | 12.3 | 291 | 337 |
| opt_binary | 3 | 368,366,634 | 16.5 | 15.9-16.8 | 2.3 | 60.75 | 10.9 | 291 | 337 |
| opt_hybrid | 3 | 368,366,634 | 20.5 | 19.9-20.6 | 1.5 | 48.74 | 12.6 | 291 | 337 |
| opt_latency | 5 | 368,366,634 | 15.5 | 15.2-16.1 | 2.1 | 64.51 | 10.5 | 291 | 337 |
| opt_noprefetch | 5 | 368,366,634 | 15.9 | 15.4-15.9 | 1.4 | 62.89 | 10.8 | 291 | 337 |
| opt_prefetch1 | 5 | 368,366,634 | 18.6 | 17.1-21.1 | 8.3 | 53.73 | 11.7 | 291 | 337 |
| opt_publish | 3 | 368,366,634 | 15.7 | 15.6-15.9 | 0.6 | 63.52 | 10.5 | 291 | 341 |
| reference | 5 | 368,366,634 | 3.9 | 3.8-4.0 | 1.9 | 257.06 | 3.5 | 440688711 | 317 |
| reference_latency | 1 | 368,366,634 | 3.5 | 3.5-3.5 | 0.0 | 285.54 | 3.2 | 440688711 | 317 |

## Per-message latency (ns; median across runs, [min-max])

| config | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |
|---|---|---|---|---|---|---|---|
| opt_latency | 42 [42-42] | 125 [125-125] | 250 [209-250] | 417 [375-417] | 917 [875-917] | 110488 [65745-2294378] | 62 [60-63] |
| reference_latency | 208 | 584 | 1042 | 1375 | 4248 | 125947878 | 283 |

Timer: reported 1.00 ns/tick; observed step median 42.0 ns (min 17.0 ns); read overhead 9.5 ns. Two reads bracket each message, so per-message values include ~one read of overhead.

## BBO hand-off to strategy thread (SPSC ring; ns, median across runs)

| config | BBO events | stalls | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| opt_publish | 174,890,436 | 0 | 125 | 166 | 209 | 7208 | 16976 | 160712 |
