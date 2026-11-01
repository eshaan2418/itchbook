# itchbook: deep-dive interview questions

Hard questions a trading-firm interviewer might ask about this project, with answers grounded in
the code. File references are relative to the repository root.

---

### 1. Walk me through what happens to one Order Delete message, from bytes to BBO event.

`for_each_frame` reads the 2-byte big-endian length and hands `dispatch()` a pointer to the
19-byte message. `dispatch` checks `expected_size('D') == 19` and switches on the type byte.
`decode_order_delete` does two unaligned loads with byte swaps (locate, timestamp) and one 8-byte
load+REV (ref). `OrderBook::on_delete` calls `OrderMap::erase(ref)`: Fibonacci hash, linear probe,
backward-shift to close the hole, return the pool index. `remove_order` unlinks the order from its
level's intrusive FIFO, subtracts its shares from the level aggregate, and if the level is now
empty, removes the `{price, level}` entry from the side array (usually `pop_back`, since the level
is at or near the touch) and pushes the level and order slots on their free lists. `publish()` then
compares the symbol's new top-of-book with its cached `Bbo` and calls `Listener::on_bbo` only if it
changed. The whole path is inlined: no virtual calls, no allocation, no locks.

### 2. Why a global order map instead of one per symbol?

ITCH order refs are unique for the day across all symbols, and E/X/D/U messages identify an order
only by ref. The locate field in their header is redundant. One global map means one probe
sequence per message and one big table sized for the day's peak (1.74M live orders on the sample
day, `bench/results/stats_01302019.json`). Per-symbol maps would cost an extra indirection, and
8.7k small tables would each need resizing logic.

### 3. Why Fibonacci hashing? Refs are already unique integers.

They are unique but highly structured: NASDAQ assigns them in roughly increasing order, so live
refs at any moment form dense runs. Identity hashing (`ref & mask`) maps those runs to contiguous
slots and builds long linear-probe clusters. A multiply by 2^64/φ followed by taking the high bits
spreads consecutive keys roughly evenly. It costs one multiply, cheaper than any mixing hash, and
keeps the high-bits property that power-of-two tables need.

### 4. Explain backward-shift deletion and why you didn't use tombstones.

With linear probing, emptying a slot can break the probe chain of a later key whose home slot is
before the hole. Backward shift walks forward from the hole. Each subsequent key whose home slot is
cyclically at or before the hole moves back into it; the hole then advances, and the walk stops at
the first empty slot. The test is `((j - home) & mask) >= ((j - hole) & mask)` in
`order_map.hpp`. Tombstones avoid the shifting, but a feed with ~192M inserts and ~190M erases per
day would fill the table with them, so lookups would degrade until a rehash, a latency spike in
the middle of the session. Backward shift keeps probe lengths at what the live load factor implies.

### 5. Your levels are a sorted vector. Isn't insertion O(n)?

It is O(distance from the end), and the array is ordered worst to best, so the best price is at
`back()`. Most ITCH adds and deletes happen at or within a few ticks of the touch, so the shift is
a handful of 8-byte entries, often zero (`push_back`/`pop_back`). The alternatives cost more on the
common path. A `std::map` is a node allocation plus 3-4 dependent cache misses per new level. A
dense price-indexed array needs re-centring and has unbounded range across 8.7k symbols. The
full-day benchmark compares linear scan, binary search and a hybrid (8-entry scan then binary
search). Linear scan from the touch wins overall: see `docs/PERF_LOG.md`.

### 6. When would linear scan from the touch be the wrong choice?

When activity is far from the touch in very deep books: thin stocks with stale orders scattered
over hundreds of levels, or a strategy that sweeps deep levels. Then every add or delete scans
O(depth). The hybrid variant bounds that at 8 + log2(depth). It is close on the full day and is
the safer choice for adversarial or skewed books. A production system would pick per symbol, or
use a bitmap tree over a price window for the top N ticks.

### 7. Why do orders store 32-bit indices rather than pointers?

Half the size (the `Order` record is 32 B, so four fit in a 128-byte line), and indices stay valid
if a pool `std::vector` ever reallocates. The cost is one base+index address computation, which is
free in the addressing mode.

### 8. How do you know the hot path doesn't allocate?

