// Differential tests: optimised book vs reference book, compared after every message.
#include <gtest/gtest.h>

#include "compare.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

namespace {
template <class Opt>
void run_differential(const SynthConfig& cfg, bool check_queues) {
  const auto buf = generate_synthetic(cfg);
  auto opt = std::make_unique<Opt>(BookConfig{1u << 12, 1u << 10, 4});  // small: forces growth
  ReferenceBook ref;
  test::Tee<Opt> tee(*opt, ref);
  std::size_t n = 0;
  for_each_frame(buf.data(), buf.size(), [&](const std::uint8_t* m, std::uint16_t len) {
    tee.last_locate = 0;
    dispatch(m, len, tee);
    ++n;
    if (tee.last_locate) {
      const std::string d = test::diff_symbol(*opt, ref, tee.last_locate, check_queues);
      ASSERT_TRUE(d.empty()) << "after message " << n << ": " << d;
    }
  });
  for (std::uint16_t l = 1; l <= cfg.symbols; ++l)
    ASSERT_EQ(test::diff_symbol(*opt, ref, l, true), "");
  EXPECT_TRUE(test::same_integrity(opt->stats(), ref.stats()));
  EXPECT_EQ(opt->live_orders(), ref.live_orders());
}
}  // namespace

TEST(Differential, CleanFlowLinear) {
  run_differential<OrderBook<NullListener, LevelSearch::Linear>>({.seed = 1, .messages = 200000}, true);
}
TEST(Differential, CleanFlowBinary) {
  run_differential<OrderBook<NullListener, LevelSearch::Binary>>({.seed = 2, .messages = 200000}, true);
}
TEST(Differential, CleanFlowHybrid) {
  run_differential<OrderBook<NullListener, LevelSearch::Hybrid>>({.seed = 7, .messages = 200000}, true);
}
TEST(Differential, MalformedFlowCountsAgree) {
  SynthConfig cfg{.seed = 3, .messages = 200000};
  cfg.p_malformed = 0.02;
  run_differential<OrderBook<>>(cfg, true);
}
TEST(Differential, ManySymbolsShallowBooks) {
  SynthConfig cfg{.seed = 4, .symbols = 500, .messages = 300000};
  cfg.max_live_per_symbol = 50;
  run_differential<OrderBook<>>(cfg, false);
}
TEST(Differential, WideBooks) {
  SynthConfig cfg{.seed = 5, .symbols = 2, .messages = 200000};
  cfg.depth_decay = 0.02;  // levels spread over hundreds of ticks
  run_differential<OrderBook<NullListener, LevelSearch::Binary>>(cfg, false);
  run_differential<OrderBook<NullListener, LevelSearch::Linear>>(cfg, false);
  run_differential<OrderBook<NullListener, LevelSearch::Hybrid>>(cfg, false);
}
TEST(Differential, SyntheticBookNeverCrosses) {
  const auto buf = generate_synthetic({.seed = 6, .messages = 100000});
  auto opt = std::make_unique<OrderBook<>>();
  std::size_t crossed = 0;
  for_each_frame(buf.data(), buf.size(), [&](const std::uint8_t* m, std::uint16_t len) {
    dispatch(m, len, *opt);
    for (std::uint16_t l = 1; l <= 8; ++l) crossed += opt->crossed(l);
  });
  EXPECT_EQ(crossed, 0u);
  EXPECT_EQ(opt->stats().unknown_ref + opt->stats().overfill + opt->stats().duplicate_ref, 0u);
}
