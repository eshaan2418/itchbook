#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "itchbook/latency.hpp"

using itchbook::Histogram;

TEST(Histogram, ExactBelow2048) {
  Histogram h;
  for (std::uint64_t v = 0; v < 1000; ++v) h.record(v);
  EXPECT_EQ(h.quantile(0.0), 0u);
  EXPECT_EQ(h.quantile(0.5), 499u);
  EXPECT_EQ(h.quantile(1.0), 999u);
  EXPECT_EQ(h.max(), 999u);
}

TEST(Histogram, IndexValueRoundTripWithinRelativeError) {
  for (std::uint64_t v : {2048ull, 2049ull, 4095ull, 4096ull, 123456789ull, 1ull << 40}) {
    const auto lo = Histogram::value_at(Histogram::index(v));
    EXPECT_LE(lo, v);
    EXPECT_LT(static_cast<double>(v - lo) / static_cast<double>(v), 1.0 / 1024);
  }
}

TEST(Histogram, QuantilesMatchSortedSample) {
  std::mt19937_64 rng(1);
  std::lognormal_distribution<double> d(8.0, 1.5);
  std::vector<std::uint64_t> xs(200000);
  Histogram h;
  for (auto& x : xs) {
    x = static_cast<std::uint64_t>(d(rng));
    h.record(x);
  }
  std::sort(xs.begin(), xs.end());
  for (double q : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
    const auto exact = xs[static_cast<std::size_t>(q * static_cast<double>(xs.size() - 1))];
    const auto got = h.quantile(q);
    EXPECT_NEAR(static_cast<double>(got), static_cast<double>(exact),
                static_cast<double>(exact) / 1000.0 + 1.0)
        << "q=" << q;
  }
}