`tools/alloc_counter.cpp` replaces global `operator new`/`delete` with a relaxed atomic counter.
`tests/test_alloc.cpp` warms the book up on 1M synthetic messages, then asserts zero allocations
over the next 1M. On the real day the replay reports `heap_allocs_after_first_chunk`. With a 64-level initial
reserve per side there were 4,100 over 368M messages, all side arrays doubling the first time a
symbol's book got deeper than its capacity. A 256-level reserve (now the default) cuts that to
291 at no measurable throughput cost; 1024 cuts it to 24 but adds about 125 MB of mostly untouched
memory (`docs/PERF_LOG.md` §7). In production
I would pre-size each symbol's side arrays from the previous day's maximum depth.

### 9. Where does the time actually go per message?

Memory latency. The data structures are small, but the working set (hash table, about 56 MB of
live orders, about 17 MB of levels at peak) is far larger than L2, and each message touches 3-5
unrelated lines: hash slot, order record, level record, side array, book header. Instruction count
is small. The profile (`sample` on macOS) shows the time in `on_delete`, `remove_order`,
`find_or_create_level` and the dispatch loop, all memory-bound. That is why software prefetching is
the main lever.

### 10. Explain the prefetch pipeline. Why does it work when the CPU has hardware prefetchers?

Hardware prefetchers detect strides, but hash slots and pool records are random, so they can't help.
The trick is that the future is already in the buffer: message i+16 is sitting in memory, so its
ref, and therefore its hash slot address, is known 16 messages early. Stage 1 (`prefetch_stage1`,
distance D) prefetches the hash slot and, for adds, the `Book` header. Stage 2 (distance D/2) does
the now-cheap map lookup and prefetches the order record, or for adds the best end of the side
array. A third stage that also prefetched the level record measured slower: its extra lookup costs
more than the miss it hides. That result is in the perf log too. Prefetches are hints, so a stale
ref (created or deleted in between) wastes a prefetch but can never be wrong.

### 11. How did you choose the prefetch distance?

Empirically: distances of 8, 16, 32 and 64 were swept. The best distance covers DRAM latency (about
100 ns) with roughly 40-50 ns of work per message: 16 ahead with the second stage at 8 gives each
stage several message-times of lead. Too short a distance doesn't hide the miss; too long a
distance evicts prefetched lines before use and wastes memory-level parallelism.

### 12. How is correctness established?

Five layers:
1. A deliberately naive `ReferenceBook` (`std::map` + `std::list` + `unordered_map`).
2. Differential tests comparing full depth and every FIFO queue after every message on synthetic
   flow with all message types and injected integrity violations.
3. RapidCheck property tests generating random operation sequences, including executes and cancels
   against dead or unknown refs, plus invariants (level aggregate = sum of orders, no empty levels).
4. A libFuzzer harness (CI) feeding arbitrary bytes to both books under ASan/UBSan.
5. The real day: `itchbook verify` compares the top 10 levels after each of 363M book messages,
   full depth every 1000th message, and every symbol at the end of day. Result: 0 mismatches,
   0 unknown refs, 0 duplicates, 0 overfills, and 0 live orders at end of day
   (`bench/results/verify_01302019.json`).

I also checked that the tests have teeth: a planted bug (not updating level shares on partial
execution) made 8 test cases fail.

### 13. The book was crossed after 663 messages. Is that a bug?

Not in the book builder. Both independently written books agree on every message, and the
integrity counters are all zero, so the crossed states are in the feed itself, not divergence. 663
out of 363M book messages is 1 in 550,000, and the states are transient. I haven't attributed each
one. Plausible causes are halts and auction re-openings, and the ordering of events in the feed
between an aggressive order's add and the executions it causes. Attributing each case (joining on
`H` trading-action messages and timestamps) is listed in the roadmap. I report the counts
rather than assert "never crossed" because asserting a property the venue doesn't guarantee would
make the handler crash on valid data.

### 14. What happens if a message is lost?

On this file replay it can't happen. In a live MoldUDP64 deployment the session layer detects
sequence gaps and requests retransmission or falls back to a snapshot. If a message did slip
through, the integrity counters catch it. A lost Add makes later E/X/D/U for that ref show up as
`unknown_ref`. A lost Delete leaves a stale order that never trades, often visible as a
persistent crossed or locked book, and as a nonzero live-order count at end of day (the sample day
ends with exactly zero). The policy (count and ignore, or remove on overfill) is identical
in both books so they stay comparable, but a production system should treat any nonzero counter as
"book for this symbol is untrusted, resync".

### 15. Why single-threaded? Wouldn't parsing on one core and book-building on another be faster?

A cross-core handoff costs more than the work being handed off. Decode is a few nanoseconds; the
publish-mode benchmark shows the SPSC hand-off itself costs about 125 ns median, plus cache
lines bouncing between cores. Book updates must be applied in sequence, so they don't parallelise.
The scalable split is by symbol: NASDAQ already shards symbols across multicast groups, and you run
one handler thread per group. The only thread boundary in itchbook is downstream, BBO events to a
strategy thread, where the consumer genuinely needs its own core.

