#include "itchbook/synth.hpp"

#include <cmath>

namespace itchbook {
namespace {

// xoshiro256** seeded by splitmix64: fast and, unlike <random> distributions, produces the same
// sequence on libc++ and libstdc++.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    for (auto& w : s_) {
      seed += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      w = z ^ (z >> 31);
    }
  }
  std::uint64_t next() {
    const std::uint64_t r = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return r;
  }
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  std::uint64_t below(std::uint64_t n) {
    __extension__ typedef unsigned __int128 u128;
    return static_cast<std::uint64_t>((static_cast<u128>(next()) * n) >> 64);
  }
  // Number of failures before first success, success probability p.
  std::uint32_t geometric(double p) {
    const double u = 1.0 - uniform();  // (0, 1]
    return static_cast<std::uint32_t>(std::floor(std::log(u) / std::log1p(-p)));
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  std::uint64_t s_[4];
};

struct Live {
  std::uint64_t ref;
  Side side;
  std::uint32_t price;
  std::uint32_t shares;
};

}  // namespace

std::vector<std::uint8_t> generate_synthetic(const SynthConfig& cfg) {
  std::vector<std::uint8_t> out;
  out.reserve(cfg.messages * 32 + 1024);
  Encoder enc(out);
  Rng rng(cfg.seed);

  std::vector<std::string> syms;
  std::vector<std::vector<Live>> live(cfg.symbols);
  std::vector<std::uint32_t> mids(cfg.symbols);
  std::uint64_t ts = 4ull * 3600 * 1'000'000'000;  // 04:00:00
  std::uint64_t next_ref = 1;
  std::uint64_t match = 1;

  enc.system_event(ts, 'O');
  for (std::size_t i = 0; i < cfg.symbols; ++i) {
    std::string s = "SYM" + std::to_string(i);
    syms.push_back(s);
    // Spread mids so symbols have different price scales; always a multiple of the tick.
    mids[i] = cfg.mid_price / cfg.tick * cfg.tick + static_cast<std::uint32_t>(i) * 50 * cfg.tick;
    enc.stock_directory(static_cast<std::uint16_t>(i + 1), ts, s);
  }

  auto draw_price = [&](std::size_t sym, Side side) {
    const std::uint32_t dist = 1 + rng.geometric(cfg.depth_decay);
    const std::uint32_t off = dist * cfg.tick - cfg.tick / 2;  // strictly away from mid
    return side == Side::Buy ? mids[sym] - std::min(off, mids[sym] - cfg.tick) : mids[sym] + off;
  };
  auto draw_shares = [&]() { return static_cast<std::uint32_t>(100 * (1 + rng.geometric(0.4))); };

  std::size_t emitted = 1 + cfg.symbols;
  while (emitted < cfg.messages) {
    ts += 1 + rng.geometric(0.01);  // ~100 ns mean gap
    const std::size_t sym = rng.below(cfg.symbols);
    const auto locate = static_cast<std::uint16_t>(sym + 1);
    auto& book = live[sym];

    if (cfg.p_malformed > 0 && rng.uniform() < cfg.p_malformed) {
      switch (rng.below(6)) {
        case 0: enc.executed(locate, ts, next_ref + 1'000'000'000ull, 100, match++); break;
        case 1: enc.del(locate, ts, next_ref + 2'000'000'000ull); break;
        case 2:
          if (!book.empty()) {
            const Live& o = book[rng.below(book.size())];
            enc.add(locate, ts, o.ref, o.side, 100, syms[sym], o.price);
          } else {
            enc.del(locate, ts, 7);
          }
          break;
        case 3:
          if (!book.empty()) {
            const std::size_t k = rng.below(book.size());
            enc.cancel(locate, ts, book[k].ref, book[k].shares + 1 + static_cast<std::uint32_t>(rng.below(50)));
            book[k] = book.back();
            book.pop_back();
          } else {
            enc.del(locate, ts, 9);
          }
          break;
        case 4: enc.raw({'Z', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}); break;
        default: enc.raw({'A', 0, 1, 0, 0}); break;  // truncated add
      }
      ++emitted;
      continue;
    }

    const double u = rng.uniform();
    const bool must_add = book.empty();
    const bool must_remove = book.size() >= cfg.max_live_per_symbol;
    const double t_add = cfg.p_add;
    const double t_delete = t_add + cfg.p_delete;
    const double t_cancel = t_delete + cfg.p_cancel;
    const double t_execute = t_cancel + cfg.p_execute;
    if (must_add || (!must_remove && u < t_add)) {
      const Side side = rng.below(2) ? Side::Sell : Side::Buy;
      const std::uint32_t px = draw_price(sym, side);
      const std::uint32_t sh = draw_shares();
      const std::uint64_t ref = next_ref++;
      enc.add(locate, ts, ref, side, sh, syms[sym], px, rng.below(10) == 0);
      book.push_back(Live{ref, side, px, sh});
    } else {
      const std::size_t k = rng.below(book.size());
      Live& o = book[k];
      auto drop = [&]() {
        book[k] = book.back();
        book.pop_back();
      };
      if (u < t_delete) {
        enc.del(locate, ts, o.ref);
        drop();
      } else if (u < t_cancel) {
        if (o.shares > 1) {
          const auto c = static_cast<std::uint32_t>(1 + rng.below(o.shares - 1));
          enc.cancel(locate, ts, o.ref, c);
          o.shares -= c;
        } else {
          enc.cancel(locate, ts, o.ref, o.shares);
          drop();
        }
      } else if (u < t_execute) {
        const bool full = rng.below(2) == 0;
        const std::uint32_t e =
            full ? o.shares : static_cast<std::uint32_t>(1 + rng.below(o.shares));
        if (rng.below(5) == 0) enc.executed_price(locate, ts, o.ref, e, match++, o.price);
        else enc.executed(locate, ts, o.ref, e, match++);
        if (rng.below(8) == 0) {
          // Occasional non-displayed trade print alongside; no book effect.
          enc.trade(locate, ts, o.side, 100, syms[sym], o.price, match++);
          ++emitted;
        }
        if (e >= o.shares) drop();
        else o.shares -= e;
      } else {
        const std::uint64_t nref = next_ref++;
        const std::uint32_t px = draw_price(sym, o.side);
        const std::uint32_t sh = draw_shares();
        enc.replace(locate, ts, o.ref, nref, sh, px);
        o.ref = nref;
        o.price = px;
        o.shares = sh;
      }
    }
    ++emitted;
  }
  enc.system_event(ts + 1, 'C');
  return out;
}

}  // namespace itchbook
