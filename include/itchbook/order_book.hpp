// Optimised full-depth (L3) order book for every symbol on an ITCH feed.
//
// Data layout
//   orders_   pool of Order records (32 B, 4 per 128 B line), linked FIFO per price level by
//             32-bit indices (stable across pool growth, half the size of pointers).
//   levels_   pool of Level records (aggregate shares, count, FIFO head/tail).
//   map_      OrderMap: order ref -> orders_ index (open addressing, see order_map.hpp).
//   books_    one Book per stock locate (direct index, locate is a u16 assigned by NASDAQ).
//   Book side: contiguous vector of {price, level index} sorted worst -> best, so the best price
//             is at back(): the common operations (add/remove near the touch) touch the end of
//             a small array and never shift more than a few entries.
//
// Hot path: no heap allocation once pools/maps/side vectors reach their working-set capacity
// (verified by tests/test_alloc.cpp), no exceptions, no virtual dispatch. The listener is a
// template parameter, so BBO callbacks inline.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "itchbook/book_types.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/order_map.hpp"
#include "itchbook/parser.hpp"

namespace itchbook {

// Linear: scan from the touch. Binary: partition_point over the whole side. Hybrid: scan the
// kHybridScan levels nearest the touch, then binary-search the rest (deep books).
enum class LevelSearch : std::uint8_t { Linear, Binary, Hybrid };
inline constexpr std::size_t kHybridScan = 8;

struct NullListener {
  void on_bbo(std::uint16_t /*locate*/, const Bbo& /*bbo*/, std::uint64_t /*ts*/) noexcept {}
};

struct BookConfig {
  std::size_t order_capacity = 1u << 22;  // live orders across all symbols
  std::size_t level_capacity = 1u << 20;  // live price levels across all symbols
  std::size_t side_reserve = 256;         // initial per-side level array capacity (PERF_LOG)
};

template <class Listener = NullListener, LevelSearch Search = LevelSearch::Linear>
class OrderBook : public HandlerBase {
 public:
  static constexpr std::uint32_t kNil = ~std::uint32_t{0};

  explicit OrderBook(const BookConfig& cfg = {}, Listener listener = {})
      : listener_(listener), cfg_(cfg), map_(cfg.order_capacity * 2) {
    orders_.reserve(cfg.order_capacity);
    levels_.reserve(cfg.level_capacity);
    free_orders_.reserve(cfg.order_capacity);
    free_levels_.reserve(cfg.level_capacity);
  }

  // ---- ITCH handler interface (called by itchbook::parse / dispatch) ---------------------------
  void on_stock_directory(const StockDirectory& m) noexcept {
    Book& b = books_[m.h.locate];
    std::memcpy(b.symbol, m.stock, 8);
    ensure_reserved(b);
  }

  void on_add(const AddOrder& m) noexcept {
    ++stats_.adds;
    add_order(m.h.locate, m.ref, m.side, m.shares, m.price, m.h.timestamp);
  }

  void on_executed(const OrderExecuted& m) noexcept {
    ++stats_.executes;
    reduce(m.ref, m.shares, m.h.timestamp);
  }
  void on_executed_price(const OrderExecutedPrice& m) noexcept {
    ++stats_.executes;
    reduce(m.ref, m.shares, m.h.timestamp);
  }
  void on_cancel(const OrderCancel& m) noexcept {
    ++stats_.cancels;
    reduce(m.ref, m.shares, m.h.timestamp);
  }
  void on_delete(const OrderDelete& m) noexcept {
    ++stats_.deletes;
    const std::uint32_t oi = map_.erase(m.ref);
    if (oi == OrderMap::kNotFound) [[unlikely]] {
      ++stats_.unknown_ref;
      return;
    }
    const std::uint16_t locate = orders_[oi].locate;
    remove_order(oi);
    publish(locate, m.h.timestamp);
  }
  void on_replace(const OrderReplace& m) noexcept {
    ++stats_.replaces;
    const std::uint32_t oi = map_.erase(m.old_ref);
    if (oi == OrderMap::kNotFound) [[unlikely]] {
      ++stats_.unknown_ref;
      return;
    }
    const Order o = orders_[oi];
    remove_order(oi);
    // Replace = cancel + new order at the back of the queue (priority lost), same side/symbol.
    add_order_nopublish(o.locate, m.new_ref, static_cast<Side>(o.side), m.shares, m.price);
    publish(o.locate, m.h.timestamp);
  }

