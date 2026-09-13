#!/usr/bin/env python
"""Run the OFI strategy backtest.

Examples:
  python scripts/run_backtest.py --ticker AMZN --latency-us 500
  python scripts/run_backtest.py --all --latency-sweep
  python scripts/run_backtest.py --ticker MSFT --aggressive --entry-z 2.5
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from lob import data as D  # noqa: E402
from lob.backtest import RunSpec, latency_sweep, run  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--ticker", default="AMZN")
ap.add_argument("--all", action="store_true", help="run every ticker with parquet files")
ap.add_argument("--latency-us", type=float, default=500.0)
ap.add_argument("--aggressive", action="store_true", help="cross the spread on entry instead of joining the queue")
ap.add_argument("--entry-z", type=float, default=2.0)
ap.add_argument("--exit-z", type=float, default=0.5)
ap.add_argument("--window", type=int, default=50)
ap.add_argument("--qty", type=int, default=100)
ap.add_argument("--cancel", default="ProRata", choices=["Pessimistic", "ProRata", "Optimistic"])
ap.add_argument("--no-snapshot", action="store_true", help="message-only book (no window resync)")
ap.add_argument("--latency-sweep", action="store_true")
ap.add_argument("--out", default=None, help="write results parquet/csv to this path")
a = ap.parse_args()

tickers = [d.ticker for d in D.find_days()] if a.all else [a.ticker]
import pandas as pd  # noqa: E402

pd.set_option("display.width", 200); pd.set_option("display.max_columns", 30)
rows = []
for t in tickers:
    kw = dict(passive_entry=not a.aggressive, entry_z=a.entry_z, exit_z=a.exit_z, window_events=a.window,
              order_qty=a.qty, max_position=a.qty, cancel_assumption=a.cancel, use_snapshot=not a.no_snapshot)
    if a.latency_sweep:
        df = latency_sweep(t, **kw)
        rows.append(df)
    else:
        r = run(RunSpec(t, latency_us=a.latency_us, **kw))
        rows.append(pd.DataFrame([r.row()]))
out = pd.concat(rows, ignore_index=True)
cols = ["ticker", "label", "latency_us", "total_pnl", "sharpe", "max_drawdown", "n_orders", "n_fills", "fill_rate",
        "n_passive_fills", "n_aggressive_fills", "volume", "fees", "pnl_per_share", "forced_close", "runtime_s"]
print(out[cols].to_string(index=False, float_format=lambda x: f"{x:,.4f}"))
if a.out:
    (out.to_parquet if a.out.endswith(".parquet") else out.to_csv)(a.out, index=False)
    print("wrote", a.out)
