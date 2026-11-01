// Reference L3 book: the obviously-correct, deliberately unoptimised model used as the oracle in
// differential tests and as the performance baseline. std::map per side (price-ordered),
// std::list per level (FIFO), std::unordered_map from ref to (locate, side, price, iterator).
// Integrity policies are identical to OrderBook so stats and books are directly comparable.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "itchbook/book_types.hpp"
#include "itchbook/messages.hpp"
#include "itchbook/parser.hpp"

namespace itchbook {

class ReferenceBook : public HandlerBase {
 public:
  void on_add(const AddOrder& m) {
    ++stats_.adds;
    add(m.h.locate, m.ref, m.side, m.shares, m.price);
  }
  void on_executed(const OrderExecuted& m) {
    ++stats_.executes;
    reduce(m.ref, m.shares);
  }
  void on_executed_price(const OrderExecutedPrice& m) {
    ++stats_.executes;
    reduce(m.ref, m.shares);
  }
  void on_cancel(const OrderCancel& m) {
    ++stats_.cancels;
    reduce(m.ref, m.shares);
  }
  void on_delete(const OrderDelete& m) {
    ++stats_.deletes;
    auto it = index_.find(m.ref);
    if (it == index_.end()) {
      ++stats_.unknown_ref;
      return;
    }
    erase(it);
  }
  void on_replace(const OrderReplace& m) {
    ++stats_.replaces;
    auto it = index_.find(m.old_ref);
    if (it == index_.end()) {
      ++stats_.unknown_ref;
      return;
    }
    const Loc loc = it->second;
    erase(it);
    add(loc.locate, m.new_ref, loc.side, m.shares, m.price);
  }

  [[nodiscard]] Bbo bbo(std::uint16_t locate) const {
    Bbo r{};
    auto bit = books_.find(locate);
    if (bit == books_.end()) return r;
    const Book& b = bit->second;
    if (!b.bids.empty()) {
      r.bid_px = b.bids.begin()->first;
      r.bid_qty = total(b.bids.begin()->second);
    }
    if (!b.asks.empty()) {
      r.ask_px = b.asks.begin()->first;
      r.ask_qty = total(b.asks.begin()->second);
    }
    return r;
  }

  // Up to max_levels best levels of one side, best first (default: full depth).
  [[nodiscard]] std::vector<LevelInfo> depth(std::uint16_t locate, Side side,
                                             std::size_t max_levels = ~std::size_t{0}) const {
    std::vector<LevelInfo> out;
    auto bit = books_.find(locate);
    if (bit == books_.end()) return out;
    auto emit = [&](const auto& m) {
      for (const auto& [px, q] : m) {
        if (out.size() >= max_levels) break;
        out.push_back(LevelInfo{px, total(q), static_cast<std::uint32_t>(q.size())});
      }
    };
    if (side == Side::Buy) emit(bit->second.bids);
    else emit(bit->second.asks);
    return out;
  }

  // FIFO queue at one level: (ref, shares).
  [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint32_t>> queue(
      std::uint16_t locate, Side side, std::uint32_t price) const {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> out;
    auto bit = books_.find(locate);
    if (bit == books_.end()) return out;
    const Queue* q = nullptr;
    if (side == Side::Buy) {
      auto it = bit->second.bids.find(price);
      if (it != bit->second.bids.end()) q = &it->second;
    } else {
      auto it = bit->second.asks.find(price);
      if (it != bit->second.asks.end()) q = &it->second;
    }
    if (q)
      for (const auto& o : *q) out.emplace_back(o.ref, o.shares);
    return out;
  }

  [[nodiscard]] std::size_t level_count(std::uint16_t locate, Side side) const {
    auto bit = books_.find(locate);
    if (bit == books_.end()) return 0;
    return side == Side::Buy ? bit->second.bids.size() : bit->second.asks.size();
  }

  [[nodiscard]] const BookStats& stats() const { return stats_; }
  [[nodiscard]] std::size_t live_orders() const { return index_.size(); }

 private:
  struct RefOrder {
    std::uint64_t ref;
    std::uint32_t shares;
  };
  using Queue = std::list<RefOrder>;
  struct Book {
    std::map<std::uint32_t, Queue, std::greater<>> bids;  // best (highest) first
    std::map<std::uint32_t, Queue, std::less<>> asks;     // best (lowest) first
  };
  struct Loc {
    std::uint16_t locate;
    Side side;
    std::uint32_t price;
    Queue::iterator it;
  };

  static std::uint64_t total(const Queue& q) {
    std::uint64_t s = 0;
    for (const auto& o : q) s += o.shares;
    return s;
  }

  Queue& queue_for(std::uint16_t locate, Side side, std::uint32_t price) {
    Book& b = books_[locate];
    return side == Side::Buy ? b.bids[price] : b.asks[price];
  }

  void add(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares,
           std::uint32_t price) {
    if (index_.count(ref)) {
      ++stats_.duplicate_ref;
      return;
    }
    Queue& q = queue_for(locate, side, price);
    q.push_back(RefOrder{ref, shares});
    index_.emplace(ref, Loc{locate, side, price, std::prev(q.end())});
  }

  void reduce(std::uint64_t ref, std::uint32_t shares) {
    auto it = index_.find(ref);
    if (it == index_.end()) {
      ++stats_.unknown_ref;
      return;
    }
    RefOrder& o = *it->second.it;
    if (shares >= o.shares) {
      if (shares > o.shares) ++stats_.overfill;
      erase(it);
    } else {
      o.shares -= shares;
    }
  }

  void erase(std::unordered_map<std::uint64_t, Loc>::iterator it) {
    const Loc loc = it->second;
    Book& b = books_[loc.locate];
    if (loc.side == Side::Buy) {
      auto lv = b.bids.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) b.bids.erase(lv);
    } else {
      auto lv = b.asks.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) b.asks.erase(lv);
    }
    index_.erase(it);
  }

  std::unordered_map<std::uint16_t, Book> books_;
  std::unordered_map<std::uint64_t, Loc> index_;
  BookStats stats_{};
};

}  // namespace itchbook
