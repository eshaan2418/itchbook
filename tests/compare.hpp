// Helpers comparing the optimised book against the reference book.
#pragma once

#include <string>
#include <vector>

#include "itchbook/order_book.hpp"
#include "itchbook/reference_book.hpp"

namespace itchbook::test {

template <class Book>
std::vector<LevelInfo> full_depth(const Book& b, std::uint16_t locate, Side s) {
  std::vector<LevelInfo> v(b.level_count(locate, s) + 1);
  v.resize(b.depth(locate, s, v.data(), v.size()));
  return v;
}

// Returns an empty string if equal, else a description of the first difference.
template <class Book>
std::string diff_symbol(const Book& opt, const ReferenceBook& ref, std::uint16_t locate,
                        bool check_queues) {
  for (Side s : {Side::Buy, Side::Sell}) {
    const auto a = full_depth(opt, locate, s);
    const auto b = ref.depth(locate, s);
    if (a.size() != b.size())
      return "locate " + std::to_string(locate) + " side " + std::to_string(int(s)) +
             " level count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!(a[i] == b[i]))
        return "locate " + std::to_string(locate) + " level " + std::to_string(i) + " px " +
               std::to_string(a[i].price) + "/" + std::to_string(b[i].price) + " sh " +
               std::to_string(a[i].shares) + "/" + std::to_string(b[i].shares) + " n " +
               std::to_string(a[i].count) + "/" + std::to_string(b[i].count);
      if (check_queues) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> q;
        opt.for_each_order_at(locate, s, a[i].price,
                              [&](std::uint64_t r, std::uint32_t sh) { q.emplace_back(r, sh); });
        if (q != ref.queue(locate, s, a[i].price))
          return "locate " + std::to_string(locate) + " FIFO queue differs at px " +
                 std::to_string(a[i].price);
      }
    }
  }
  if (!(opt.bbo(locate) == ref.bbo(locate))) return "bbo differs";
  return {};
}

inline bool same_integrity(const BookStats& a, const BookStats& b) {
  return a.adds == b.adds && a.executes == b.executes && a.cancels == b.cancels &&
         a.deletes == b.deletes && a.replaces == b.replaces && a.unknown_ref == b.unknown_ref &&
         a.duplicate_ref == b.duplicate_ref && a.overfill == b.overfill;
}

// Forwards every message to both books and remembers the locate it touched.
template <class Opt>
struct Tee : HandlerBase {
  Opt& opt;
  ReferenceBook& ref;
  std::uint16_t last_locate = 0;
  Tee(Opt& o, ReferenceBook& r) : opt(o), ref(r) {}
  void on_stock_directory(const StockDirectory& m) { opt.on_stock_directory(m); }
#define ITCHBOOK_TEE(fn, T)        \
  void fn(const T& m) {            \
    last_locate = m.h.locate;      \
    opt.fn(m);                     \
    ref.fn(m);                     \
  }
  ITCHBOOK_TEE(on_add, AddOrder)
  ITCHBOOK_TEE(on_executed, OrderExecuted)
  ITCHBOOK_TEE(on_executed_price, OrderExecutedPrice)
  ITCHBOOK_TEE(on_cancel, OrderCancel)
  ITCHBOOK_TEE(on_delete, OrderDelete)
  ITCHBOOK_TEE(on_replace, OrderReplace)
#undef ITCHBOOK_TEE
};

}  // namespace itchbook::test
