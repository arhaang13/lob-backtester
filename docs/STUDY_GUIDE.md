# Study guide: every design decision as an interview question

Numbers quoted below come from your own runs on the LOBSTER 2012-06-21 samples
(Apple M4 Pro). Re-run `scripts/validate_all.py`, `build/replay_bench`, and
notebook 03 to refresh them before an interview.

---

## A. Market microstructure basics

**Q: What are L1, L2, L3 data?**
L1: best bid/ask price and size. L2: aggregated size at each price level
(depth). L3: every individual order with its own id, so you know the queue and
can track a specific order's position. LOBSTER message files are L3 (each row
names an order id); its orderbook files are L2 snapshots.

**Q: What does price-time priority mean and where is it in your code?**
Orders are matched by best price first, then by arrival time within a price.
Each `PriceLevel` is a FIFO queue (head = oldest = fills first). The fill model
uses it: a simulated order joins the back (`queue_ahead = visible qty at that
price`) and only fills after that quantity has traded or left.

**Q: Maker vs taker?**
A resting limit order that gets filled *adds* liquidity (maker) and typically
earns a rebate; an order that crosses the spread *removes* liquidity (taker)
and pays a fee. Defaults here: −$0.0020 / +$0.0030 per share (2012 NASDAQ order
of magnitude). Passive fills are flagged `passive=true` and priced with the
maker rate.

**Q: Hidden orders?**
Non-displayed liquidity. LOBSTER reports their executions (type 5) but they are
never in the visible book. They lose priority to visible orders at the same
price, so a hidden fill at our price while nobody visible is ahead of us means
we would have filled first.

**Q: Why is the message `direction` on an execution the resting side?**
LOBSTER reports what happened to the *book*: order X on side s was executed.
The aggressor is implicit (opposite side). A buy market order shows up as one
or more type-4 rows with `direction = -1`.

---

## B. The order book data structure

**Q: Describe your data structure and its complexity.**
Three parts. (1) A price-level map per side: `std::map<Price, PriceLevel>` with
`greater<>` for bids and `less<>` for asks so `begin()` is the best level.
(2) In each level an intrusive doubly-linked FIFO of `Order` nodes.
(3) `std::unordered_map<OrderId, Order*>` as the id index. Add is O(log L) for
the level lookup (L ≈ hundreds of live levels) then O(1); cancel, execute and
delete are O(1): hash → `Order*` → its `PriceLevel*` back-pointer → unlink.
Erasing an emptied level is an extra O(log L). Best bid/ask is O(1).

**Q: Why intrusive lists instead of `std::list`?**
`std::list` allocates a separate node per element and you need the *iterator*
to erase in O(1). With prev/next inside `Order`, the pointer already stored in
the hash index is enough to unlink, there is no second allocation, and the
`Order` and its links share a cache line. The order *is* the node.

**Q: Why `std::map` and not a hash map or an array for levels?**
We need ordered iteration (best first, depth walks) and stable node addresses
(orders hold `PriceLevel*`). A hash map has no order. A dense array indexed by
tick gives O(1) find-or-create but costs memory proportional to the price range
and needs a scan for the next best level when the top empties. `std::map` was
the simplest correct choice; the tick-array is the documented upgrade if `add`
ever dominated a profile (it does not: 30–38 ns per message end-to-end).

**Q: Why a pool allocator?**
~270k–670k messages a day means hundreds of thousands of `new`/`delete`. The
pool hands out nodes from 64k-node slabs and recycles through a free list, so
allocation is O(1), memory is contiguous, and crucially pointers never move
(slabs are never reallocated). Pointer stability is what makes raw pointers in
the index and the lists safe.

**Q: How do you avoid dangling pointers when a level empties?**
The level is erased only after the order is unlinked and returned to the pool;
`PriceLevel` lives inside the map node, and `std::map` nodes are stable until
erased. We never keep iterators across erasures.

**Q: How did you test it?**
Unit tests for each operation and edge case (duplicate id, over-sized cancel,
best-level removal restoring the next level), a structural invariant checker
(level totals equal the sum of order quantities, lists are consistent, index
size equals pool live count), a 300k-operation fuzz against a naive
`std::map` reference book, and the whole suite under ASan/UBSan.

**Q: Where do integers come from and why never doubles?**
Prices are LOBSTER's `dollars × 10 000` (a tick = 100 units), times are
nanoseconds since midnight in `int64`. Exact equality is needed for level
lookup and validation; doubles would make `220.13` a lookup hazard. The mid is
kept as `bid + ask` (twice the mid) until converted for display.

