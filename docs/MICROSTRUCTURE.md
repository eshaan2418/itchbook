# Market microstructure used by itchbook

This document explains the market concepts the code depends on, precisely enough to defend each
design decision.

## 1. Order-driven markets and the limit order book

NASDAQ is a continuous double-auction, order-driven market. Participants send **limit orders**
(buy or sell a quantity at a price or better). An order that cannot execute immediately **rests**
in the **limit order book**. The book has two sides:

- **bids**: resting buy orders, best = highest price;
- **asks/offers**: resting sell orders, best = lowest price.

The best bid and best ask together are the **BBO** (top of book). Their difference is the
**spread**; their average is the **mid**. A book is **locked** when best bid == best ask and
**crossed** when best bid > best ask.

## 2. L1, L2, L3

| level | content | itchbook output |
|---|---|---|
| L1 | BBO prices and sizes | `Bbo` events via the listener |
| L2 | aggregate size and order count per price level (market-by-price) | `depth()` |
| L3 | every individual order, in queue order (market-by-order) | internal state; `for_each_order_at()` |

TotalView-ITCH is an **L3 feed**: NASDAQ publishes every displayed order event, and the
consumer must rebuild the book itself. That is why a single lost or misapplied message
corrupts the book until the order in question is deleted, possibly hours later.

## 3. Price-time priority and queue position

NASDAQ matches by **price-time priority**: better prices execute first; at the same price, earlier
orders execute first (FIFO). Each price level is therefore a FIFO queue, implemented as an
intrusive doubly linked list in arrival order. Queue position matters to market makers: being
early in a queue gets more fills and less adverse selection, and it is the key input to
queue-position models in a backtester.

Priority rules for modifications in ITCH 5.0:

- **Order Executed (E / C)** and **Order Cancel (X)** reduce shares and **keep** priority.
- **Order Replace (U)** cancels the original and creates a new order with a new reference number,
  price and size. It **loses** priority and joins the back of the queue, even at an unchanged price.
  `ReplaceLosesQueuePriority` in `tests/test_book.cpp` pins this down.
- **Order Delete (D)** removes the order.

## 4. ITCH 5.0 message model

All messages share an 11-byte header: message type, **stock locate** (a 16-bit per-day symbol index
NASDAQ assigns in the Stock Directory `R` message), tracking number, and a 48-bit timestamp in
nanoseconds since midnight. Integers are big-endian, prices are unsigned 32-bit with four implied
decimals (`1234500` = $123.45).

Book-affecting messages:

| type | name | fields used | effect |
|---|---|---|---|
| A | Add Order (no MPID) | ref, side, shares, price | new resting order at the back of its level |
| F | Add Order with MPID attribution | as A (+ MPID) | same as A |
| E | Order Executed | ref, executed shares | reduce; remove at zero |
| C | Order Executed With Price | ref, shares, execution price | as E (the print can be at a different price, e.g. a cross) |
| X | Order Cancel | ref, cancelled shares | partial cancel |
| D | Order Delete | ref | remove |
| U | Order Replace | old ref, new ref, shares, price | remove old, add new (side and symbol inherited) |

Messages that print trades without a displayed book effect: **P** (non-displayed/hidden order
executions), **Q** (cross trades: opening, closing, halt and IPO crosses), **B** (broken trade).
Hidden liquidity never appears in an L3 rebuild; only its executions are visible through `P`.
That limits any ITCH-derived book.

Order reference numbers are unique for the whole day across all symbols. So a single global
ref → order hash map is enough, and the E/X/D/U messages need no symbol lookup.

## 5. Sessions and the crossed-book counter

A NASDAQ day in the sample file runs from about 03:04 (system start) through pre-market
(04:00-09:30), regular hours (09:30-16:00) and post-market (16:00-20:00). The opening and closing
crosses are single-price auctions. Their trades print as `Q` messages, and the orders they consume
are removed with `E`/`C`/`D`.

The displayed book should not stay crossed during continuous trading. It can lock or cross
briefly around halts, crosses and pre-market activity, and with orders that rest but do not
interact. itchbook does not assert "never crossed". `verify` counts the messages after which the
affected symbol's book is crossed, split by session, and reports the counts as data.

## 6. Message mix on a real day (2019-01-30, measured)

From `bench/results/stats_01302019.json`: 368,366,634 messages. Adds (A+F) are 44.7%, deletes (D)
43.0%, replaces (U) 7.4%, executions (E+C) 2.2%, cancels (X) 1.3%. Fewer than 1 in 40 messages is
an execution. The workload is dominated by add/delete churn near the touch, which is why the design
optimises the add → delete round trip: an O(1) ref lookup plus an O(1)-amortised update at the best
end of a small sorted array.

## 7. What a production feed handler adds beyond this project

- **Transport**: MoldUDP64 multicast (A/B line arbitration, sequence-gap detection,
  retransmission requests) or SoupBinTCP. The sample files contain only the unframed message
  stream with a 2-byte length prefix.
- **Snapshot / recovery**: rebuilding state after a late join from a snapshot or a GLIMPSE session.
- **Symbol filtering and fan-out** per strategy, and conflation for slow consumers.
