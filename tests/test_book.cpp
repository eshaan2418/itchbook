#include <gtest/gtest.h>

#include <vector>

#include "compare.hpp"
#include "itchbook/order_book.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

namespace {
struct Recorder {
  std::vector<Bbo>* out;
  void on_bbo(std::uint16_t, const Bbo& b, std::uint64_t) { out->push_back(b); }
};

template <class B>
void feed(B& b, const std::vector<std::uint8_t>& buf) {
  parse(buf.data(), buf.size(), b);
}
}  // namespace

TEST(Book, PriceTimePriorityAndAggregation) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.add(1, 1, 1, Side::Buy, 100, "X", 1000);
  e.add(1, 2, 2, Side::Buy, 200, "X", 1000);
  e.add(1, 3, 3, Side::Buy, 50, "X", 1010);
  e.add(1, 4, 4, Side::Sell, 70, "X", 1020);
  OrderBook<> b;
  feed(b, buf);
  const Bbo bbo = b.bbo(1);
  EXPECT_EQ(bbo.bid_px, 1010u);
  EXPECT_EQ(bbo.bid_qty, 50u);
  EXPECT_EQ(bbo.ask_px, 1020u);
  EXPECT_EQ(bbo.ask_qty, 70u);
  LevelInfo lv[4];
  ASSERT_EQ(b.depth(1, Side::Buy, lv, 4), 2u);
  EXPECT_EQ(lv[1], (LevelInfo{1000, 300, 2}));
  std::vector<std::uint64_t> fifo;
  b.for_each_order_at(1, Side::Buy, 1000, [&](std::uint64_t r, std::uint32_t) { fifo.push_back(r); });
  EXPECT_EQ(fifo, (std::vector<std::uint64_t>{1, 2}));
}

TEST(Book, ExecuteCancelDeleteReplace) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.add(1, 1, 1, Side::Sell, 100, "X", 1000);
  e.add(1, 2, 2, Side::Sell, 100, "X", 1000);
  e.executed(1, 3, 1, 30, 1);   // 70 left
  e.cancel(1, 4, 1, 20);        // 50 left
  e.replace(1, 5, 1, 9, 400, 990);  // order 1 -> 9 at new price, loses priority
  e.del(1, 6, 2);
  OrderBook<> b;
  feed(b, buf);
  EXPECT_EQ(b.level_count(1, Side::Sell), 1u);
  EXPECT_EQ(b.bbo(1).ask_px, 990u);
  EXPECT_EQ(b.bbo(1).ask_qty, 400u);
  EXPECT_EQ(b.live_orders(), 1u);
  EXPECT_EQ(b.stats().unknown_ref, 0u);
}

TEST(Book, ReplaceLosesQueuePriority) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.add(1, 1, 1, Side::Buy, 100, "X", 1000);
  e.add(1, 2, 2, Side::Buy, 100, "X", 1000);
  e.replace(1, 3, 1, 3, 100, 1000);  // same price, still goes to back
  OrderBook<> b;
  feed(b, buf);
  std::vector<std::uint64_t> fifo;
  b.for_each_order_at(1, Side::Buy, 1000, [&](std::uint64_t r, std::uint32_t) { fifo.push_back(r); });
  EXPECT_EQ(fifo, (std::vector<std::uint64_t>{2, 3}));
}

TEST(Book, IntegrityCounters) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.add(1, 1, 1, Side::Buy, 100, "X", 1000);
  e.add(1, 2, 1, Side::Buy, 100, "X", 1000);  // duplicate
  e.del(1, 3, 77);                             // unknown
  e.executed(1, 4, 1, 150, 1);                 // overfill: removes order
  OrderBook<> b;
  feed(b, buf);
  EXPECT_EQ(b.stats().duplicate_ref, 1u);
  EXPECT_EQ(b.stats().unknown_ref, 1u);
  EXPECT_EQ(b.stats().overfill, 1u);
  EXPECT_EQ(b.live_orders(), 0u);
  EXPECT_EQ(b.level_count(1, Side::Buy), 0u);
}

TEST(Book, EmitsBboOnlyOnTopOfBookChange) {
  std::vector<Bbo> events;
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.add(1, 1, 1, Side::Buy, 100, "X", 1000);  // new top: event
  e.add(1, 2, 2, Side::Buy, 100, "X", 990);   // behind top: no event
  e.cancel(1, 3, 2, 50);                      // behind top: no event
  e.add(1, 4, 3, Side::Buy, 10, "X", 1000);   // top qty change: event
  e.del(1, 5, 1);                             // top qty change: event
  e.del(1, 6, 3);                             // top level gone -> 990: event
  OrderBook<Recorder> b({}, Recorder{&events});
  feed(b, buf);
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0].bid_qty, 100u);
  EXPECT_EQ(events[1].bid_qty, 110u);
  EXPECT_EQ(events[2].bid_qty, 10u);
  EXPECT_EQ(events[3].bid_px, 990u);
  EXPECT_EQ(events[3].bid_qty, 50u);
}

template <class B>
class BookSearch : public ::testing::Test {};
using Variants = ::testing::Types<OrderBook<NullListener, LevelSearch::Linear>,
                                  OrderBook<NullListener, LevelSearch::Binary>,
                                  OrderBook<NullListener, LevelSearch::Hybrid>>;
TYPED_TEST_SUITE(BookSearch, Variants);

TYPED_TEST(BookSearch, DeepBookBothSidesOrdered) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  std::uint64_t ref = 1;
  // Insert levels in scrambled order on both sides.
  for (std::uint32_t i = 0; i < 300; ++i) {
    const std::uint32_t k = (i * 37) % 300;
    e.add(1, 1, ref++, Side::Buy, 100, "X", 100000 - k * 10);
    e.add(1, 1, ref++, Side::Sell, 100, "X", 100010 + k * 10);
  }
  auto b = std::make_unique<TypeParam>();
  feed(*b, buf);
  const auto bids = test::full_depth(*b, 1, Side::Buy);
  const auto asks = test::full_depth(*b, 1, Side::Sell);
  ASSERT_EQ(bids.size(), 300u);
  ASSERT_EQ(asks.size(), 300u);
  for (std::size_t i = 1; i < 300; ++i) {
    EXPECT_GT(bids[i - 1].price, bids[i].price);
    EXPECT_LT(asks[i - 1].price, asks[i].price);
  }
  // Remove every third order and check again.
  std::vector<std::uint8_t> buf2;
  Encoder e2(buf2);
  for (std::uint64_t r = 1; r < ref; r += 3) e2.del(1, 2, r);
  feed(*b, buf2);
  EXPECT_EQ(b->live_orders(), 600u - (600u + 2) / 3);
  EXPECT_FALSE(b->crossed(1));
}