---

## C. LOBSTER-specific reconstruction

**Q: What is anonymous liquidity in your book?**
Orders placed before 09:30 never appear as type-1 messages, but their cancels
and executions do, with ids we have never seen. We seed each top-10 level with
the size in orderbook row 0 as `anon_qty` and route unknown-id events to it.
Visible size = identified + anonymous, and anonymous quantity counts as
"ahead" of any simulated order (it is older than anything we saw arrive).

**Q: Did your reconstruction match LOBSTER's snapshots?**
Not at first, and the reason is the most useful thing I learned: a level-10
message file omits every event outside the top 10 levels. A level pushed to
level 11 can be changed off-file; when it comes back our copy is stale. Pure
message replay matched L1 on only 1–5 % of rows. Two fixes:
(1) price-priority inference, message-only and provably sound: a trade at
price p on side s means nothing visible rests at a better price on s; a new
order resting at p means nothing on the other side is priced through it.
Purging such phantoms lifted L1 exactness to 49–76 %. (2) Reconciling the
window boundary with the snapshot row after each message (levels entering or
leaving the top 10) makes it exact: 100 % of rows, all ten levels, all five
tickers. The resync uses only contemporaneous rows, so it is not look-ahead,
and its counters quantify how much the truncation hides (thousands of
boundary events per day).

**Q: Why not just use the orderbook file?**
The orderbook file is L2 and has no order ids: you cannot know queue position,
order ages, or which order a cancel hit. The message file gives the flow; the
snapshot gives the boundary condition. Using both is the honest reconstruction
for level-limited data.

**Q: How do you parse `34200.017459617` exactly?**
Split on the decimal point and use integer arithmetic. Parsing as float64 and
multiplying by 1e9 can be off in the last digit (14 significant digits, and
parsers round differently). Both the Python converter and the C++ bench do it
this way.

---

## D. Backtester

**Q: Event-driven vs vectorised backtest?**
Vectorised: compute a signal series, assume fills at the observed price, sum
returns. Fast, but it cannot represent queue position, partial fills, latency,
or order state, and it silently assumes every limit order is filled. Event
driven: replay each message, keep order state machines, decide fills from what
actually traded. Slower, but the only way to answer "would this order have
been filled, when, and at what price". Here the C++ event loop runs a full
AMZN day in ~40 ms, so the speed argument disappears.

**Q: How do you model latency and why does it matter?**
The strategy observes the book at exchange time t; any action it sends reaches
the exchange at t + (market-data latency + order latency). Market messages with
timestamps up to the arrival time are applied first, so the order sees the book
as it was on arrival. With zero latency you react to a trade and hit liquidity
that existed at the same nanosecond, which is look-ahead. The passive strategy
is nearly latency-insensitive (you join the back of the queue either way); the
aggressive variant is not.

**Q: Walk me through a passive fill.**
Our bid at p arrives; `queue_ahead` = visible size at p. Every later message is
checked: an execution on the bid side at p first depletes `queue_ahead`, the
excess fills us; an execution on the bid side at a *lower* price means the
seller went through our price, so we fill; a new sell order priced at or below
p would have hit us, so we fill; a cancel at p ahead of us reduces
`queue_ahead` under a chosen assumption (pessimistic: none of it was ahead;
pro-rata: proportional; optimistic: all of it). If the level trades through and
we still have quantity, we keep resting.

**Q: What assumptions are you making that a real venue would not?**
No market impact: our aggressive fills do not remove liquidity from the
historical book, and our resting order does not change other participants'
behaviour. The cancel-ahead assumption is a knob because it is unknowable from
the data. Hidden liquidity ahead of us at our price is ignored.

**Q: How is PnL computed?**
Cash and position per fill; realized PnL via average cost; equity = cash +
position × mid, sampled every second; fees applied per fill. One minute before
the close all orders are cancelled and the position is flattened with a market
order through the same fill model (so the close pays the spread and taker fee).

**Q: Your Sharpe numbers look absurd. Why?**
Annualizing one day of 1-second equity changes multiplies a tiny mean/std by
√(23 400 × 252). A single day cannot support an annual Sharpe; report PnL per
share, mark-outs and hit rates instead, and treat Sharpe as a comparative
number across configurations only.

---

## E. The signal

