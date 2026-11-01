// ITCH 5.0 wire format: message layouts and big-endian field decoding.
//
// Reference: NASDAQ TotalView-ITCH 5.0 specification. Every message starts with a common 11-byte
// header (type, stock locate, tracking number, 48-bit nanosecond timestamp). All integers are
// big-endian and unaligned. Decoding is done with memcpy + byte swap, which compiles to a single
// unaligned load + REV on arm64 (BSWAP/MOVBE on x86-64); no SIMD is needed for fields this small.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchbook {

namespace be {
inline std::uint16_t u16(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap16(v);
}
inline std::uint32_t u32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap32(v);
}
inline std::uint64_t u64(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap64(v);
}
// 48-bit timestamp: load 2 + 4 bytes; avoids reading past the field.
inline std::uint64_t u48(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(u16(p)) << 32) | u32(p + 2);
}
}  // namespace be

// Wire sizes (bytes, excluding the 2-byte length prefix used by the sample-file framing).
enum MsgSize : std::uint16_t {
  kSystemEvent = 12,
  kStockDirectory = 39,
  kTradingAction = 25,
  kRegSho = 20,
  kParticipantPosition = 26,
  kMwcbDecline = 35,
  kMwcbStatus = 12,
  kIpoQuoting = 28,
  kLuldCollar = 35,
  kOperationalHalt = 21,
  kAddOrder = 36,
  kAddOrderMpid = 40,
  kOrderExecuted = 31,
  kOrderExecutedPrice = 36,
  kOrderCancel = 23,
  kOrderDelete = 19,
  kOrderReplace = 35,
  kTrade = 44,
  kCrossTrade = 40,
  kBrokenTrade = 19,
  kNoii = 50,
  kRpii = 20,
  kDlcr = 48,
};

// Expected size for a message type, or 0 if the type is unknown.
constexpr std::uint16_t expected_size(std::uint8_t type) noexcept {
  switch (type) {
    case 'S': return kSystemEvent;
    case 'R': return kStockDirectory;
    case 'H': return kTradingAction;
    case 'Y': return kRegSho;
    case 'L': return kParticipantPosition;
    case 'V': return kMwcbDecline;
    case 'W': return kMwcbStatus;
    case 'K': return kIpoQuoting;
    case 'J': return kLuldCollar;
    case 'h': return kOperationalHalt;
    case 'A': return kAddOrder;
    case 'F': return kAddOrderMpid;
    case 'E': return kOrderExecuted;
    case 'C': return kOrderExecutedPrice;
    case 'X': return kOrderCancel;
    case 'D': return kOrderDelete;
    case 'U': return kOrderReplace;
    case 'P': return kTrade;
    case 'Q': return kCrossTrade;
    case 'B': return kBrokenTrade;
    case 'I': return kNoii;
    case 'N': return kRpii;
    case 'O': return kDlcr;
    default: return 0;
  }
}

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

struct Header {
  std::uint16_t locate;
  std::uint16_t tracking;
  std::uint64_t timestamp;  // ns since midnight
};

inline Header decode_header(const std::uint8_t* p) noexcept {
  return Header{be::u16(p + 1), be::u16(p + 3), be::u48(p + 5)};
}

struct SystemEvent {
  Header h;
  char event;  // O S Q M E C
};
struct StockDirectory {
  Header h;
  char stock[8];
  char market_category;
  char financial_status;
  std::uint32_t round_lot;
};
struct TradingAction {
  Header h;
  char stock[8];
  char state;  // H halted, P paused, Q quotation only, T trading
};
struct AddOrder {
  Header h;
  std::uint64_t ref;
  Side side;
  std::uint32_t shares;
  char stock[8];
  std::uint32_t price;  // 4 implied decimals
};
struct OrderExecuted {
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
};
struct OrderExecutedPrice {
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  char printable;
  std::uint32_t price;
};
struct OrderCancel {
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
};
struct OrderDelete {
  Header h;
  std::uint64_t ref;
};
struct OrderReplace {
  Header h;
  std::uint64_t old_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};