  // ---- Queries ----------------------------------------------------------------------------------
  [[nodiscard]] Bbo bbo(std::uint16_t locate) const noexcept {
    const Book& b = books_[locate];
    Bbo r{};
    if (!b.bids.empty()) {
      const Level& l = levels_[b.bids.back().level];
      r.bid_px = l.price;
      r.bid_qty = l.shares;
    }
    if (!b.asks.empty()) {
      const Level& l = levels_[b.asks.back().level];
      r.ask_px = l.price;
      r.ask_qty = l.shares;
    }
    return r;
  }

  // Up to `n` best levels of one side, best first. Returns the number written.
  std::size_t depth(std::uint16_t locate, Side side, LevelInfo* out, std::size_t n) const noexcept {
    const auto& v = side == Side::Buy ? books_[locate].bids : books_[locate].asks;
    std::size_t k = 0;
    for (auto it = v.rbegin(); it != v.rend() && k < n; ++it, ++k) {
      const Level& l = levels_[it->level];
      out[k] = LevelInfo{l.price, l.shares, l.count};
    }
    return k;
  }

  [[nodiscard]] std::size_t level_count(std::uint16_t locate, Side side) const noexcept {
    return side == Side::Buy ? books_[locate].bids.size() : books_[locate].asks.size();
  }

  // Order refs at one price level in FIFO (time-priority) order; test/debug helper.
  template <class F>
  void for_each_order_at(std::uint16_t locate, Side side, std::uint32_t price, F&& f) const {
    const auto& v = side == Side::Buy ? books_[locate].bids : books_[locate].asks;
    for (const auto& e : v)
      if (e.price == price)
        for (std::uint32_t i = levels_[e.level].head; i != kNil; i = orders_[i].next)
          f(orders_[i].ref, orders_[i].shares);
  }

  [[nodiscard]] bool crossed(std::uint16_t locate) const noexcept {
    const Book& b = books_[locate];
    return !b.bids.empty() && !b.asks.empty() &&
           levels_[b.bids.back().level].price >= levels_[b.asks.back().level].price;
  }

  [[nodiscard]] const char* symbol(std::uint16_t locate) const noexcept {
    return books_[locate].symbol;
  }
  [[nodiscard]] const BookStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t live_orders() const noexcept { return map_.size(); }
  [[nodiscard]] std::size_t live_levels() const noexcept {
    return levels_.size() - free_levels_.size();
  }
  [[nodiscard]] std::size_t map_rehashes() const noexcept { return map_.rehashes(); }
  Listener& listener() noexcept { return listener_; }

  void prefetch_ref(std::uint64_t ref) const noexcept { map_.prefetch(ref); }

  // Two-stage software pipeline over the upcoming message stream (see docs/PERF_LOG.md).
  // Stage 1 (far ahead): hash slot(s) of the message's order ref(s) and the symbol's Book header.
  // Stage 2 (nearer): with the slot now cached, resolve ref -> order record and prefetch it; for
  // adds, prefetch the best end of the target side's level array. Prefetches are hints only:
  // a ref that is created or removed in between simply wastes one prefetch.
  void prefetch_stage1(const std::uint8_t* m) const noexcept {
    switch (m[0]) {
      case 'A': case 'F':
        map_.prefetch(be::u64(m + 11));
        __builtin_prefetch(&books_[be::u16(m + 1)], 0, 3);
        break;
      case 'U':
        map_.prefetch(be::u64(m + 11));
        map_.prefetch(be::u64(m + 19));
        break;
      case 'E': case 'C': case 'X': case 'D':
        map_.prefetch(be::u64(m + 11));
        break;
      default: break;
    }
  }
  void prefetch_stage2(const std::uint8_t* m) const noexcept {
    switch (m[0]) {
      case 'A': case 'F': {
        const Book& b = books_[be::u16(m + 1)];
        const auto& v = m[19] == 'S' ? b.asks : b.bids;
        if (!v.empty()) __builtin_prefetch(v.data() + v.size() - 1, 0, 3);
        break;
      }
      case 'E': case 'C': case 'X': case 'D': case 'U': {
        const std::uint32_t oi = map_.find(be::u64(m + 11));
        if (oi != OrderMap::kNotFound) __builtin_prefetch(&orders_[oi], 1, 3);
        break;
      }
      default: break;
    }
  }
  // Stage 3 (nearest): the order record is cached by now; prefetch its price level and the
  // symbol's Book header so remove_order() / publish() hit cache.
  void prefetch_stage3(const std::uint8_t* m) const noexcept {
    switch (m[0]) {
      case 'E': case 'C': case 'X': case 'D': case 'U': {
        const std::uint32_t oi = map_.find(be::u64(m + 11));
        if (oi == OrderMap::kNotFound) break;
        const Order& o = orders_[oi];
        __builtin_prefetch(&levels_[o.level], 1, 3);
        __builtin_prefetch(&books_[o.locate], 0, 3);
        break;
      }
      default: break;
    }
  }

