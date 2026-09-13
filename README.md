# lob-backtester

L3 limit order book reconstruction from LOBSTER message data and an
event-driven backtester with queue-position fills, latency and transaction
costs. C++17 core exposed to Python through pybind11; Parquet data pipeline.

* `docs/DESIGN.md` — architecture, data structures, fill model, boundary
* `docs/LOBSTER_FORMAT.md` — the data format and the level-k window problem
* `docs/STUDY_GUIDE.md` — every design decision as an interview Q&A

## Quickstart

```bash
uv venv --python 3.11 .venv
uv pip install --python .venv/bin/python pybind11 scikit-build-core ninja numpy pandas pyarrow pytest matplotlib statsmodels jupyter
uv pip install --python .venv/bin/python -e . --no-build-isolation      # builds lob._lob

.venv/bin/python scripts/fetch_samples.py        # 5 tickers, 2012-06-21, level 10 (~600 MB CSV)
.venv/bin/python scripts/convert.py              # -> data/parquet (8-15x smaller)
.venv/bin/python scripts/validate_all.py         # reconstruction vs LOBSTER snapshots
.venv/bin/python scripts/run_backtest.py --ticker AMZN --latency-us 500
.venv/bin/python scripts/run_backtest.py --all --latency-sweep

# C++ tests and benchmark
cmake -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=$PWD/.venv/bin/ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ./build/lob_tests && ./build/replay_bench data/raw/AMZN_2012-06-21_34200000_57600000_message_10.csv
.venv/bin/python -m pytest python/tests
```

## Results (2012-06-21 samples, Apple M4 Pro)

Reconstruction vs LOBSTER orderbook file, all rows, all 5 tickers:

| mode | L1 exact | top-10 exact |
|---|---|---|
| message replay only | 0.8–5.2 % | ≈0 % |
| + price-priority inference (message-only) | 49–76 % | ≈0 % |
| + snapshot resync at the 10-level window boundary | **100 %** | **100 %** |

Why message-only cannot be exact: a level-10 file omits every event outside the
top 10 levels (see `docs/LOBSTER_FORMAT.md`).

Throughput: 26–34 M messages/s pure replay (30–38 ns/msg); full-day OFI
backtest with the C++ strategy in ~40 ms; the same strategy in Python via the
trampoline gives identical fills at ~14× the runtime.

OFI (Cont–Kukanov–Stoikov) contemporaneous regression, 10 s buckets:

| ticker | slope (ticks/share) | HAC t | R² |
|---|---|---|---|
| AMZN | 2.5e-3 | 3.9 | 0.32 |
| AAPL | 4.1e-3 | 9.1 | 0.41 |
| GOOG | 5.6e-3 | 2.6 | 0.16 |
| INTC | 1.9e-5 | 21.4 | 0.65 |
| MSFT | 2.0e-5 | 19.6 | 0.72 |

Naive OFI threshold strategy (passive entry, 500 µs latency): loses ~3 ¢/share on
AMZN/AAPL and ~0.3 ¢/share on INTC/MSFT. Mark-outs show adverse selection on
passive fills (−2 ticks after 1 s on AMZN) plus half a 13-tick spread on exits.
The signal is real; the naive rule is not tradeable. Details in notebook 03.

## Layout

```
cpp/include/lob/  types, order, price_level, order_pool, book_side, order_book, lobster, replay,
                  event, fill_model, costs, portfolio, strategy, ofi_strategy, backtester, metrics
cpp/src/          implementations           cpp/tests/  GoogleTest      cpp/bench/  replay_bench
bindings/         pybind11 module           python/lob/ data, reconstruct, validate, signals, research, backtest
scripts/          fetch_samples, convert, validate_all, run_backtest, make_notebooks
notebooks/        01 data+book, 02 OFI regression, 03 backtest results
```
