// Google Benchmark micro-benchmarks on a synthetic Poisson order flow (no external data):
// optimised book (linear / binary level search) vs the std::map reference book, plus parser-only
// decode cost and the order-map against std::unordered_map.
#include <benchmark/benchmark.h>

#include <unordered_map>

#include "itchbook/order_book.hpp"
#include "itchbook/order_map.hpp"
#include "itchbook/reference_book.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

namespace {
const std::vector<std::uint8_t>& flow() {
  static const auto buf = [] {
    SynthConfig c{.seed = 99, .symbols = 64, .messages = 2'000'000};
    c.max_live_per_symbol = 4000;
    return generate_synthetic(c);
  }();
  return buf;
}

struct Count : HandlerBase {
  std::uint64_t n = 0;
  void on_add(const AddOrder& m) { n += m.shares; }
  void on_delete(const OrderDelete& m) { n += m.ref; }
  void on_replace(const OrderReplace& m) { n += m.new_ref; }
  void on_executed(const OrderExecuted& m) { n += m.shares; }
  void on_cancel(const OrderCancel& m) { n += m.shares; }
};

template <class Book>
void BM_Book(benchmark::State& st) {
  const auto& buf = flow();
  std::size_t msgs = 0;
  for (auto _ : st) {
    st.PauseTiming();
    auto b = std::make_unique<Book>();
    st.ResumeTiming();
    msgs += parse(buf.data(), buf.size(), *b).messages;
    benchmark::DoNotOptimize(b->live_orders());
    st.PauseTiming();
    b.reset();
    st.ResumeTiming();
  }
  st.counters["msgs/s"] = benchmark::Counter(static_cast<double>(msgs), benchmark::Counter::kIsRate);
  st.counters["ns/msg"] = benchmark::Counter(static_cast<double>(msgs),
                                             benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

void BM_ParseOnly(benchmark::State& st) {
  const auto& buf = flow();
  std::size_t msgs = 0;
  for (auto _ : st) {
    Count c;
    msgs += parse(buf.data(), buf.size(), c).messages;
    benchmark::DoNotOptimize(c.n);
  }
  st.counters["msgs/s"] = benchmark::Counter(static_cast<double>(msgs), benchmark::Counter::kIsRate);
  st.counters["ns/msg"] = benchmark::Counter(static_cast<double>(msgs),
                                             benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// Churn at a fixed live-set size: insert a new (increasing) ref, erase a pseudo-randomly chosen
// live ref. Mirrors ITCH, where refs are assigned in order but orders die in arbitrary order.
// Erasing in insertion order would flatter std::unordered_map (identity hash -> sequential
// buckets), so both maps get the same random erase sequence.
template <class Map>
void churn(benchmark::State& st, Map& m) {
  const std::size_t window = static_cast<std::size_t>(st.range(0));
  std::vector<std::uint64_t> live(window);
  std::uint64_t k = 1, x = 0x9E3779B97F4A7C15ull;
  for (auto& v : live) {
    v = k++;
    m.insert({v, static_cast<std::uint32_t>(v)});
  }
  for (auto _ : st) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;  // xorshift
    const std::size_t i = static_cast<std::size_t>(x % window);
    m.erase(live[i]);
    live[i] = k;
    m.insert({k, static_cast<std::uint32_t>(k)});
    ++k;
  }
  benchmark::DoNotOptimize(m.size());
}
struct OrderMapAdapter {
  OrderMap m;
  explicit OrderMapAdapter(std::size_t n) : m(n) {}
  void insert(std::pair<std::uint64_t, std::uint32_t> kv) { m.insert(kv.first, kv.second); }
  void erase(std::uint64_t key) { benchmark::DoNotOptimize(m.erase(key)); }
  std::size_t size() const { return m.size(); }
};
void BM_OrderMapChurn(benchmark::State& st) {
  OrderMapAdapter m(static_cast<std::size_t>(st.range(0)) * 4);
  churn(st, m);
}
void BM_StdUnorderedMapChurn(benchmark::State& st) {
  std::unordered_map<std::uint64_t, std::uint32_t> m;
  m.reserve(static_cast<std::size_t>(st.range(0)) * 2);
  churn(st, m);
}
}  // namespace

BENCHMARK(BM_ParseOnly)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Book, OrderBook<NullListener, LevelSearch::Linear>)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Book, OrderBook<NullListener, LevelSearch::Binary>)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Book, ReferenceBook)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_OrderMapChurn)->Arg(1 << 16)->Arg(1 << 22);
BENCHMARK(BM_StdUnorderedMapChurn)->Arg(1 << 16)->Arg(1 << 22);
BENCHMARK_MAIN();
