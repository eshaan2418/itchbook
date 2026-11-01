# itchbook design

## 1. Pipeline

```
 gzip file / stdin
      │  zlib inflate, 64 MiB chunks (FrameChunker carries a partial trailing frame)
      ▼
 framed bytes  [u16 len][msg][u16 len][msg]...
      │  for_each_frame: one u16 load per frame (frame index for prefetch look-ahead)
      ▼
 dispatch(msg, len, handler)      ← switch on type, decode into a stack POD, static call
      │
      ▼
 OrderBook<Listener, Search>      ← L3 state for all ~8.7k symbols
      │  publish(): compare new top-of-book with the cached one per symbol
      ▼
 Listener::on_bbo                 ← inlined; NullListener, or PublishListener → SpscRing
      │
      ▼ (optional, --publish)
 strategy thread (busy-poll consumer)
```

Threading model: one feed-handler thread owns the parser and all books, with no locks or atomics
on the book path. This is the standard deployment: one core per feed partition (NASDAQ splits
symbols across multicast groups in production). Downstream consumers receive BBO events over a
bounded SPSC ring. A slow consumer causes counted producer stalls, never silent drops.

## 2. Parser

- **Zero-copy**: fields are read in place from the caller's buffer. The only copy is into a small
  stack struct, which the optimiser usually scalarises into registers.
- **Big-endian decode**: `memcpy` into an integer plus `__builtin_bswap*`. This compiles to an
  unaligned `LDR` + `REV` on arm64 and `MOVBE` or `MOV`+`BSWAP` on x86-64. ITCH fields are
  2-8 bytes at fixed offsets, so SIMD has nothing to vectorise. A structure-of-arrays bulk decode
  would only help a column-store consumer, not a book that applies messages strictly in sequence.
- **Validation**: each type has a fixed length (`expected_size`). Unknown types and wrong lengths
  are reported to the handler and skipped, and the framing lets the parser resync on the next
  message. A zero-length frame is reported, never read. The parser is fuzzed in CI with
  ASan/UBSan, feeding both books and asserting they agree.
- **Dispatch**: a `switch` over the type byte, with handlers as template parameters
  (`HandlerBase` provides no-op defaults; no virtual functions). The compiler inlines the book
  update into the switch arm.

## 3. Order book data structures

### Order-ID map (`order_map.hpp`)
ITCH references are 64-bit and unique per day across all symbols. Peak live orders on the sample
day: 1,742,866 (`bench/results/stats_01302019.json`). The map is:

- open addressing with linear probing over 16-byte slots, 8 per 128-byte line;
- Fibonacci hashing (`key * 2^64/φ >> shift`). NASDAQ assigns refs mostly in increasing order,
  so identity hashing would cluster badly; the multiply spreads consecutive keys over the table;
- **backward-shift deletion**: no tombstones. A day has ~192M inserts (164.7M adds + 27.2M replaces) and nearly as many erases,
  and a tombstone table would degrade or need periodic rebuilds;
- default capacity 2^23 slots (128 MiB) for up to 4M orders, so load factor is ≤ 0.21 at the
  sample day's peak. Growth is a counted slow path.

Rejected alternatives: `std::unordered_map` (node-based, one allocation per insert, pointer-chasing
buckets; see the micro-benchmark); a direct-indexed array on ref (the sample day reaches ref 329,176,641, so 1.3 GB
of mostly empty memory); robin-hood hashing (lower probe-length variance, but at load ≤ 0.25 plain
linear probing already averages close to one probe and has a simpler erase).

### Order and level pools
- `Order` (32 B): ref, shares, price, level index, prev/next indices, locate, side. Indices are
  32-bit: half the size of pointers, and they stay valid if the pool vector ever grows.
- `Level` (24 B): aggregate shares, count, price, FIFO head/tail.
- Free lists recycle slots, so steady state performs no allocation.

### Price levels per side
Each side is a `std::vector<{price, level_index}>` sorted **worst → best**, so the best price is at
`back()`. Most ITCH activity happens within a few ticks of the touch: adding a new best level is
`push_back`, and removing it is `pop_back`. A new level k positions behind the touch shifts k
8-byte entries, and k is usually small. Lookup is a template choice:

- `LevelSearch::Linear`: scan from the back (the touch). Cost is proportional to distance from
  the touch, which is usually small.
- `LevelSearch::Binary`: `std::partition_point`, O(log n) regardless of distance.

Both are differential-tested. The replay benchmark compares them on the full day
(`docs/PERF_LOG.md`).

Rejected: `std::map` per side (node per level, pointer chasing, allocation per new level; this is
the reference/baseline); a dense price-indexed array per symbol (O(1), but NASDAQ prices span
$0.0001 to $100k+ with sparse levels, so it needs a sliding window plus re-centring and is fragile
for 8.7k symbols at once); a tree of bitmaps (O(1) best-price find, but more complex and only
worth it for very deep books).

### BBO events
After each book-affecting message, `publish()` reads the two `back()` levels, builds the new BBO and
compares it to the per-symbol cached BBO. It emits only on change. This costs two hot cache lines
per message and removes the need for case analysis ("was the touched level the top?").

## 4. Allocation, exceptions, and branches

- The tool is built with `-fno-exceptions -fno-rtti`. The book path has no `throw`, no virtual
  dispatch and no `dynamic_cast`.
- `tests/test_alloc.cpp` replaces global `operator new` with a counter. It warms the book up on
  1M synthetic messages, then asserts **zero** allocations over the next 1M. The replay tool
  links the same counter and reports `heap_allocs_after_first_chunk` on real data.
- `[[unlikely]]` marks only the integrity-error paths and pool/map growth, which are never taken
  on a clean feed. No other branch hints are used.

## 5. Concurrency (publish mode)
`SpscRing<T>`: power-of-two ring with monotonically increasing 64-bit indices. The producer and
consumer blocks are each `alignas(128)`, matching `hw.cachelinesize` = 128 on Apple M-series and
x86's adjacent-line prefetcher. Each side caches the other side's index and reloads it (acquire)
only when the ring looks full or empty. Memory ordering:

- producer: relaxed load of own `tail`; write slot; **release** store `tail+1` (publishes the slot);
- consumer: relaxed load of own `head`; **acquire** load `tail` (synchronises with the release);
  read slot; **release** store `head+1` (lets the producer reuse the slot);
- producer's **acquire** load of `head` when full pairs with the consumer's release.

No sequentially consistent operations and no read-modify-write instructions are needed. TSan runs
the two-thread stress test in CI.

## 6. Determinism and reproducibility
- The replay is a pure function of the input bytes. Book state depends only on message order.
- The synthetic generator uses its own xoshiro256** and distributions, not `<random>`
  distributions, which differ between libc++ and libstdc++. Fixtures are therefore identical on
  macOS and Linux.
- Raw NASDAQ data is never committed. `scripts/fetch_itch.sh` downloads it and checks a pinned
  SHA-256.

## 7. Timing methodology
See README §Performance. On Apple silicon the timer is `CNTVCT_EL0` (24 MHz, 41.67 ns per tick),
read with a preceding `ISB`. Per-message latency is measured as the delta between consecutive
timestamps around each `dispatch()`. Values are quantised to 41.67 ns, so the per-message
histogram's low percentiles are coarse, and the mean comes from aggregate throughput. On Linux CI,
`steady_clock` has ns resolution.
