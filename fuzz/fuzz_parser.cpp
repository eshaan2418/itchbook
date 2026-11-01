// libFuzzer harness: arbitrary bytes through the framed parser into both books. Must never crash
// or trip ASan/UBSan, and the two books must agree on whatever they were fed.
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "itchbook/order_book.hpp"
#include "itchbook/parser.hpp"
#include "itchbook/reference_book.hpp"

using namespace itchbook;

namespace {
struct Tee : HandlerBase {
  OrderBook<>& a;
  ReferenceBook& b;
  Tee(OrderBook<>& x, ReferenceBook& y) : a(x), b(y) {}
#define T(fn, M)          \
  void fn(const M& m) {   \
    a.fn(m);              \
    b.fn(m);              \
  }
  T(on_add, AddOrder)
  T(on_executed, OrderExecuted)
  T(on_executed_price, OrderExecutedPrice)
  T(on_cancel, OrderCancel)
  T(on_delete, OrderDelete)
  T(on_replace, OrderReplace)
#undef T
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Tiny initial capacities so pool / map / side-vector growth paths are fuzzed too.
  auto opt = std::make_unique<OrderBook<>>(BookConfig{64, 16, 1});
  ReferenceBook ref;
  Tee tee(*opt, ref);
  parse(data, size, tee);
  if (opt->live_orders() != ref.live_orders()) std::abort();
  const auto& s = opt->stats();
  const auto& r = ref.stats();
  if (s.unknown_ref != r.unknown_ref || s.duplicate_ref != r.duplicate_ref || s.overfill != r.overfill)
    std::abort();
  return 0;
}
