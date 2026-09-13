# Design

## 1. Goals

* Reconstruct the full L3 book (every order, its price, and its queue position)
  from LOBSTER message data, fast enough that a whole day is a fraction of a
  second, and verify it row-by-row against LOBSTER's own snapshots.
* Backtest a strategy against that history with realistic mechanics: our
  orders arrive late, queue behind existing orders, pay maker/taker fees, and
  never see the future.
* Keep the hot loops in C++17 and everything exploratory in Python, connected by
  pybind11 with zero-copy numpy hand-off.

## 2. Layout

```
cpp/include/lob/   engine headers (documented in place)      cpp/src/  implementations
bindings/          pybind11 module `lob._lob`                python/lob/  data, reconstruct, validate, signals, research, backtest
cpp/tests/         GoogleTest (29 tests, ASan/UBSan clean)   python/tests/ pytest (8 tests)
cpp/bench/         replay_bench                              scripts/  fetch, convert, validate_all, run_backtest, make_notebooks
docs/              this file, LOBSTER_FORMAT.md, STUDY_GUIDE.md   notebooks/ 01 data+book, 02 OFI, 03 backtest
```

## 3. Order book (`order_book.hpp`)

```
              BidSide: std::map<Price, PriceLevel, greater<>>      AskSide: std::map<Price, PriceLevel, less<>>
              ┌──────────┐                                          ┌──────────┐
   best ───►  │ 2237500  │ head ─► Order ─► Order ─► Order ◄─ tail  │ 2239500  │ ...
              │ total 74 │         ▲ prev/next intrusive links       │ anon 100 │
              ├──────────┤         │                                 ├──────────┤
              │ 2236500  │         │                                 │ 2239600  │
              └──────────┘         │                                 └──────────┘
                                   │
   std::unordered_map<OrderId, Order*> ──── O(1) lookup ─┘        OrderPool: 64k-node slabs + free list
```

* **Order** is an intrusive doubly-linked node with a back-pointer to its level.
  Cancel/execute = hash lookup → `Order*` → `level->erase(o)`: O(1), no map
  lookup, no allocation.
* **PriceLevel** keeps `total_qty` (identified orders), `anon_qty` (liquidity we
  know exists but cannot attribute to ids), `num_orders`, and the FIFO queue.
  `qty_ahead(o)` walks predecessors; it is only called when a *simulated* order
  joins a queue, not on the hot path.
* **BookSide** is an ordered `std::map` so `begin()` is the best level and
  `depth(k)` is a k-step walk. Nodes never move, so `PriceLevel*` stays valid.
  Empty levels are erased immediately so `best()` is always O(1).
* **OrderPool** hands out `Order` nodes from contiguous slabs and recycles them
  through a free list threaded through `next`. Pointer stability is what makes
  raw pointers in the index and the lists safe.
* **Errors are counters, not exceptions** (`BookStats`): duplicate ids,
  over-sized cancels, anonymous underflow. Data quirks must not abort a replay.
* Complexity: add O(log L) for the level lookup (L ≈ hundreds of live levels)
  and O(1) otherwise; cancel/execute/remove O(1) (+ O(log L) only when a level
  empties). Measured: 30–38 ns per message end-to-end on an M4 Pro, i.e.
  26–34 M messages/s (`replay_bench`, message-only mode).

### Window maintenance (level-k data)

See `LOBSTER_FORMAT.md` §3. `purge_better_than`, `purge_level`,
`set_level_visible` implement the two remedies; `lobster::apply` applies the
price-priority rule, `lobster::resync` the snapshot reconciliation. Both are
optional flags so the message-only behaviour can be measured.

## 4. Replay (`replay.hpp`)

`replay(MessageColumns, seed, ReplayOptions)` applies every message and writes
per-event arrays: best bid/ask price and size, the CKS per-event OFI term,
trade size/side, and optionally the top-k snapshot after each message (used by
the validator). The Python side never loops over messages.

## 5. Backtester (`backtester.hpp`)

```
 historical messages (exchange ts) ──┐
                                     ├─► pop earlier ──► apply to book ──► fill model ──► portfolio ──► strategy.on_market
 action heap (arrival ts) ───────────┘        ▲                                                            │
     Submit / Cancel / Timer                  └──────────── submit/cancel stamped now()+latency ◄──────────┘
```

