# LOBSTER data format and the edge cases that matter

LOBSTER (lobsterdata.com) reconstructs NASDAQ TotalView-ITCH into two CSVs per
ticker-day and depth level `k`. The free samples used here are 2012-06-21,
09:30–16:00, level 10, for AMZN, AAPL, GOOG, INTC, MSFT.

## Files

`TICKER_DATE_34200000_57600000_message_10.csv` (34200000 ms = 09:30, 57600000 ms = 16:00)

| column | meaning | our type |
|---|---|---|
| time | seconds after midnight, up to 9 decimals (`34200.017459617`) | `int64` ns |
| type | event type 1–7 (below) | `int8` |
| order_id | exchange order reference; 0 for types 5/6/7 | `int64` |
| size | shares | `int32` |
| price | dollars × 10 000 (`2238200` = $223.82) | `int64` |
| direction | +1 buy order, −1 sell order | `int8` |

`..._orderbook_10.csv`: 40 columns `ask_px_1, ask_sz_1, bid_px_1, bid_sz_1, …, level 10`.
Row *i* is the book **after** message *i*. Empty levels are `9999999999 / 0` (ask)
and `-9999999999 / 0` (bid).

## Event types

| type | meaning | book action in `lobster.cpp` |
|---|---|---|
| 1 | new visible limit order | `add` |
| 2 | partial cancellation | `cancel(id, size)` |
| 3 | full deletion | `remove(id)` |
| 4 | execution of a visible order | `execute(id, size)` |
| 5 | execution of a hidden order | none (hidden depth is not in the visible book) |
| 6 | cross / auction trade | none |
| 7 | trading halt | none |

For types 4 and 5, `direction` is the side of the **resting** order that was
hit. The aggressor is on the opposite side. A buy market order therefore shows
up as type-4 rows with `direction = -1` (sell orders being executed).

## Edge cases found in the samples (and how we handle them)

1. **Exact time parsing.** `34200.017459617` has 14 significant digits; parsing it
   as float64 and multiplying by 1e9 can be off by a nanosecond. We split on `.`
   and use integer arithmetic (`data.parse_time_to_ns`, `replay_bench.cpp`).

2. **Orders that predate the file.** Types 2/3/4 frequently reference ids that
   never appeared as type 1: those orders were entered pre-market. We seed each
   level with the size shown in orderbook row 0 as *anonymous liquidity*
   (`PriceLevel::anon_qty`) and route unknown-id events to it. Message 0 is a
   hidden execution in every sample, so row 0 of the orderbook file is the
   opening state and replay starts at message 1.

3. **The level-k window (the big one).** A level-10 message file contains only
   events that touch the top 10 levels. When a level is pushed to level 11 it can
   be cancelled, reduced or added to with no message in the file; when it
   re-enters the top 10 our copy is stale. Concrete case, AMZN: the ask at
   2245400×100 is seeded from row 0, leaves the window at row 33, is deleted
   off-file, and would otherwise sit in our book as a phantom best ask from
   row 1413 onward. Measured on all five tickers:

   | mode | L1 exact | top-10 exact |
   |---|---|---|
   | message only | 0.8 – 5.2 % | ≈ 0 % |
   | + price-priority inference | 49 – 76 % | ≈ 0 % |
   | + snapshot resync at the window boundary | 100 % | 100 % |

   *Price-priority inference* (`ApplyOptions::price_priority_purge`) is sound
   and message-only: a visible trade at price *p* on side *s* proves nothing
   visible rests at a better price on *s*; a new order that rests at *p* proves
   nothing on the opposite side is priced through *p*. Any such level in our
   book must be a phantom and is purged.

   *Snapshot resync* (`lobster::resync`) reconciles our top-k with orderbook
   row *i* after applying message *i*: levels the snapshot has and we lack are
   added as anonymous liquidity; levels we hold inside the window that the
   snapshot lacks are purged; quantity differences adjust the anonymous part
   (or trim the youngest identified orders if identified qty alone exceeds the
   snapshot). It uses only contemporaneous information, so it is not look-ahead.
   Its counters (`resync_levels_added/purged/qty_adjusted`) quantify how much
   the truncation hides: a few thousand events per ticker-day.

   Message-only reconstruction is exact *inside* the window; the residual error
   is entirely at the boundary. With full-depth data (level 50/200 files) the
   boundary effects shrink accordingly.

4. **Hidden executions (type 5)** do not change the visible book but are trades:
   they feed trade statistics and the fill model (a hidden fill at our price
   while nobody is ahead of us means we would have filled first, since visible
   orders have priority over hidden at the same price).

5. **Type 7 / price −1 rows** appear only around halts; none in these samples.

6. **Same-timestamp bursts.** Many consecutive rows share a timestamp (one
   aggressive order sweeping several resting orders). The backtester processes
   history first on timestamp ties, so our order arriving "at the same time"
   sees the post-sweep book.