 private:
  struct Order {
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint32_t level;
    std::uint32_t prev;
    std::uint32_t next;
    std::uint16_t locate;
    std::uint8_t side;
  };
  static_assert(sizeof(Order) == 32);

  struct Level {
    std::uint64_t shares;
    std::uint32_t price;
    std::uint32_t count;
    std::uint32_t head;
    std::uint32_t tail;
  };
  static_assert(sizeof(Level) == 24);

  struct LevelRef {
    std::uint32_t price;
    std::uint32_t level;
  };

  struct Book {
    std::vector<LevelRef> bids;  // ascending price: best bid at back
    std::vector<LevelRef> asks;  // descending price: best ask at back
    Bbo last{};
    char symbol[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    bool reserved = false;
  };

  // "a is a better price than b" for the side.
  static bool better(Side s, std::uint32_t a, std::uint32_t b) noexcept {
    return s == Side::Buy ? a > b : a < b;
  }

  void ensure_reserved(Book& b) {
    if (!b.reserved) [[unlikely]] {
      b.bids.reserve(cfg_.side_reserve);
      b.asks.reserve(cfg_.side_reserve);
      b.reserved = true;
    }
  }

  // Locate the level for `price`, creating it if absent. Returns the level index.
  std::uint32_t find_or_create_level(std::vector<LevelRef>& v, Side s, std::uint32_t price) {
    std::size_t pos;
    if constexpr (Search == LevelSearch::Linear) {
      // Scan from the best price downward; most adds land within a few ticks of the touch.
      pos = v.size();
      while (pos > 0 && better(s, v[pos - 1].price, price)) --pos;
      if (pos > 0 && v[pos - 1].price == price) return v[pos - 1].level;
    } else {
      // v is sorted worst -> best; find first entry that is better than price.
      std::size_t hi = v.size();
      bool scanned = false;
      if constexpr (Search == LevelSearch::Hybrid) {
        const std::size_t stop = hi > kHybridScan ? hi - kHybridScan : 0;
        while (hi > stop && better(s, v[hi - 1].price, price)) --hi;
        scanned = hi > stop || hi == 0;  // slot lies within the scanned window
      }
      if (scanned) {
        pos = hi;
      } else {
        auto it = std::partition_point(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(hi),
                                       [&](const LevelRef& e) { return !better(s, e.price, price); });
        pos = static_cast<std::size_t>(it - v.begin());
      }
      if (pos > 0 && v[pos - 1].price == price) return v[pos - 1].level;
    }
    const std::uint32_t li = alloc_level(price);
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), LevelRef{price, li});
    return li;
  }