* **Two streams, one clock.** Historical messages and our own actions are
  merged by timestamp. Ties go to history (our order arriving "at the same
  time" as a message lands after it).
* **Latency.** The strategy observes the book at exchange time *t* and anything
  it sends arrives at *t + md_latency + order_latency*. Zero latency lets you
  trade on the state that produced the signal, which is look-ahead bias; the
  latency sweep in notebook 03 shows the effect on the aggressive variant.
* **Simulated orders never enter the real book.** History is what actually
  happened; the fill model asks "would this order have been filled by it?"
* **Fill model** (`fill_model.hpp`): passive orders join behind the visible
  size at their price; executions at that price deplete the queue ahead first;
  executions at a *worse* price on our side fill us (price priority); opposite
  orders priced through us fill us; hidden executions at our price fill us if
  nobody is ahead; cancels ahead of us are handled under a selectable
  assumption (pessimistic / pro-rata / optimistic). Aggressive orders walk the
  opposite depth at arrival and pay each level's price; IOC remainders expire,
  GTC remainders rest. Market impact is not modelled: our aggressive fills do
  not remove liquidity from the historical book.
* **Costs** (`costs.hpp`): maker rebate / taker fee per share (defaults −$0.0020
  / +$0.0030, NASDAQ 2012 order of magnitude) plus optional commission in bps.
* **Portfolio** (`portfolio.hpp`): position, cash, average cost, realized PnL;
  equity marked to mid and sampled every second.
* **Close.** One minute before 16:00 (configurable) all open orders are
  cancelled and the position is flattened with a market order through the fill
  model; if the book is empty the fallback closes at the last mid and flags
  `forced_close`. If the data ends earlier, the flatten fires at the last message.
* **Metrics** (`metrics.hpp`): total/realized PnL, fees, max drawdown, Sharpe
  from per-sample equity changes (annualized; treat intraday Sharpe with
  suspicion), fill rate, passive/aggressive fill counts, PnL per share.

## 6. Strategy interface

`Strategy` has `on_start / on_market(ctx, msg, ofi_e) / on_fill / on_timer /
on_end`. `StrategyContext` exposes `now()`, `book()`, `position()`,
`submit_limit`, `submit_market`, `cancel`, `schedule_timer`, `order(id)`,
`open_orders()`. The built-in `OfiStrategy` is C++; `python/lob/backtest.py`
contains the same rules in Python via the trampoline. Fills are identical; the
Python version is ~14× slower on AMZN (one callback per event).

## 7. pybind11 boundary (`bindings/module.cpp`)

* Inputs: `py::array_t<T, c_style | forcecast>` for the six columns; contiguous
  arrays of the right dtype are read in place (zero copy), anything else is
  converted once.
* Outputs: result vectors are moved to the heap and exposed as numpy arrays
  whose base object is a capsule that deletes the vector: no copy back.
* GIL is released for the duration of `replay` and `Backtester.run`; Python
  strategy callbacks re-acquire it per call.
* `PyStrategy` is a hand-written trampoline (the context must be passed as a
  pointer so pybind11 wraps it as a reference instead of copying).
* `Backtester` keeps the numpy arrays alive for its own lifetime; `run` uses
  `keep_alive<1,2>` so the strategy outlives the call.

## 8. Data pipeline (`python/lob/data.py`)

CSV → Parquet with an explicit schema (`ts_ns:int64, type:int8, order_id:int64,
size:int32, price:int64, direction:int8`), snappy compression, 64k-row groups,
file-level metadata (ticker, date, units). Sizes: 8–15× smaller than CSV, load in
tens of milliseconds, columns come out contiguous and typed for the engine.

## 9. Testing

* GoogleTest: intrusive list, book operations, 300k-op fuzz against a
  `std::map` reference, LOBSTER semantics, replay features/OFI, purge and
  resync, fill model scenarios, backtester scenarios (queue depletion, latency,
  position cap, sampling/forced close, OFI strategy end-to-end). Also run under
  ASan/UBSan (`-DLOB_SANITIZE=ON`).
* pytest: bindings, replay dtype handling, Python trampoline, latency test,
  OFI strategy on the real AMZN day, validation exactness.
* `scripts/validate_all.py`: the row-by-row comparison against LOBSTER's
  orderbook file in all three reconstruction modes.

## 10. Known limitations / next steps

* Level-10 data cannot be reconstructed exactly from messages alone; the
  snapshot resync fixes it for this dataset but full-depth files are the real
  solution.
* No market impact on aggressive fills; no partial hidden-liquidity model.
* Single day of data: nothing here is a statistically meaningful PnL claim.
* Possible upgrades: dense tick-indexed level array for O(1) `add`, open-
  addressing hash for the id index, multi-day runner, Arrow C++ reader so the
  engine can run without Python.
