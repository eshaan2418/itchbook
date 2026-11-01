# Performance log

Each optimisation, its measured effect and why it worked or didn't. All numbers are full-day
replays of the 2019-01-30 NASDAQ sample (368,366,634 messages) on an Apple M5 Pro unless marked
*micro*. "Book Mmsg/s" times decode + book + BBO detection and excludes zlib decompression.

**Session drift.** On this laptop, results drift up to ~5% between benchmark sessions (other
system activity, SLC state, frequency behaviour), while runs within a session vary by < 1%. So
comparisons below are within one session; the session directory is named for each result.
Close calls were settled with interleaved A/B runs (`bench/run_ab.sh`).

## 0. Profile of the starting point

`sample` (macOS) on a full-day replay with the linear-search book and no prefetch put the book's
time in `on_delete`, `remove_order`, `find_or_create_level` and the inlined dispatch loop. Each
does little arithmetic and touches 3-5 unrelated cache lines: hash slot, order record, level
record, side array, book header. The working set (128 MiB hash table, ~56 MB of live orders, ~17 MB
of levels at the 15:57 peak) is far larger than the caches. **The workload is bound by memory
latency, not instructions**, so the main lever is overlapping misses, not reducing work.
Parser-only decode costs 4.8 ns/msg (*micro*, `BM_ParseOnly`), about 10% of the total.

## 1. Data structures vs the reference

Session `20261101-062825` (5 runs each):

| | Mmsg/s | ns/msg |
|---|---|---|
| reference: `std::map` per side + `std::list` per level + `unordered_map` index | 3.8 | 262.6 |
| optimised, no prefetch | 15.1 | 66.0 |

**4.0×** from data structures alone: no allocation per order or level, 32-byte records linked by
32-bit indices, an open-addressing map (one probe sequence in a flat array instead of bucket → node
pointer chasing), and side arrays where the touch is at `back()`. On synthetic flow with small books
that fit in cache the gap is only 1.6× (*micro*: 21.1 vs 13.3 Mmsg/s,
`ab-20261101-070800/micro.txt`). The real-data advantage comes mostly from cache behaviour at
1.7M live orders, which a synthetic benchmark does not reproduce.

Order map in isolation (*micro*, random erase order, same op sequence for both maps):

| live keys | `OrderMap` | `std::unordered_map` (reserved) |
|---|---|---|
| 65,536 | 6.0 ns / op pair | 48.8 ns |
| 4,194,304 | 21.6 ns | 389 ns |

An earlier version of this benchmark erased keys in insertion order. With `std::hash<uint64_t>`
being the identity, that gave `unordered_map` perfectly sequential buckets, and it looked faster
than `OrderMap` at 4M keys (12.5 vs 14.2 ns). ITCH orders don't die in FIFO order, so the
benchmark was changed to random erase order. Both results are kept in the results directories.

## 2. Software prefetch, stage 1: hash slot of message i+D

Message i+D is already in the buffer, so its ref, and therefore its hash-slot address, is known D
messages early. `__builtin_prefetch` on that slot overlaps the map miss with the current message's
work.

| session `20261101-062825` | Mmsg/s | Δ vs no prefetch |
|---|---|---|
| no prefetch | 15.1 | |
| stage 1, D = 16 | 19.3 | +28% |

## 3. Stage 2: resolve the ref and prefetch the order record (or the add's side array)

At distance D/2 the slot is already cached, so `map_.find()` is cheap. The order record it points
to is prefetched. For adds, which have no order yet, the best end of the target side's level
array is prefetched instead, and stage 1 also prefetches the symbol's `Book` header.

| session `20261101-062825` | Mmsg/s | Δ vs stage 1 |
|---|---|---|
| stage 1, D = 16 | 19.3 | |
| stage 1 + 2, D = 16 | **21.1** | +9% (total +40% vs no prefetch) |

Prefetches are hints. If a ref is created or deleted between prefetch and use, one prefetch is
wasted and correctness is unaffected (the full-day verify runs without prefetch; the replay
integrity counters with prefetch are identical: all zero).

## 4. Stage 3 (rejected): also prefetch the level record

Reading the now-cached order to prefetch its level costs a second map probe and an order load per
message, and the level record is often already hot (levels near the touch are shared by many
orders).