**Q: What is order flow imbalance and why did you use it?**
Cont, Kukanov & Stoikov (2014). Per event, the change in bid-side demand minus
the change in ask-side supply at the touch: size added at or above the old best
bid counts positive, size removed counts negative, symmetrically for the ask.
Summed over a window it explains contemporaneous mid changes linearly, with a
slope inversely proportional to depth. It is cheap to compute in the replay
loop (needs only the previous and current L1) and it has a published
theoretical basis, unlike ad-hoc "buy pressure" indicators.

**Q: What did you find?**
10-second buckets, 2012-06-21: slope positive and significant on all five names
(HAC t-stats 3.9 AMZN, 9.1 AAPL, 2.6 GOOG, 21 INTC, 20 MSFT; R² 0.16–0.72,
higher for the large-tick, deep-book names). Slope × depth is roughly constant
within the small-tick and large-tick groups, as CKS predict. R² increases with
bucket size. Event-time *predictive* correlations of windowed OFI with the next
10–200 events' mid change are positive but small, 0.03–0.10.

**Q: So did the strategy make money?**
No, and it should not have. A naive threshold rule (enter when the rolling OFI
z-score exceeds 2, exit when it fades) loses ~3 cents per share on AMZN and
AAPL, 0.3 cents on INTC/MSFT. Mark-outs show why: passive entries fill about
2 ticks inside a 13-tick spread and the mid then moves against them (−2 ticks
after 1 s, −6 after 30 s), i.e. we are filled when an informed sweep reaches
our level; exits pay half the spread. The contemporaneous relationship is
strong but the predictive edge is smaller than the spread. The backtester is
doing its job: a vectorised test that fills every limit order would have shown
a profit.

**Q: What would you try next?**
Trade only when the spread is one tick (INTC/MSFT-like names), condition on
queue position rather than time, use OFI to time *exits* of an inventory-driven
market maker rather than as a directional entry, and test across many days
before believing anything.

---

## F. Python / C++ boundary

**Q: How does data cross into C++ without copying?**
pyarrow reads Parquet into typed, contiguous columns; numpy exposes them via
the buffer protocol; pybind11 `array_t<T, c_style | forcecast>` gives the C++
side a raw pointer when dtype and layout already match (otherwise one converted
copy). Results are `std::vector`s moved to the heap and wrapped in numpy arrays
whose base is a capsule that deletes the vector, so nothing is copied back.

**Q: What about the GIL?**
Released for the whole replay / backtest run. Python strategy callbacks
re-acquire it per call through the trampoline. A Python strategy costs about
one microsecond per event versus tens of nanoseconds in C++: 14× slower on the
AMZN day here, with identical fills.

**Q: What is a trampoline and why did you write it by hand?**
A C++ subclass whose virtual overrides look up the Python override and call
it. pybind11's `PYBIND11_OVERRIDE` macro casts reference arguments with the
`copy` policy; the backtester context is not copyable, so it must cross as a
pointer, which the macro cannot express for the base-call fallback.

**Q: Why Parquet?**
Columnar (we read a few columns at a time), typed (int64 nanoseconds, not a
float string), compressed (8–15× smaller than CSV here), and pyarrow gives
zero-copy numpy columns. Row groups of 64k rows keep it streamable; file
metadata records ticker, date and units.

---

## G. Performance

**Q: Numbers?**
Message-only replay: AMZN 269 748 messages in ~10 ms (38 ns/msg, 26 M msg/s);
MSFT 668 765 in ~20 ms (30 ns/msg, 34 M msg/s). With snapshot resync and 10-level
output from Python: 0.1–0.4 s per ticker-day including the numpy hand-off.
Full OFI backtest with the C++ strategy: ~40 ms per day.

**Q: What made it fast / what would you do next?**
Integer keys, no allocation in the hot path (pool), O(1) cancel/execute via the
id index and intrusive unlink, counters instead of exceptions, results written
into preallocated arrays, GIL released. Next: dense tick-indexed levels, an
open-addressing hash map for ids (reserve once), and profile-guided cache
alignment of `Order` (currently 56 bytes).

---

## H. Honest limitations to volunteer

* One trading day of free sample data; nothing here is a PnL claim.
* Level-10 window: exact reconstruction needs the snapshot boundary condition.
* No market impact, no hidden-liquidity queue model, single venue.
* The strategy is a demonstration vehicle for the backtester, not alpha.
