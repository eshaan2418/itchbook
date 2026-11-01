#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "itchbook/order_map.hpp"

using itchbook::OrderMap;

TEST(OrderMap, BasicOps) {
  OrderMap m(16);
  EXPECT_EQ(m.find(1), OrderMap::kNotFound);
  EXPECT_TRUE(m.insert(1, 10));
  EXPECT_FALSE(m.insert(1, 11));
  EXPECT_EQ(m.find(1), 10u);
  EXPECT_EQ(m.erase(1), 10u);
  EXPECT_EQ(m.erase(1), OrderMap::kNotFound);
  EXPECT_EQ(m.size(), 0u);
}

// Differential test against std::unordered_map with heavy churn in a small table, so probe runs
// wrap around and backward-shift deletion is exercised constantly.
TEST(OrderMap, MatchesStdUnorderedMapUnderChurn) {
  std::mt19937_64 rng(7);
  OrderMap m(64);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  for (int i = 0; i < 400000; ++i) {
    const std::uint64_t k = rng() % 200;
    switch (rng() % 3) {
      case 0: {
        const auto v = static_cast<std::uint32_t>(rng());
        EXPECT_EQ(m.insert(k, v), ref.emplace(k, v).second);
        break;
      }
      case 1: {
        auto it = ref.find(k);
        const std::uint32_t want = it == ref.end() ? OrderMap::kNotFound : it->second;
        EXPECT_EQ(m.erase(k), want);
        if (it != ref.end()) ref.erase(it);
        break;
      }
      default: {
        auto it = ref.find(k);
        EXPECT_EQ(m.find(k), it == ref.end() ? OrderMap::kNotFound : it->second);
      }
    }
    ASSERT_EQ(m.size(), ref.size());
  }
}

TEST(OrderMap, GrowsAndKeepsEntries) {
  OrderMap m(16);
  for (std::uint64_t k = 0; k < 10000; ++k) ASSERT_TRUE(m.insert(k * 3 + 1, static_cast<std::uint32_t>(k)));
  EXPECT_GT(m.rehashes(), 0u);
  for (std::uint64_t k = 0; k < 10000; ++k) ASSERT_EQ(m.find(k * 3 + 1), k);
}