| session `experiments-20261101-065607` | Mmsg/s |
|---|---|
| stages 1+2, D = 32 | 22.1 |
| stages 1+2+3, D = 16 | 20.2 |
| stages 1+2+3, D = 32 | 19.9 |

**−10%**, kept only as `--prefetch-stages 3` for reference.

## 5. Prefetch distance

| session `experiments-20261101-065607`, stages 1+2 | Mmsg/s |
|---|---|
| D = 8 | 22.5 |
| D = 32 | 22.1 |
| D = 64 | 21.8 |

| interleaved A/B, session `ab-20261101-070800` | Mmsg/s (median of 3) |
|---|---|
| D = 16 | 22.4 (22.2-22.5) |
| D = 8 | 22.4 (22.4-22.6) |

D = 8 and D = 16 are indistinguishable. Larger distances lose slightly: prefetched lines get
evicted before use, and too many outstanding misses compete. The default stays at 16.

## 6. Level search: linear from the touch vs binary vs hybrid

| | session | Mmsg/s |
|---|---|---|
| linear (scan from `back()`) | `20261101-062825` | 21.1 |
| binary (`partition_point`) | `20261101-062825` | 16.8 |
| hybrid (scan 8 from touch, then binary) | `20261101-062825` | 20.1 |
| linear, D = 8 | `ab-20261101-070800` (A/B) | 22.4 |
| hybrid, D = 8 | `ab-20261101-070800` (A/B) | 21.6 |

Linear wins on real data because activity is concentrated near the touch. A scan from `back()`
then does a few compares on an already-hot line, while binary search touches about log2(n) lines
spread across the array. (Distance-from-touch distribution is not instrumented; that is an
inference from the result.) Binary search is 20% slower. Hybrid
bounds the worst case for deep or adversarial books and costs 3.5%. On the synthetic micro flow
binary is marginally faster (21.5 vs 21.1 Mmsg/s), because the generator's geometric price
distribution places more activity away from the touch than real NASDAQ flow does. Real data
decides.

## 7. Side-array reserve and the remaining allocations

The replay links the counting `operator new`. With the original 64-level initial reserve, 4,100
allocations happened after the first chunk over the whole day. All are side arrays doubling the
first time a symbol's book gets deeper than its capacity.

| interleaved A/B, session `ab-20261101-070800`, D = 8 | Mmsg/s | allocations after first chunk | peak RSS |
|---|---|---|---|
| reserve 64 | 22.4 | 4,100 | 315 MB |
| reserve 256 | 22.3 | 291 | 337 MB |
| reserve 1024 (session `experiments-…`) | 21.8 | 24 | 439 MB |

Reserve 256 removes 93% of the allocations at no measurable cost (+22 MB), so it is now the
default. 1024 removes nearly all of them but adds ~125 MB of mostly untouched capacity across 8.7k
symbols. In production, per-symbol reserves would be sized from the previous day's maximum depth.
The synthetic allocation test (`tests/test_alloc.cpp`) shows exactly zero after warm-up when
capacities are sized.

## 8. Considered, not done

- **SIMD decode.** ITCH fields are 1-8 bytes at fixed offsets within one message and must be
  applied in order. A field load is one `LDR` + `REV`, and decode is ~10% of total time. There is
  nothing to vectorise.
- **Huge pages.** The order map spans 8,192 16 KB pages. macOS offers no practical userspace
  huge-page API; on Linux, `MAP_HUGETLB` or THP for the map and pools is the obvious next step.
- **Smaller map.** Halving the order-capacity, and with it the map to 2^22 slots, looked slower in a
  40M-message probe. That run was not repeated within one session, so it is not claimed as a result.

## 9. Cost of measurement itself

| session `20261101-062825` | Mmsg/s |
|---|---|
| untimed | 21.1 |
| per-message timing (`--latency`) | 14.9 |
| publish mode (timestamp + SPSC push per BBO change) | 15.7 |

A timer read is `ISB; MRS CNTVCT_EL0`, which costs 8.7 ns (`itchbook timer`). The `ISB` is a
pipeline barrier, so it also stops the out-of-order core from overlapping misses across messages.
The prefetch pipeline still helps, but less. So the latency histogram describes the timed
configuration, and throughput is the better estimate of the untimed per-message cost.