  void erase_level(std::vector<LevelRef>& v, Side s, std::uint32_t price, std::uint32_t li) {
    std::size_t pos = 0;
    if constexpr (Search == LevelSearch::Linear) {
      pos = v.size();
      while (pos > 0 && v[pos - 1].level != li) --pos;
      pos -= 1;
    } else {
      std::size_t hi = v.size();
      bool found = false;
      if constexpr (Search == LevelSearch::Hybrid) {
        const std::size_t stop = hi > kHybridScan ? hi - kHybridScan : 0;
        for (std::size_t j = hi; j > stop; --j)
          if (v[j - 1].level == li) {
            pos = j - 1;
            found = true;
            break;
          }
        hi = stop;
      }
      if (!found) {
        auto it = std::partition_point(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(hi),
                                       [&](const LevelRef& e) {
                                         return !better(s, e.price, price) && e.price != price;
                                       });
        pos = static_cast<std::size_t>(it - v.begin());
      }
    }
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos));
    free_levels_.push_back(li);
  }

  std::uint32_t alloc_level(std::uint32_t price) {
    std::uint32_t li;
    if (!free_levels_.empty()) {
      li = free_levels_.back();
      free_levels_.pop_back();
    } else {
      li = static_cast<std::uint32_t>(levels_.size());
      levels_.emplace_back();
    }
    levels_[li] = Level{0, price, 0, kNil, kNil};
    return li;
  }

  std::uint32_t alloc_order() {
    if (!free_orders_.empty()) {
      const std::uint32_t oi = free_orders_.back();
      free_orders_.pop_back();
      return oi;
    }
    orders_.emplace_back();
    return static_cast<std::uint32_t>(orders_.size() - 1);
  }

  void add_order(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares,
                 std::uint32_t price, std::uint64_t ts) {
    if (add_order_nopublish(locate, ref, side, shares, price)) publish(locate, ts);
  }

  bool add_order_nopublish(std::uint16_t locate, std::uint64_t ref, Side side,
                           std::uint32_t shares, std::uint32_t price) {
    const std::uint32_t oi = alloc_order();
    if (!map_.insert(ref, oi)) [[unlikely]] {
      ++stats_.duplicate_ref;
      free_orders_.push_back(oi);
      return false;
    }
    Book& b = books_[locate];
    ensure_reserved(b);
    auto& v = side == Side::Buy ? b.bids : b.asks;
    const std::uint32_t li = find_or_create_level(v, side, price);
    Level& l = levels_[li];
    Order& o = orders_[oi];
    o = Order{ref, shares, price, li, l.tail, kNil, locate, static_cast<std::uint8_t>(side)};
    if (l.tail != kNil) orders_[l.tail].next = oi;
    else l.head = oi;
    l.tail = oi;
    l.shares += shares;
    ++l.count;
    return true;
  }

  // Execute or cancel `shares` from an order; removes it when it reaches zero.
  void reduce(std::uint64_t ref, std::uint32_t shares, std::uint64_t ts) {
    const std::uint32_t oi = map_.find(ref);
    if (oi == OrderMap::kNotFound) [[unlikely]] {
      ++stats_.unknown_ref;
      return;
    }
    Order& o = orders_[oi];
    const std::uint16_t locate = o.locate;
    if (shares >= o.shares) [[unlikely]] {
      if (shares > o.shares) ++stats_.overfill;
      map_.erase(ref);
      remove_order(oi);
    } else {
      o.shares -= shares;
      levels_[o.level].shares -= shares;
    }
    publish(locate, ts);
  }

  void remove_order(std::uint32_t oi) {
    const Order& o = orders_[oi];
    Level& l = levels_[o.level];
    if (o.prev != kNil) orders_[o.prev].next = o.next;
    else l.head = o.next;
    if (o.next != kNil) orders_[o.next].prev = o.prev;
    else l.tail = o.prev;
    l.shares -= o.shares;
    if (--l.count == 0) {
      Book& b = books_[o.locate];
      const Side s = static_cast<Side>(o.side);
      erase_level(s == Side::Buy ? b.bids : b.asks, s, o.price, o.level);
    }
    free_orders_.push_back(oi);
  }

  void publish(std::uint16_t locate, std::uint64_t ts) {
    Book& b = books_[locate];
    const Bbo now = bbo(locate);
    if (!(now == b.last)) {
      b.last = now;
      ++stats_.bbo_updates;
      listener_.on_bbo(locate, now, ts);
    }
  }

  Listener listener_;
  BookConfig cfg_;
  OrderMap map_;
  std::vector<Order> orders_;
  std::vector<Level> levels_;
  std::vector<std::uint32_t> free_orders_;
  std::vector<std::uint32_t> free_levels_;
  std::vector<Book> books_ = std::vector<Book>(65536);  // heap: ~5 MB, indexed by locate
  BookStats stats_{};
};

}  // namespace itchbook
