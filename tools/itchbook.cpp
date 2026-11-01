// itchbook CLI: replay / verify / top / stats / gen.
//
// Built with -fno-exceptions -fno-rtti. All replay paths stream-decompress input in fixed-size
// chunks (decompression is excluded from timed regions and reported separately).
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif
#include <sys/resource.h>
#include <time.h>
#include <zlib.h>

#include "itchbook/alloc_counter.hpp"
#include "itchbook/gz_reader.hpp"
#include "itchbook/latency.hpp"
#include "itchbook/order_book.hpp"
#include "itchbook/parser.hpp"
#include "itchbook/reference_book.hpp"
#include "itchbook/spsc_ring.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

namespace {

// ---- tiny argument parser -------------------------------------------------------------------
struct Args {
  std::vector<std::string> pos;
  std::vector<std::pair<std::string, std::string>> kv;
  const char* get(const char* k, const char* def = nullptr) const {
    for (auto& [a, b] : kv)
      if (a == k) return b.c_str();
    return def;
  }
  std::uint64_t num(const char* k, std::uint64_t def) const {
    const char* v = get(k);
    return v ? std::strtoull(v, nullptr, 10) : def;
  }
  bool flag(const char* k) const { return get(k) != nullptr; }
};

Args parse_args(int argc, char** argv, int from) {
  Args a;
  for (int i = from; i < argc; ++i) {
    std::string s = argv[i];
    if (s.rfind("--", 0) == 0) {
      std::string key = s.substr(2), val = "1";
      const auto eq = key.find('=');
      if (eq != std::string::npos) {
        val = key.substr(eq + 1);
        key = key.substr(0, eq);
      } else if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
        val = argv[++i];
      }
      a.kv.emplace_back(key, val);
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

void prefer_performance_cores() {
#if defined(__APPLE__)
  // macOS cannot pin threads to cores; USER_INTERACTIVE QoS biases scheduling to P-cores.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

double wall_seconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

long peak_rss_bytes() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return ru.ru_maxrss;  // bytes on macOS
#else
  return ru.ru_maxrss * 1024;  // KiB on Linux
#endif
}

// Order ref of a book message, used for prefetching ahead; 0 if the message has none.
inline std::uint64_t ref_of(const std::uint8_t* m) noexcept {
  switch (m[0]) {
    case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U': return be::u64(m + 11);
    default: return 0;
  }
}

std::string fmt_ts(std::uint64_t ns) {
  const std::uint64_t s = ns / 1'000'000'000ull;
  char b[32];
  std::snprintf(b, sizeof b, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%06" PRIu64, s / 3600,
                (s / 60) % 60, s % 60, (ns / 1000) % 1'000'000);
  return b;
}


// ---- replay ------------------------------------------------------------------------------------
struct ReplayOpts {
  std::uint64_t max_msgs;
  std::size_t chunk_bytes;
  unsigned prefetch;   // frames ahead (0 = off)
  bool latency;        // time every message individually
  const char* hist_out;
  unsigned prefetch_stages;  // 1 = hash slot; 2 = + order record / add-side array; 3 = + level
};

template <class Book>
inline void prefetch_ahead(const Book& book, const std::vector<const std::uint8_t*>& frames,
                           std::size_t i, std::size_t n, const ReplayOpts& o) noexcept {
  if (!o.prefetch) return;
  if constexpr (requires { book.prefetch_stage2(frames[0]); }) {
    if (o.prefetch_stages >= 2) {
      if (i + o.prefetch < n) book.prefetch_stage1(frames[i + o.prefetch]);
      const std::size_t near = o.prefetch / 2;
      if (near && i + near < n) book.prefetch_stage2(frames[i + near]);
      if (o.prefetch_stages >= 3) {
        const std::size_t nearest = o.prefetch / 4;
        if (nearest && i + nearest < n) book.prefetch_stage3(frames[i + nearest]);
      }
      return;
    }
  }
  if (i + o.prefetch < n)
    if (const std::uint64_t r = ref_of(frames[i + o.prefetch])) book.prefetch_ref(r);
}

struct NoExtra {
  void operator()() const {}
};

template <class Book, class Extra = NoExtra>
int replay_with(Book& book, const std::string& path, const ReplayOpts& o, const char* impl,
                Extra extra = {}) {
  GzReader reader(path);
  if (!reader.ok()) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 1;
  }
  FrameChunker chunker(reader, o.chunk_bytes);
  Histogram hist;
  std::vector<const std::uint8_t*> frames;
  frames.reserve(o.chunk_bytes / 13 + 1);
  std::uint64_t msgs = 0, timed_ticks = 0, chunks = 0;
  std::uint64_t allocs_after_warmup = 0;
  std::size_t peak_live = 0;
  const double ns_tick = Clock::ns_per_tick();
  const double t_wall0 = wall_seconds();
  double decompress_s = 0;

  const std::uint8_t* data;
  std::size_t size;
  for (;;) {
    const double d0 = wall_seconds();
    if (!chunker.fill(data, size)) break;
    decompress_s += wall_seconds() - d0;
    // Frame index (cheap: one u16 load per frame) — inside the timed region for throughput.
    const std::uint64_t a0 = alloc_count();
    const std::uint64_t c0 = Clock::now();
    frames.clear();
    for_each_frame(data, size, [&](const std::uint8_t* m, std::uint16_t) { frames.push_back(m); });
    std::size_t n = frames.size();
    if (o.max_msgs && msgs + n > o.max_msgs) n = static_cast<std::size_t>(o.max_msgs - msgs);
    if (o.latency) {
      std::uint64_t prev = Clock::now();
      for (std::size_t i = 0; i < n; ++i) {
        prefetch_ahead(book, frames, i, n, o);
        const std::uint8_t* m = frames[i];
        dispatch(m, be::u16(m - 2), book);
        const std::uint64_t t = Clock::now();
        hist.record(t - prev);
        prev = t;
      }
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        prefetch_ahead(book, frames, i, n, o);
        const std::uint8_t* m = frames[i];
        dispatch(m, be::u16(m - 2), book);
      }
    }
    timed_ticks += Clock::now() - c0;
    if (chunks > 0) allocs_after_warmup += alloc_count() - a0;
    ++chunks;
    msgs += n;
    if (book.live_orders() > peak_live) peak_live = book.live_orders();
    if (o.max_msgs && msgs >= o.max_msgs) break;
  }
  const double wall = wall_seconds() - t_wall0;
  const double book_s = static_cast<double>(timed_ticks) * ns_tick * 1e-9;
  const BookStats& s = book.stats();

  std::printf("{\n  \"impl\": \"%s\",\n  \"messages\": %" PRIu64 ",\n", impl, msgs);
  std::printf("  \"bytes_decompressed\": %zu,\n", chunker.bytes_read());
  std::printf("  \"book_seconds\": %.4f,\n  \"book_msgs_per_sec\": %.0f,\n", book_s,
              static_cast<double>(msgs) / book_s);
  std::printf("  \"decompress_seconds\": %.4f,\n  \"wall_seconds\": %.4f,\n", decompress_s, wall);
  std::printf("  \"end_to_end_msgs_per_sec\": %.0f,\n", static_cast<double>(msgs) / wall);
  std::printf("  \"prefetch\": %u,\n  \"prefetch_stages\": %u,\n  \"chunk_bytes\": %zu,\n",
              o.prefetch, o.prefetch ? o.prefetch_stages : 0, o.chunk_bytes);
  std::printf("  \"adds\": %" PRIu64 ", \"executes\": %" PRIu64 ", \"cancels\": %" PRIu64
              ", \"deletes\": %" PRIu64 ", \"replaces\": %" PRIu64 ",\n",
              s.adds, s.executes, s.cancels, s.deletes, s.replaces);
  std::printf("  \"unknown_ref\": %" PRIu64 ", \"duplicate_ref\": %" PRIu64
              ", \"overfill\": %" PRIu64 ",\n",
              s.unknown_ref, s.duplicate_ref, s.overfill);
  std::printf("  \"bbo_updates\": %" PRIu64 ",\n", s.bbo_updates);
  std::printf("  \"live_orders_end\": %zu,\n  \"peak_live_orders_chunk_sampled\": %zu,\n",
              book.live_orders(), peak_live);
  std::printf("  \"heap_allocs_after_first_chunk\": %" PRIu64 ",\n", allocs_after_warmup);
  std::printf("  \"peak_rss_bytes\": %ld", peak_rss_bytes());
  if (o.latency) {
    auto q = [&](double p) { return static_cast<double>(hist.quantile(p)) * ns_tick; };
    std::printf(",\n  \"timer_ns_per_tick\": %.3f,\n  \"latency_ns\": {\"p50\": %.1f, \"p90\": %.1f, "
                "\"p99\": %.1f, \"p99.9\": %.1f, \"p99.99\": %.1f, \"max\": %.1f, \"mean\": %.2f}",
                ns_tick, q(0.5), q(0.9), q(0.99), q(0.999), q(0.9999),
                static_cast<double>(hist.max()) * ns_tick, hist.mean() * ns_tick);
    if (o.hist_out) {
      if (FILE* f = std::fopen(o.hist_out, "w")) {
        std::fprintf(f, "# unit: timer ticks; ns_per_tick=%.6f\n", ns_tick);
        std::fputs(hist.to_csv().c_str(), f);
        std::fclose(f);
      }
    }
  }
  extra();
  std::printf("\n}\n");
  return chunker.error() ? 2 : 0;
}

// ---- publish mode: BBO updates handed to a strategy thread over the SPSC ring -----------------
struct BboEvent {
  std::uint64_t pushed_at;  // Clock ticks when the feed thread published
  std::uint64_t exch_ts;
  Bbo bbo;
  std::uint16_t locate;
};

struct PublishListener {
  SpscRing<BboEvent>* ring = nullptr;
  std::uint64_t* stalls = nullptr;
  void on_bbo(std::uint16_t locate, const Bbo& b, std::uint64_t ts) noexcept {
    BboEvent e{Clock::now(), ts, b, locate};
    // Back-pressure policy: never drop a BBO; spin and count stalls (a slow-consumer signal).
    while (!ring->try_push(e)) [[unlikely]] {
      ++*stalls;
      e.pushed_at = Clock::now();
    }
  }
};

int replay_publish(const Args& a, const ReplayOpts& o, const BookConfig& cfg) {
  SpscRing<BboEvent> ring(static_cast<std::size_t>(a.num("ring", 1u << 16)));
  std::uint64_t stalls = 0;
  std::atomic<bool> stop{false};
  Histogram hop;  // feed thread push -> strategy thread pop, in ticks
  std::uint64_t consumed = 0, checksum = 0;
  std::thread consumer([&] {
    prefer_performance_cores();
    BboEvent e{};
    for (;;) {
      if (ring.try_pop(e)) {
        hop.record(Clock::now() - e.pushed_at);
        ++consumed;
        checksum += e.bbo.bid_px ^ (e.bbo.ask_px * 31u) ^ e.locate;
      } else if (stop.load(std::memory_order_acquire)) {
        // stop is set after the producer's last push; one more drain pass catches stragglers.
        while (ring.try_pop(e)) {
          hop.record(Clock::now() - e.pushed_at);
          ++consumed;
          checksum += e.bbo.bid_px ^ (e.bbo.ask_px * 31u) ^ e.locate;
        }
        return;
      }
    }
  });
  auto b = std::make_unique<OrderBook<PublishListener>>(cfg, PublishListener{&ring, &stalls});
  const double ns_tick = Clock::ns_per_tick();
  int rc = replay_with(*b, a.pos[0], o, "opt-linear+publish", [&] {
    stop.store(true, std::memory_order_release);
    consumer.join();
    auto q = [&](double p) { return static_cast<double>(hop.quantile(p)) * ns_tick; };
    std::printf(",\n  \"ring_capacity\": %zu,\n  \"bbo_consumed\": %llu,\n  \"producer_stalls\": %llu,"
                "\n  \"consumer_checksum\": %llu,\n  \"handoff_latency_ns\": {\"p50\": %.1f, \"p90\": %.1f, "
                "\"p99\": %.1f, \"p99.9\": %.1f, \"p99.99\": %.1f, \"max\": %.1f}",
                ring.capacity(), static_cast<unsigned long long>(consumed),
                static_cast<unsigned long long>(stalls), static_cast<unsigned long long>(checksum),
                q(0.5), q(0.9), q(0.99), q(0.999), q(0.9999),
                static_cast<double>(hop.max()) * ns_tick);
  });
  return rc;
}

int cmd_replay(const Args& a) {
  if (a.pos.empty()) {
    std::fprintf(stderr, "usage: itchbook replay FILE [--impl opt|opt-binary|opt-hybrid|ref] [--max-msgs N]"
                         " [--prefetch D] [--latency] [--hist-out F] [--chunk-mb M] [--publish [--ring N]]\n");
    return 1;
  }
  prefer_performance_cores();
  ReplayOpts o{a.num("max-msgs", 0), static_cast<std::size_t>(a.num("chunk-mb", 64)) << 20,
               static_cast<unsigned>(a.num("prefetch", 0)), a.flag("latency"), a.get("hist-out"),
               static_cast<unsigned>(a.num("prefetch-stages", 1))};
  const std::string impl = a.get("impl", "opt");
  BookConfig cfg{static_cast<std::size_t>(a.num("order-capacity", 1u << 22)),
                 static_cast<std::size_t>(a.num("level-capacity", 1u << 20)),
                 static_cast<std::size_t>(a.num("side-reserve", 256))};
  if (a.flag("publish")) return replay_publish(a, o, cfg);
  if (impl == "opt") {
    auto b = std::make_unique<OrderBook<NullListener, LevelSearch::Linear>>(cfg);
    return replay_with(*b, a.pos[0], o, "opt-linear");
  }
  if (impl == "opt-binary") {
    auto b = std::make_unique<OrderBook<NullListener, LevelSearch::Binary>>(cfg);
    return replay_with(*b, a.pos[0], o, "opt-binary");
  }
  if (impl == "opt-hybrid") {
    auto b = std::make_unique<OrderBook<NullListener, LevelSearch::Hybrid>>(cfg);
    return replay_with(*b, a.pos[0], o, "opt-hybrid");
  }
  if (impl == "ref") {
    struct RefAdapter : ReferenceBook {
      void prefetch_ref(std::uint64_t) const noexcept {}
    };
    auto b = std::make_unique<RefAdapter>();
    return replay_with(*b, a.pos[0], o, "reference");
  }
  std::fprintf(stderr, "unknown --impl %s\n", impl.c_str());
  return 1;
}

// ---- verify: differential check against the reference on every message -------------------------
struct VerifyTee : HandlerBase {
  OrderBook<>& opt;
  ReferenceBook& ref;
  std::uint16_t touched = 0;
  std::uint64_t ts = 0;
  VerifyTee(OrderBook<>& o, ReferenceBook& r) : opt(o), ref(r) {}
  void on_stock_directory(const StockDirectory& m) { opt.on_stock_directory(m); }
#define VTEE(fn, T)        \
  void fn(const T& m) {    \
    touched = m.h.locate;  \
    ts = m.h.timestamp;    \
    opt.fn(m);             \
    ref.fn(m);             \
  }
  VTEE(on_add, AddOrder)
  VTEE(on_executed, OrderExecuted)
  VTEE(on_executed_price, OrderExecutedPrice)
  VTEE(on_cancel, OrderCancel)
  VTEE(on_delete, OrderDelete)
  VTEE(on_replace, OrderReplace)
#undef VTEE
};

bool levels_equal(const OrderBook<>& opt, const ReferenceBook& ref, std::uint16_t l, std::size_t k,
                  std::vector<LevelInfo>& tmp) {
  for (Side s : {Side::Buy, Side::Sell}) {
    if (opt.level_count(l, s) != ref.level_count(l, s)) return false;
    const auto want = ref.depth(l, s, k);
    const std::size_t n = want.size();
    tmp.resize(n);
    if (opt.depth(l, s, tmp.data(), n) != n) return false;
    for (std::size_t i = 0; i < n; ++i)
      if (!(tmp[i] == want[i])) return false;
  }
  return opt.bbo(l) == ref.bbo(l);
}

int cmd_verify(const Args& a) {
  if (a.pos.empty()) {
    std::fprintf(stderr, "usage: itchbook verify FILE [--max-msgs N] [--depth K] [--full-every M]\n");
    return 1;
  }
  const std::uint64_t max_msgs = a.num("max-msgs", 0);
  const std::size_t k = a.num("depth", 10);
  const std::uint64_t full_every = a.num("full-every", 1000);
  GzReader reader(a.pos[0]);
  if (!reader.ok()) return 1;
  FrameChunker chunker(reader, 64u << 20);
  auto opt = std::make_unique<OrderBook<>>();
  ReferenceBook ref;
  VerifyTee tee(*opt, ref);
  std::vector<LevelInfo> tmp;
  std::uint64_t msgs = 0, compared = 0, full = 0, mismatches = 0;
  std::uint64_t crossed[3] = {0, 0, 0};  // pre-open, regular hours, post-close
  const std::uint64_t open_ns = 34200ull * 1'000'000'000, close_ns = 57600ull * 1'000'000'000;
  const std::uint8_t* data;
  std::size_t size;
  bool done = false;
  while (!done && chunker.fill(data, size)) {
    for_each_frame(data, size, [&](const std::uint8_t* m, std::uint16_t len) {
      if (done) return;
      tee.touched = 0;
      dispatch(m, len, tee);
      ++msgs;
      if (tee.touched) {
        const std::uint16_t l = tee.touched;
        const bool deep = full_every && (msgs % full_every == 0);
        ++compared;
        if (deep) ++full;
        if (!levels_equal(*opt, ref, l, deep ? ~std::size_t{0} : k, tmp)) {
          if (++mismatches <= 10)
            std::fprintf(stderr, "MISMATCH at message %" PRIu64 " locate %u ts %s\n", msgs, l,
                         fmt_ts(tee.ts).c_str());
        }
        if (opt->crossed(l)) ++crossed[tee.ts < open_ns ? 0 : tee.ts < close_ns ? 1 : 2];
      }
      if (max_msgs && msgs >= max_msgs) done = true;
    });
    std::fprintf(stderr, "  verified %" PRIu64 " msgs (%s)\n", msgs, fmt_ts(tee.ts).c_str());
  }
  // Final: every symbol, full depth.
  std::uint64_t final_mismatch = 0;
  for (std::uint32_t l = 0; l < 65536; ++l)
    if (!levels_equal(*opt, ref, static_cast<std::uint16_t>(l), ~std::size_t{0}, tmp)) ++final_mismatch;
  const BookStats& s = opt->stats();
  const BookStats& r = ref.stats();
  const bool stats_equal = s.adds == r.adds && s.executes == r.executes && s.cancels == r.cancels &&
                           s.deletes == r.deletes && s.replaces == r.replaces &&
                           s.unknown_ref == r.unknown_ref && s.duplicate_ref == r.duplicate_ref &&
                           s.overfill == r.overfill;
  std::printf("{\n  \"messages\": %" PRIu64 ",\n  \"book_messages_compared\": %" PRIu64
              ",\n  \"compared_top_k\": %zu,\n  \"full_depth_compares\": %" PRIu64
              ",\n  \"mismatches\": %" PRIu64 ",\n  \"final_symbols_mismatched\": %" PRIu64
              ",\n  \"integrity_counters_equal\": %s,\n  \"unknown_ref\": %" PRIu64
              ", \"duplicate_ref\": %" PRIu64 ", \"overfill\": %" PRIu64
              ",\n  \"crossed_after_msg\": {\"pre_open\": %" PRIu64 ", \"regular\": %" PRIu64
              ", \"post_close\": %" PRIu64 "},\n  \"live_orders_end\": %zu,\n"
              "  \"last_timestamp\": \"%s\"\n}\n",
              msgs, compared, k, full, mismatches, final_mismatch, stats_equal ? "true" : "false",
              s.unknown_ref, s.duplicate_ref, s.overfill, crossed[0], crossed[1], crossed[2],
              opt->live_orders(), fmt_ts(tee.ts).c_str());
  return (mismatches || final_mismatch || !stats_equal) ? 3 : 0;
}

// ---- stats: message mix --------------------------------------------------------------------------
int cmd_stats(const Args& a) {
  if (a.pos.empty()) return 1;
  GzReader reader(a.pos[0]);
  if (!reader.ok()) return 1;
  FrameChunker chunker(reader, 64u << 20);
  std::uint64_t counts[256] = {};
  std::uint64_t msgs = 0, bytes = 0, first_ts = 0, last_ts = 0;
  const std::uint64_t max_msgs = a.num("max-msgs", 0);
  auto book = std::make_unique<OrderBook<>>();
  std::size_t peak_live = 0;
  std::uint64_t peak_ts = 0;
  std::size_t peak_levels = 0;
  std::uint64_t max_ref = 0;
  const std::uint8_t* data;
  std::size_t size;
  bool done = false;
  while (!done && chunker.fill(data, size)) {
    for_each_frame(data, size, [&](const std::uint8_t* m, std::uint16_t len) {
      if (done) return;
      ++counts[m[0]];
      ++msgs;
      bytes += len + 2u;
      if (len >= 11) {
        last_ts = be::u48(m + 5);
        if (!first_ts) first_ts = last_ts;
      }
      if ((m[0] == 'A' || m[0] == 'F') && len >= 19) max_ref = std::max(max_ref, be::u64(m + 11));
      if (m[0] == 'U' && len >= 27) max_ref = std::max(max_ref, be::u64(m + 19));
      dispatch(m, len, *book);
      if (book->live_orders() > peak_live) {
        peak_live = book->live_orders();
        peak_ts = last_ts;
      }
      if (book->live_levels() > peak_levels) peak_levels = book->live_levels();
      if (max_msgs && msgs >= max_msgs) done = true;
    });
  }
  std::printf("{\n  \"messages\": %" PRIu64 ",\n  \"bytes\": %" PRIu64
              ",\n  \"first_ts\": \"%s\",\n  \"last_ts\": \"%s\",\n  \"peak_live_orders\": %zu,"
              "\n  \"peak_live_orders_at\": \"%s\",\n  \"peak_live_levels\": %zu,\n  \"max_order_ref\": %" PRIu64
              ",\n  \"by_type\": {",
              msgs, bytes, fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str(), peak_live,
              fmt_ts(peak_ts).c_str(), peak_levels, max_ref);
  bool first = true;
  for (int t = 0; t < 256; ++t)
    if (counts[t]) {
      std::printf("%s\"%c\": %" PRIu64, first ? "" : ", ", t, counts[t]);
      first = false;
    }
  std::printf("}\n}\n");
  return 0;
}

// ---- top: live top-of-book for one symbol (demo) --------------------------------------------------
int cmd_top(const Args& a) {
  if (a.pos.empty() || !a.get("symbol")) {
    std::fprintf(stderr, "usage: itchbook top FILE --symbol SYM [--depth 5] [--every-ms 1000]"
                         " [--from HH:MM] [--frames 20] [--sleep-ms 150]\n");
    return 1;
  }
  std::string sym = a.get("symbol");
  sym.resize(8, ' ');
  const std::size_t depth = a.num("depth", 5);
  const std::uint64_t every_ns = a.num("every-ms", 1000) * 1'000'000ull;
  const std::uint64_t frames_max = a.num("frames", 20);
  const std::uint64_t sleep_ms = a.num("sleep-ms", 150);
  std::uint64_t from_ns = 0;
  if (const char* f = a.get("from")) {
    unsigned hh = 0, mm = 0;
    std::sscanf(f, "%u:%u", &hh, &mm);
    from_ns = (hh * 3600ull + mm * 60ull) * 1'000'000'000ull;
  }
  GzReader reader(a.pos[0]);
  if (!reader.ok()) return 1;
  FrameChunker chunker(reader, 16u << 20);
  auto book = std::make_unique<OrderBook<>>();
  std::int32_t locate = -1;
  std::uint64_t next_print = 0, printed = 0, msgs = 0, sym_msgs = 0;
  std::vector<LevelInfo> bids(depth), asks(depth);
  const std::uint8_t* data;
  std::size_t size;
  bool done = false;
  while (!done && chunker.fill(data, size)) {
    for_each_frame(data, size, [&](const std::uint8_t* m, std::uint16_t len) {
      if (done) return;
      ++msgs;
      if (m[0] == 'R' && len == kStockDirectory && std::memcmp(m + 11, sym.data(), 8) == 0)
        locate = be::u16(m + 1);
      dispatch(m, len, *book);
      if (locate < 0 || len < 11) return;
      if (be::u16(m + 1) == locate) ++sym_msgs;
      const std::uint64_t ts = be::u48(m + 5);
      if (ts < from_ns || ts < next_print) return;
      next_print = ts + every_ns;
      const auto l = static_cast<std::uint16_t>(locate);
      const std::size_t nb = book->depth(l, Side::Buy, bids.data(), depth);
      const std::size_t na = book->depth(l, Side::Sell, asks.data(), depth);
      std::printf("\033[2J\033[H%s  %s   feed msgs %" PRIu64 "  (%s msgs %" PRIu64 ")\n\n",
                  a.get("symbol"), fmt_ts(ts).c_str(), msgs, a.get("symbol"), sym_msgs);
      std::printf("   %8s %10s %5s | %-10s %-8s %-5s\n", "orders", "bid size", "", "ask", "size",
                  "orders");
      for (std::size_t i = 0; i < depth; ++i) {
        char lb[64] = "", rb[64] = "";
        if (i < nb)
          std::snprintf(lb, sizeof lb, "%8u %10" PRIu64 " %10.4f", bids[i].count, bids[i].shares,
                        bids[i].price / 1e4);
        if (i < na)
          std::snprintf(rb, sizeof rb, "%-10.4f %-8" PRIu64 " %-5u", asks[i].price / 1e4,
                        asks[i].shares, asks[i].count);
        std::printf("%30s | %s\n", lb, rb);
      }
      const Bbo b = book->bbo(l);
      if (b.bid_px && b.ask_px)
        std::printf("\n spread %.4f  mid %.4f  levels %zu/%zu\n", (b.ask_px - b.bid_px) / 1e4,
                    (b.ask_px + b.bid_px) / 2e4, book->level_count(l, Side::Buy),
                    book->level_count(l, Side::Sell));
      std::fflush(stdout);
      if (sleep_ms) {
        timespec t{static_cast<time_t>(sleep_ms / 1000), static_cast<long>((sleep_ms % 1000) * 1'000'000)};
        nanosleep(&t, nullptr);
      }
      if (++printed >= frames_max) done = true;
    });
  }
  return 0;
}

// ---- gen: synthetic ITCH stream ---------------------------------------------------------------------
int cmd_gen(const Args& a) {
  if (a.pos.empty()) return 1;
  SynthConfig cfg;
  cfg.seed = a.num("seed", 42);
  cfg.messages = a.num("messages", 1'000'000);
  cfg.symbols = a.num("symbols", 8);
  const auto buf = generate_synthetic(cfg);
  gzFile f = gzopen(a.pos[0].c_str(), "wb6");
  if (!f) return 1;
  gzwrite(f, buf.data(), static_cast<unsigned>(buf.size()));
  gzclose(f);
  std::printf("wrote %zu bytes (%" PRIu64 " messages) to %s\n", buf.size(),
              static_cast<std::uint64_t>(cfg.messages), a.pos[0].c_str());
  return 0;
}

// ---- timer: resolution and overhead of the timestamp source ------------------------------------
int cmd_timer(const Args& a) {
  prefer_performance_cores();
  const std::uint64_t n = a.num("samples", 20'000'000);
  const double ns_tick = Clock::ns_per_tick();
  // Overhead: back-to-back reads, total elapsed / n.
  const double w0 = wall_seconds();
  std::uint64_t sink = 0;
  for (std::uint64_t i = 0; i < n; ++i) sink += Clock::now();
  const double per_call_ns = (wall_seconds() - w0) * 1e9 / static_cast<double>(n);
  // Resolution: distribution of the smallest observable increments.
  Histogram steps;
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t t0 = Clock::now();
    std::uint64_t t1;
    do t1 = Clock::now(); while (t1 == t0);
    steps.record(t1 - t0);
  }
  std::printf("{\n  \"reported_ns_per_tick\": %.4f,\n  \"read_overhead_ns\": %.2f,\n"
              "  \"min_step_ns\": %.2f,\n  \"median_step_ns\": %.2f,\n  \"p99_step_ns\": %.2f,\n"
              "  \"sink\": %llu\n}\n",
              ns_tick, per_call_ns, static_cast<double>(steps.min()) * ns_tick,
              static_cast<double>(steps.quantile(0.5)) * ns_tick,
              static_cast<double>(steps.quantile(0.99)) * ns_tick,
              static_cast<unsigned long long>(sink & 1));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: itchbook {replay|verify|stats|top|gen|timer} ...\n");
    return 1;
  }
  const std::string cmd = argv[1];
  const Args a = parse_args(argc, argv, 2);
  if (cmd == "replay") return cmd_replay(a);
  if (cmd == "verify") return cmd_verify(a);
  if (cmd == "stats") return cmd_stats(a);
  if (cmd == "top") return cmd_top(a);
  if (cmd == "gen") return cmd_gen(a);
  if (cmd == "timer") return cmd_timer(a);
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 1;
}
