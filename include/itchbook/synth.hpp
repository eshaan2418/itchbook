// ITCH 5.0 encoder + synthetic order-flow generator.
//
// The encoder writes framed ([u16 len][msg]) big-endian messages and is the inverse of the
// decoders in messages.hpp (round-trip tested). The generator produces a reproducible, valid
// ITCH stream for tests, CI fixtures and micro-benchmarks: Poisson arrivals over a set of
// symbols, add prices drawn at a geometric distance from the touch (so books have a realistic
// dense-near-touch / sparse-tail shape), and a mix of executes, partial cancels, deletes and
// replaces against live orders. Optionally injects malformed traffic (unknown refs, overfills,
// duplicate adds) to exercise the integrity counters.
#pragma once

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "itchbook/messages.hpp"

namespace itchbook {

class Encoder {
 public:
  explicit Encoder(std::vector<std::uint8_t>& out) : out_(out) {}

  void system_event(std::uint64_t ts, char code) {
    begin('S', kSystemEvent, 0, ts);
    u8(static_cast<std::uint8_t>(code));
  }
  void stock_directory(std::uint16_t locate, std::uint64_t ts, const std::string& sym) {
    begin('R', kStockDirectory, locate, ts);
    stock(sym);
    u8('Q');  // market category
    u8('N');  // financial status
    u32(100);  // round lot
    u8('N');
    u8('C');
    u8('Z');
    u8(' ');
    u8('P');
    u8('N');
    u8('N');
    u8('1');
    u8('N');
    u32(0);
    u8('N');
  }
  void add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, Side side,
           std::uint32_t shares, const std::string& sym, std::uint32_t price, bool mpid = false) {
    begin(mpid ? 'F' : 'A', mpid ? kAddOrderMpid : kAddOrder, locate, ts);
    u64(ref);
    u8(side == Side::Buy ? 'B' : 'S');
    u32(shares);
    stock(sym);
    u32(price);
    if (mpid) {
      for (char c : std::string("ABCD")) u8(static_cast<std::uint8_t>(c));
    }
  }
  void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                std::uint64_t match) {
    begin('E', kOrderExecuted, locate, ts);
    u64(ref);
    u32(shares);
    u64(match);
  }
  void executed_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                      std::uint32_t shares, std::uint64_t match, std::uint32_t price) {
    begin('C', kOrderExecutedPrice, locate, ts);
    u64(ref);
    u32(shares);
    u64(match);
    u8('Y');
    u32(price);
  }
  void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    begin('X', kOrderCancel, locate, ts);
    u64(ref);
    u32(shares);
  }
  void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    begin('D', kOrderDelete, locate, ts);
    u64(ref);
  }
  void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref,
               std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
    begin('U', kOrderReplace, locate, ts);
    u64(old_ref);
    u64(new_ref);
    u32(shares);
    u32(price);
  }
  void trade(std::uint16_t locate, std::uint64_t ts, Side side, std::uint32_t shares,
             const std::string& sym, std::uint32_t price, std::uint64_t match) {
    begin('P', kTrade, locate, ts);
    u64(0);
    u8(side == Side::Buy ? 'B' : 'S');
    u32(shares);
    stock(sym);
    u32(price);
    u64(match);
  }
  // Raw framed bytes (for malformed-input tests).
  void raw(const std::vector<std::uint8_t>& msg) {
    u16(static_cast<std::uint16_t>(msg.size()));
    out_.insert(out_.end(), msg.begin(), msg.end());
  }

 private:
  void begin(char type, std::uint16_t size, std::uint16_t locate, std::uint64_t ts) {
    u16(size);
    u8(static_cast<std::uint8_t>(type));
    u16(locate);
    u16(0);  // tracking number
    u16(static_cast<std::uint16_t>(ts >> 32));
    u32(static_cast<std::uint32_t>(ts));
  }
  void u8(std::uint8_t v) { out_.push_back(v); }
  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v >> 8));
    u8(static_cast<std::uint8_t>(v));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v >> 16));
    u16(static_cast<std::uint16_t>(v));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v >> 32));
    u32(static_cast<std::uint32_t>(v));
  }
  void stock(const std::string& s) {
    char buf[8];
    std::memset(buf, ' ', 8);
    std::memcpy(buf, s.data(), s.size() < 8 ? s.size() : 8);
    for (char c : buf) u8(static_cast<std::uint8_t>(c));
  }
  std::vector<std::uint8_t>& out_;
};

struct SynthConfig {
  std::uint64_t seed = 42;
  std::size_t symbols = 8;
  std::size_t messages = 100000;
  std::uint32_t mid_price = 1000000;  // $100.0000
  std::uint32_t tick = 100;           // $0.01
  double p_add = 0.48, p_delete = 0.22, p_cancel = 0.10, p_execute = 0.10, p_replace = 0.10;
  double depth_decay = 0.35;  // geometric parameter for distance from touch (in ticks)
  double p_malformed = 0.0;   // probability of injecting an integrity violation
  std::size_t max_live_per_symbol = 5000;
};

// Generates a framed ITCH byte stream. Deterministic for a given config.
std::vector<std::uint8_t> generate_synthetic(const SynthConfig& cfg);

}  // namespace itchbook
