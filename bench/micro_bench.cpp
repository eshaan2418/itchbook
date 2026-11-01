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

// Churn: insert ref, erase ref - window live keys, the access pattern of an ITCH day.
void BM_OrderMapChurn(benchmark::State& st) {
  const std::uint64_t window = static_cast<std::uint64_t>(st.range(0));
  OrderMap m(window * 2);
  std::uint64_t k = 1;
  for (; k <= window; ++k) m.insert(k, static_cast<std::uint32_t>(k));
  for (auto _ : st) {
    m.insert(k, static_cast<std::uint32_t>(k));
    benchmark::DoNotOptimize(m.erase(k - window));
    ++k;
  }
}
void BM_StdUnorderedMapChurn(benchmark::State& st) {
  const std::uint64_t window = static_cast<std::uint64_t>(st.range(0));
  std::unordered_map<std::uint64_t, std::uint32_t> m;
  m.reserve(window * 2);
  std::uint64_t k = 1;
  for (; k <= window; ++k) m.emplace(k, static_cast<std::uint32_t>(k));
  for (auto _ : st) {
    m.emplace(k, static_cast<std::uint32_t>(k));
    benchmark::DoNotOptimize(m.erase(k - window));
    ++k;
  }
}
}  // namespace

BENCHMARK(BM_ParseOnly)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Book, OrderBook<NullListener, LevelSearch::Linear>)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Book, OrderBook<NullListener, LevelSearch::Binary>)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Book, ReferenceBook)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_OrderMapChurn)->Arg(1 << 16)->Arg(1 << 22);
BENCHMARK(BM_StdUnorderedMapChurn)->Arg(1 << 16)->Arg(1 << 22);
BENCHMARK_MAIN();