### 16. Justify every memory order in the SPSC ring.

- Producer loads its own `tail` relaxed: only the producer writes it.
- Producer stores `tail+1` with release: orders the slot write before the index publication.
- Consumer loads `tail` with acquire: pairs with that release, so the slot contents are visible.
- Consumer stores `head+1` with release: orders the slot read before the slot is handed back.
- Producer loads `head` with acquire only when the ring looks full: pairs with that release, so it
  never overwrites a slot still being read.

No seq_cst (no store→load ordering needed), no RMW. Each side caches the other's index, so the
shared line is touched only when the ring looks full or empty. The indices are 64-bit and
monotonically increasing, so there's no ABA and no wrap arithmetic. Producer and consumer state is
`alignas(128)`: Apple M-series has 128-byte lines (`hw.cachelinesize`), and on x86 the
adjacent-line prefetcher makes 64-byte padding insufficient against false sharing.

### 17. What does a slow consumer do to the feed thread?

The current policy is lossless back-pressure: `PublishListener` spins and counts
`producer_stalls`. For a trading system that is usually the wrong default, because stalling the
feed thread delays every symbol. Better policies are conflation (keep only the latest BBO per
symbol in a per-symbol slot plus a dirty bitmap: the consumer always sees current state and the
producer never blocks) or disconnecting the consumer. The stall counter is the slow-consumer
detector. In the full-day run it stayed at zero with a 64K-slot ring.

### 18. Your latency histogram's p50 is "42 ns". How precise is that?

Not very, and the README says so. Apple silicon's architected counter (`CNTVCT_EL0`) runs at
24 MHz, so one tick is 41.67 ns. A message that takes 20 ns records as 0 or 1 tick depending on
where the tick edges fall. The percentiles are quantised to 41.67 ns, but they are unbiased in
aggregate: the histogram mean agrees with aggregate throughput. The tails (p99.9, p99.99, max) are
far above the quantum and are meaningful. User-space cycle counters (PMCR) aren't available on
macOS without kernel extensions. On Linux x86 the same code uses `steady_clock` (TSC via vDSO,
about 1 ns resolution). With isolated cores I would use `rdtscp` with `lfence` directly.

### 19. What breaks at 10× message rate?

Throughput has headroom: the book processes the whole day's 368M messages in under 20 s of CPU
time, against a wall-clock session of about 17 hours. The real issue is bursts. NASDAQ's peak
microburst rates are orders of magnitude above the daily average, so what matters is the tail
latency at the queue head during a burst. At 10× the order count, the order map (8M slots) would
exceed its load-factor threshold and rehash mid-session, a multi-millisecond stall, so it would be
pre-sized. Memory per order is 32 B + 16 B/0.25 load = about 96 B, so 17M live orders is about
1.6 GB. Fine on a server, but TLB reach becomes the bottleneck, so huge pages
(`MAP_HUGETLB`/THP on Linux) would be the first change.

### 20. How would you take this to kernel bypass or FPGA?

Kernel bypass: replace the file reader with an ef_vi / DPDK / AF_XDP receive loop on an isolated
core. Parse MoldUDP64 headers in place on the RX ring and feed `dispatch()` directly. No copy is
needed because the parser is already zero-copy over a caller-owned buffer. Pin with `isolcpus` and
`nohz_full`, use huge pages for the pools, and busy-poll.

FPGA: put the parser and the BBO-for-a-subset-of-symbols logic in hardware. ITCH's fixed-length
messages and big-endian fields are a natural fit. Hardware book-building is typically limited to
top-N levels per symbol in BRAM/URAM with the order map in on-board DRAM/HBM. The software book
stays as the full-depth reference, and the differential harness here would validate the FPGA's
output.

### 21. Why are prices `uint32` and aggregates `uint64`?

ITCH prices are 4-decimal fixed point in a u32 (max about $429,496), so storing them unconverted
avoids floating point entirely and keeps comparisons exact. Share counts per order are u32, but a
level aggregates many orders, and a pathological level could exceed 2^32 shares, so `Level::shares`
is u64.

### 22. How would you extend this to multiple venues?

Each venue gets its own handler and book (BX, PSX, and NYSE's different protocols). A consolidated
layer above them keeps per-venue BBOs and computes the NBBO. The design already separates the
parser (protocol), the book (state) and the listener (output), so a new venue mostly means a new
parser emitting the same callbacks.
