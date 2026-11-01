// SPSC ring: single-threaded semantics plus a two-thread stress test (meaningful under TSan).
#include <gtest/gtest.h>

#include <thread>

#include "itchbook/spsc_ring.hpp"

using itchbook::SpscRing;

TEST(Spsc, FullAndEmpty) {
  SpscRing<int> r(4);
  int v;
  EXPECT_FALSE(r.try_pop(v));
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
  EXPECT_FALSE(r.try_push(99));
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(r.try_pop(v));
}

struct Payload {
  std::uint64_t seq;
  std::uint64_t check;
};

TEST(Spsc, TwoThreadStressPreservesOrderAndContent) {
  constexpr std::uint64_t kN = 2'000'000;
  SpscRing<Payload> r(1024);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN; ++i) {
      const Payload p{i, i * 0x9E3779B97F4A7C15ull};
      while (!r.try_push(p)) std::this_thread::yield();
    }
  });
  std::uint64_t expect = 0;
  Payload p{};
  while (expect < kN) {
    if (r.try_pop(p)) {
      ASSERT_EQ(p.seq, expect);
      ASSERT_EQ(p.check, expect * 0x9E3779B97F4A7C15ull);
      ++expect;
    }
  }
  producer.join();
  EXPECT_FALSE(r.try_pop(p));
}