struct Trade {
  Header h;
  std::uint64_t ref;
  Side side;
  std::uint32_t shares;
  char stock[8];
  std::uint32_t price;
  std::uint64_t match;
};
struct CrossTrade {
  Header h;
  std::uint64_t shares;
  char stock[8];
  std::uint32_t price;
  std::uint64_t match;
  char cross_type;
};
struct BrokenTrade {
  Header h;
  std::uint64_t match;
};

// ---- Decoders. Preconditions: p points at the type byte and the buffer holds expected_size(type).
inline SystemEvent decode_system_event(const std::uint8_t* p) noexcept {
  return {decode_header(p), static_cast<char>(p[11])};
}
inline StockDirectory decode_stock_directory(const std::uint8_t* p) noexcept {
  StockDirectory m{};
  m.h = decode_header(p);
  std::memcpy(m.stock, p + 11, 8);
  m.market_category = static_cast<char>(p[19]);
  m.financial_status = static_cast<char>(p[20]);
  m.round_lot = be::u32(p + 21);
  return m;
}
inline TradingAction decode_trading_action(const std::uint8_t* p) noexcept {
  TradingAction m{};
  m.h = decode_header(p);
  std::memcpy(m.stock, p + 11, 8);
  m.state = static_cast<char>(p[19]);
  return m;
}
inline AddOrder decode_add_order(const std::uint8_t* p) noexcept {
  AddOrder m{};
  m.h = decode_header(p);
  m.ref = be::u64(p + 11);
  m.side = p[19] == 'S' ? Side::Sell : Side::Buy;
  m.shares = be::u32(p + 20);
  std::memcpy(m.stock, p + 24, 8);
  m.price = be::u32(p + 32);
  return m;
}
inline OrderExecuted decode_order_executed(const std::uint8_t* p) noexcept {
  return {decode_header(p), be::u64(p + 11), be::u32(p + 19), be::u64(p + 23)};
}
inline OrderExecutedPrice decode_order_executed_price(const std::uint8_t* p) noexcept {
  return {decode_header(p), be::u64(p + 11), be::u32(p + 19), be::u64(p + 23),
          static_cast<char>(p[31]), be::u32(p + 32)};
}
inline OrderCancel decode_order_cancel(const std::uint8_t* p) noexcept {
  return {decode_header(p), be::u64(p + 11), be::u32(p + 19)};
}
inline OrderDelete decode_order_delete(const std::uint8_t* p) noexcept {
  return {decode_header(p), be::u64(p + 11)};
}
inline OrderReplace decode_order_replace(const std::uint8_t* p) noexcept {
  return {decode_header(p), be::u64(p + 11), be::u64(p + 19), be::u32(p + 27), be::u32(p + 31)};
}
inline Trade decode_trade(const std::uint8_t* p) noexcept {
  Trade m{};
  m.h = decode_header(p);
  m.ref = be::u64(p + 11);
  m.side = p[19] == 'S' ? Side::Sell : Side::Buy;
  m.shares = be::u32(p + 20);
  std::memcpy(m.stock, p + 24, 8);
  m.price = be::u32(p + 32);
  m.match = be::u64(p + 36);
  return m;
}
inline CrossTrade decode_cross_trade(const std::uint8_t* p) noexcept {
  CrossTrade m{};
  m.h = decode_header(p);
  m.shares = be::u64(p + 11);
  std::memcpy(m.stock, p + 19, 8);
  m.price = be::u32(p + 27);
  m.match = be::u64(p + 31);
  m.cross_type = static_cast<char>(p[39]);
  return m;
}
inline BrokenTrade decode_broken_trade(const std::uint8_t* p) noexcept {
  return {decode_header(p), be::u64(p + 11)};
}

}  // namespace itchbook
