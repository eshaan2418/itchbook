// Zero-copy, allocation-free ITCH 5.0 framed-stream parser.
//
// Framing: NASDAQ's sample files (and the payload of SoupBinTCP/MoldUDP64 after session
// stripping) are a sequence of [u16 big-endian length][message]. The parser walks a caller-owned
// buffer, decodes each message into a small POD on the stack and dispatches it to a handler by
// static (template) dispatch: no virtual calls, no heap, no exceptions.
//
// Handlers inherit HandlerBase (non-virtual, all no-ops) and shadow the callbacks they need.
#pragma once

#include <cstddef>
#include <cstdint>

#include "itchbook/messages.hpp"

namespace itchbook {

enum class ParseError : std::uint8_t { UnknownType, BadLength };

struct HandlerBase {
  void on_system_event(const SystemEvent&) noexcept {}
  void on_stock_directory(const StockDirectory&) noexcept {}
  void on_trading_action(const TradingAction&) noexcept {}
  void on_add(const AddOrder&) noexcept {}
  void on_executed(const OrderExecuted&) noexcept {}
  void on_executed_price(const OrderExecutedPrice&) noexcept {}
  void on_cancel(const OrderCancel&) noexcept {}
  void on_delete(const OrderDelete&) noexcept {}
  void on_replace(const OrderReplace&) noexcept {}
  void on_trade(const Trade&) noexcept {}
  void on_cross_trade(const CrossTrade&) noexcept {}
  void on_broken_trade(const BrokenTrade&) noexcept {}
  // Messages that carry no book state (Reg SHO, MWCB, LULD, NOII, ...).
  void on_other(std::uint8_t /*type*/, const std::uint8_t* /*msg*/, std::uint16_t /*len*/) noexcept {}
  void on_error(ParseError, std::uint8_t /*type*/, std::uint16_t /*len*/) noexcept {}
};

// Dispatch one unframed message of `len` bytes. Returns false on a malformed message (which is
// reported to the handler and skipped; framing lets us resynchronise on the next message).
template <class H>
inline bool dispatch(const std::uint8_t* msg, std::uint16_t len, H& h) noexcept {
  if (len == 0) [[unlikely]] {
    h.on_error(ParseError::BadLength, 0, 0);
    return false;
  }
  const std::uint8_t type = msg[0];
  const std::uint16_t want = expected_size(type);
  if (want == 0) [[unlikely]] {
    h.on_error(ParseError::UnknownType, type, len);
    return false;
  }
  if (want != len) [[unlikely]] {
    h.on_error(ParseError::BadLength, type, len);
    return false;
  }
  // Ordered roughly by frequency in a real NASDAQ day (A, D, U, E, X dominate).
  switch (type) {
    case 'A': h.on_add(decode_add_order(msg)); break;
    case 'D': h.on_delete(decode_order_delete(msg)); break;
    case 'U': h.on_replace(decode_order_replace(msg)); break;
    case 'E': h.on_executed(decode_order_executed(msg)); break;
    case 'X': h.on_cancel(decode_order_cancel(msg)); break;
    case 'F': {
      // F is A plus a 4-byte MPID attribution; the book does not need the MPID.
      h.on_add(decode_add_order(msg));
      break;
    }
    case 'C': h.on_executed_price(decode_order_executed_price(msg)); break;
    case 'P': h.on_trade(decode_trade(msg)); break;
    case 'Q': h.on_cross_trade(decode_cross_trade(msg)); break;
    case 'B': h.on_broken_trade(decode_broken_trade(msg)); break;
    case 'S': h.on_system_event(decode_system_event(msg)); break;
    case 'R': h.on_stock_directory(decode_stock_directory(msg)); break;
    case 'H': h.on_trading_action(decode_trading_action(msg)); break;
    default: h.on_other(type, msg, len); break;
  }
  return true;
}

struct ParseResult {
  std::size_t consumed;   // bytes fully consumed (complete frames only)
  std::size_t messages;   // frames dispatched (including malformed ones)
};

// Parse as many complete frames as `buf` holds. An incomplete trailing frame is left unconsumed
// so the caller can carry it into the next read.
template <class H>
inline ParseResult parse(const std::uint8_t* buf, std::size_t n, H& h) noexcept {
  std::size_t off = 0;
  std::size_t count = 0;
  while (n - off >= 2) {
    const std::uint16_t len = be::u16(buf + off);
    if (n - off - 2 < len) break;
    dispatch(buf + off + 2, len, h);
    off += 2 + static_cast<std::size_t>(len);
    ++count;
  }
  return {off, count};
}

// Index of frame start offsets; used by the benchmark to time messages individually and to
// prefetch ahead without re-walking the framing.
template <class F>
inline std::size_t for_each_frame(const std::uint8_t* buf, std::size_t n, F&& f) noexcept {
  std::size_t off = 0;
  while (n - off >= 2) {
    const std::uint16_t len = be::u16(buf + off);
    if (n - off - 2 < len) break;
    f(buf + off + 2, len);
    off += 2 + static_cast<std::size_t>(len);
  }
  return off;
}

}  // namespace itchbook
