#include <gtest/gtest.h>

#include <vector>

#include "itchbook/parser.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

namespace {
struct Capture : HandlerBase {
  std::vector<AddOrder> adds;
  std::vector<OrderExecuted> execs;
  std::vector<OrderExecutedPrice> execps;
  std::vector<OrderCancel> cancels;
  std::vector<OrderDelete> dels;
  std::vector<OrderReplace> reps;
  std::vector<Trade> trades;
  std::vector<StockDirectory> dirs;
  std::vector<SystemEvent> sys;
  int errors_unknown = 0, errors_len = 0;
  void on_add(const AddOrder& m) { adds.push_back(m); }
  void on_executed(const OrderExecuted& m) { execs.push_back(m); }
  void on_executed_price(const OrderExecutedPrice& m) { execps.push_back(m); }
  void on_cancel(const OrderCancel& m) { cancels.push_back(m); }
  void on_delete(const OrderDelete& m) { dels.push_back(m); }
  void on_replace(const OrderReplace& m) { reps.push_back(m); }
  void on_trade(const Trade& m) { trades.push_back(m); }
  void on_stock_directory(const StockDirectory& m) { dirs.push_back(m); }
  void on_system_event(const SystemEvent& m) { sys.push_back(m); }
  void on_error(ParseError e, std::uint8_t, std::uint16_t) {
    (e == ParseError::UnknownType ? errors_unknown : errors_len)++;
  }
};
}  // namespace

TEST(Endian, DecodesBigEndian) {
  const std::uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  EXPECT_EQ(be::u16(b), 0x0102);
  EXPECT_EQ(be::u32(b), 0x01020304u);
  EXPECT_EQ(be::u64(b), 0x0102030405060708ull);
  EXPECT_EQ(be::u48(b), 0x010203040506ull);
}

TEST(Parser, RoundTripsEveryBookMessage) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  const std::uint64_t ts = 0x0000123456789ABCull;  // fits 48 bits
  e.system_event(ts, 'O');
  e.stock_directory(7, ts, "AAPL");
  e.add(7, ts, 0xDEADBEEFCAFEull, Side::Sell, 300, "AAPL", 1234500);
  e.add(7, ts, 99, Side::Buy, 100, "AAPL", 1234400, /*mpid=*/true);
  e.executed(7, ts, 99, 40, 555);
  e.executed_price(7, ts, 99, 10, 556, 1234300);
  e.cancel(7, ts, 99, 25);
  e.replace(7, ts, 99, 100, 77, 1234200);
  e.del(7, ts, 100);
  e.trade(7, ts, Side::Buy, 500, "AAPL", 1234450, 557);

  Capture c;
  const auto r = parse(buf.data(), buf.size(), c);
  EXPECT_EQ(r.consumed, buf.size());
  EXPECT_EQ(r.messages, 10u);
  ASSERT_EQ(c.sys.size(), 1u);
  EXPECT_EQ(c.sys[0].event, 'O');
  EXPECT_EQ(c.sys[0].h.timestamp, ts);
  ASSERT_EQ(c.dirs.size(), 1u);
  EXPECT_EQ(std::string(c.dirs[0].stock, 8), "AAPL    ");
  EXPECT_EQ(c.dirs[0].h.locate, 7);
  ASSERT_EQ(c.adds.size(), 2u);
  EXPECT_EQ(c.adds[0].ref, 0xDEADBEEFCAFEull);
  EXPECT_EQ(c.adds[0].side, Side::Sell);
  EXPECT_EQ(c.adds[0].shares, 300u);
  EXPECT_EQ(c.adds[0].price, 1234500u);
  EXPECT_EQ(c.adds[1].side, Side::Buy);
  ASSERT_EQ(c.execs.size(), 1u);
  EXPECT_EQ(c.execs[0].shares, 40u);
  EXPECT_EQ(c.execs[0].match, 555u);
  ASSERT_EQ(c.execps.size(), 1u);
  EXPECT_EQ(c.execps[0].price, 1234300u);
  EXPECT_EQ(c.execps[0].printable, 'Y');
  ASSERT_EQ(c.cancels.size(), 1u);
  EXPECT_EQ(c.cancels[0].shares, 25u);
  ASSERT_EQ(c.reps.size(), 1u);
  EXPECT_EQ(c.reps[0].old_ref, 99u);
  EXPECT_EQ(c.reps[0].new_ref, 100u);
  EXPECT_EQ(c.reps[0].shares, 77u);
  EXPECT_EQ(c.reps[0].price, 1234200u);
  ASSERT_EQ(c.dels.size(), 1u);
  EXPECT_EQ(c.dels[0].ref, 100u);
  ASSERT_EQ(c.trades.size(), 1u);
  EXPECT_EQ(c.trades[0].match, 557u);
  EXPECT_EQ(c.errors_unknown + c.errors_len, 0);
}

TEST(Parser, LeavesPartialFrameUnconsumed) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.del(1, 1, 5);
  e.del(1, 1, 6);
  for (std::size_t cut = 0; cut < buf.size(); ++cut) {
    Capture c;
    const auto r = parse(buf.data(), cut, c);
    const std::size_t frame = 2 + kOrderDelete;
    EXPECT_EQ(r.consumed, (cut / frame) * frame) << "cut=" << cut;
    EXPECT_EQ(c.dels.size(), cut / frame);
  }
}

TEST(Parser, ReportsAndSkipsMalformed) {
  std::vector<std::uint8_t> buf;
  Encoder e(buf);
  e.raw({'Z', 1, 2, 3});              // unknown type
  e.raw({'A', 0, 1, 0, 0, 0, 0});     // add with wrong length
  e.raw({});                          // zero-length frame
  e.del(1, 1, 42);                    // still parsed afterwards
  Capture c;
  const auto r = parse(buf.data(), buf.size(), c);
  EXPECT_EQ(r.consumed, buf.size());
  EXPECT_EQ(c.errors_unknown, 1);
  EXPECT_EQ(c.errors_len, 2);
  ASSERT_EQ(c.dels.size(), 1u);
  EXPECT_EQ(c.dels[0].ref, 42u);
}

TEST(Parser, ExpectedSizesMatchSpec) {
  // Spot-check sizes from the ITCH 5.0 specification.
  EXPECT_EQ(expected_size('A'), 36);
  EXPECT_EQ(expected_size('F'), 40);
  EXPECT_EQ(expected_size('U'), 35);
  EXPECT_EQ(expected_size('P'), 44);
  EXPECT_EQ(expected_size('I'), 50);
  EXPECT_EQ(expected_size('R'), 39);
  EXPECT_EQ(expected_size('?'), 0);
}
