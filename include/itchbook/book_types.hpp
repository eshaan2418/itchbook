// Types shared by the optimised and the reference order books.
#pragma once

#include <cstdint>

namespace itchbook {

// Best bid/offer. Price 0 / qty 0 means the side is empty.
struct Bbo {
  std::uint32_t bid_px = 0;
  std::uint32_t ask_px = 0;
  std::uint64_t bid_qty = 0;
  std::uint64_t ask_qty = 0;
  friend bool operator==(const Bbo&, const Bbo&) = default;
};

struct LevelInfo {
  std::uint32_t price;
  std::uint64_t shares;
  std::uint32_t count;
  friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

// Message counts and integrity counters. On a clean, complete feed every *_ref / overfill counter
// must be zero; both book implementations apply identical policies so they stay comparable.
struct BookStats {
  std::uint64_t adds = 0;
  std::uint64_t executes = 0;
  std::uint64_t cancels = 0;
  std::uint64_t deletes = 0;
  std::uint64_t replaces = 0;
  std::uint64_t bbo_updates = 0;
  std::uint64_t unknown_ref = 0;    // execute/cancel/delete/replace of a ref not in the book
  std::uint64_t duplicate_ref = 0;  // add (or replace's new ref) of a ref already live: ignored
  std::uint64_t overfill = 0;       // executed/cancelled shares > remaining: order removed
  friend bool operator==(const BookStats&, const BookStats&) = default;
};

}  // namespace itchbook
