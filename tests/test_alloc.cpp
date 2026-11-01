// Proves the steady-state hot path is allocation-free: after a warm-up phase sizes the pools,
// hash map and per-side level arrays, processing further messages performs zero heap
// allocations (global operator new is replaced by a counter, see tools/alloc_counter.cpp).
#include <gtest/gtest.h>

#include "itchbook/alloc_counter.hpp"
#include "itchbook/order_book.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

TEST(Alloc, NoHeapAllocationAfterWarmup) {
  SynthConfig cfg{.seed = 21, .symbols = 16, .messages = 2'000'000};
  cfg.max_live_per_symbol = 2000;
  const auto buf = generate_synthetic(cfg);
  auto b = std::make_unique<OrderBook<>>(BookConfig{1u << 16, 1u << 14, 256});

  // Warm-up: first half of the stream.
  std::size_t half = 0;
  {
    std::size_t n = 0;
    for_each_frame(buf.data(), buf.size(), [&](const std::uint8_t* m, std::uint16_t len) {
      if (n++ == cfg.messages / 2) half = static_cast<std::size_t>(m - 2 - buf.data());
      (void)len;
    });
  }
  parse(buf.data(), half, *b);
  const auto before = alloc_count();
  const auto r = parse(buf.data() + half, buf.size() - half, *b);
  const auto after = alloc_count();
  EXPECT_GT(r.messages, 900'000u);
  EXPECT_EQ(after - before, 0u) << "hot path allocated " << (after - before) << " times";
  EXPECT_EQ(b->map_rehashes(), 0u);
}
