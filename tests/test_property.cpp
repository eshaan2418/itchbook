// Property-based tests (RapidCheck): random operation sequences, including invalid ones,
// must leave the optimised and reference books identical, and codec round-trips are exact.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "compare.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

namespace {
struct Op {
  int kind;            // 0 add, 1 exec, 2 cancel, 3 delete, 4 replace, 5 exec-price
  bool sell;
  std::uint32_t tick;  // price offset in ticks
  std::uint32_t shares;
  std::uint32_t pick;  // which ref to target (may be dead/unknown on purpose)
};

std::vector<std::uint8_t> encode_ops(const std::vector<Op>& ops, std::size_t symbols) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  std::uint64_t next = 1;
  std::uint64_t ts = 1;
  for (const Op& op : ops) {
    const auto locate = static_cast<std::uint16_t>(1 + op.pick % symbols);
    // Refs used by non-add ops come from [1, next + 2): mostly live/dead, sometimes unknown.
    const std::uint64_t target = 1 + op.pick % (next + 1);
    const std::uint32_t price = op.sell ? 10000 + op.tick * 100 : 9900 - op.tick * 100;
    switch (op.kind) {
      case 0:
        e.add(locate, ts, next++, op.sell ? Side::Sell : Side::Buy, op.shares, "P", price);
        break;
      case 1: e.executed(locate, ts, target, op.shares, ts); break;
      case 2: e.cancel(locate, ts, target, op.shares); break;
      case 3: e.del(locate, ts, target); break;
      case 4: e.replace(locate, ts, target, next++, op.shares, price); break;
      default: e.executed_price(locate, ts, target, op.shares, ts, price); break;
    }
    ++ts;
  }
  return buf;
}
}  // namespace

namespace rc {
template <>
struct Arbitrary<Op> {
  static Gen<Op> arbitrary() {
    return gen::build<Op>(
        gen::set(&Op::kind, gen::weightedOneOf<int>({{5, gen::just(0)}, {2, gen::just(1)},
                                                     {2, gen::just(2)}, {2, gen::just(3)},
                                                     {2, gen::just(4)}, {1, gen::just(5)}})),
        gen::set(&Op::sell, gen::arbitrary<bool>()),
        gen::set(&Op::tick, gen::inRange<std::uint32_t>(0, 12)),
        gen::set(&Op::shares, gen::inRange<std::uint32_t>(1, 400)),
        gen::set(&Op::pick, gen::arbitrary<std::uint32_t>()));
  }
};
}  // namespace rc

RC_GTEST_PROP(Property, OptimisedEqualsReference, (const std::vector<Op>& ops)) {
  const auto buf = encode_ops(ops, 3);
  auto opt = std::make_unique<OrderBook<>>(BookConfig{8, 4, 1});
  ReferenceBook ref;
  test::Tee<OrderBook<>> tee(*opt, ref);
  parse(buf.data(), buf.size(), tee);
  for (std::uint16_t l = 1; l <= 3; ++l) RC_ASSERT(test::diff_symbol(*opt, ref, l, true).empty());
  RC_ASSERT(test::same_integrity(opt->stats(), ref.stats()));
  RC_ASSERT(opt->live_orders() == ref.live_orders());
}

RC_GTEST_PROP(Property, HybridEqualsReference, (const std::vector<Op>& ops)) {
  using B = OrderBook<NullListener, LevelSearch::Hybrid>;
  const auto buf = encode_ops(ops, 2);
  auto opt = std::make_unique<B>(BookConfig{8, 4, 1});
  ReferenceBook ref;
  test::Tee<B> tee(*opt, ref);
  parse(buf.data(), buf.size(), tee);
  for (std::uint16_t l = 1; l <= 2; ++l) RC_ASSERT(test::diff_symbol(*opt, ref, l, true).empty());
}

RC_GTEST_PROP(Property, LevelAggregatesEqualSumOfOrders, (const std::vector<Op>& ops)) {
  const auto buf = encode_ops(ops, 2);
  auto b = std::make_unique<OrderBook<NullListener, LevelSearch::Binary>>(BookConfig{8, 4, 1});
  parse(buf.data(), buf.size(), *b);
  std::size_t orders = 0;
  for (std::uint16_t l = 1; l <= 2; ++l)
    for (Side s : {Side::Buy, Side::Sell})
      for (const LevelInfo& lv : test::full_depth(*b, l, s)) {
        std::uint64_t sum = 0;
        std::uint32_t n = 0;
        b->for_each_order_at(l, s, lv.price, [&](std::uint64_t, std::uint32_t sh) {
          sum += sh;
          ++n;
          RC_ASSERT(sh > 0u);
        });
        RC_ASSERT(sum == lv.shares);
        RC_ASSERT(n == lv.count);
        RC_ASSERT(n > 0u);  // no empty levels linger
        orders += n;
      }
  RC_ASSERT(orders == b->live_orders());
}

RC_GTEST_PROP(Property, AddOrderRoundTrip,
              (std::uint16_t locate, std::uint64_t ref, bool sell, std::uint32_t shares,
               std::uint32_t price)) {
  const std::uint64_t ts = *rc::gen::inRange<std::uint64_t>(0, 1ull << 48);
  std::vector<std::uint8_t> buf;
  Encoder(buf).add(locate, ts, ref, sell ? Side::Sell : Side::Buy, shares, "RT", price);
  struct H : HandlerBase {
    AddOrder got{};
    void on_add(const AddOrder& m) { got = m; }
  } h;
  RC_ASSERT(parse(buf.data(), buf.size(), h).consumed == buf.size());
  RC_ASSERT(h.got.h.locate == locate);
  RC_ASSERT(h.got.h.timestamp == ts);
  RC_ASSERT(h.got.ref == ref);
  RC_ASSERT(h.got.side == (sell ? Side::Sell : Side::Buy));
  RC_ASSERT(h.got.shares == shares);
  RC_ASSERT(h.got.price == price);
}
