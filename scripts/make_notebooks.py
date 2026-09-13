#!/usr/bin/env python
"""Generate the three research notebooks (kept as code so they are reproducible)."""
from pathlib import Path

import nbformat as nbf

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "notebooks"
OUT.mkdir(exist_ok=True)

SETUP = '''import sys, time
from pathlib import Path
ROOT = Path.cwd().resolve().parent if Path.cwd().name == "notebooks" else Path.cwd().resolve()
sys.path.insert(0, str(ROOT / "python"))
import numpy as np, pandas as pd
import matplotlib.pyplot as plt
pd.set_option("display.width", 200); pd.set_option("display.max_columns", 40)
import lob
from lob import data as D
from lob.reconstruct import replay_day
RESULTS = ROOT / "results"; RESULTS.mkdir(exist_ok=True)
print("lob", lob.__version__, "| tickers:", [d.ticker for d in D.find_days()])'''


def nb(cells, path):
    n = nbf.v4.new_notebook()
    n.cells = [nbf.v4.new_markdown_cell(c[1]) if c[0] == "md" else nbf.v4.new_code_cell(c[1]) for c in cells]
    n.metadata["kernelspec"] = {"name": "python3", "display_name": "Python 3", "language": "python"}
    nbf.write(n, path)
    print("wrote", path)


# ----------------------------------------------------------------------------- 01
nb([
("md", """# 01 · Data and book reconstruction

LOBSTER sample day 2012-06-21 for AMZN, AAPL, GOOG, INTC, MSFT. This notebook
loads the Parquet files, replays a day through the C++ book, checks the
reconstruction against LOBSTER's own orderbook file, and looks at the L1 series.

Key idea from `docs/LOBSTER_FORMAT.md`: a *level-10* message file omits every event
outside the top 10 levels, so a pure message replay drifts once a level leaves and
re-enters the window. Three modes are compared below."""),
("code", SETUP),
("code", '''msgs = D.load_messages("AMZN")
df = msgs.to_frame()
print(len(df), "messages;", df.ts_ns.min()/1e9, "->", df.ts_ns.max()/1e9, "s after midnight")
df.type.value_counts().sort_index().rename({1:"1 new",2:"2 partial cancel",3:"3 delete",4:"4 exec visible",5:"5 exec hidden",6:"6 cross",7:"7 halt"})'''),
("code", '''from lob.validate import validate
reports = {t: validate(t) for t in ["AMZN","AAPL","GOOG","INTC","MSFT"]}
rows = [{"ticker": t, "mode": m, "top10_exact_%": r.exact_pct, "L1_exact_%": r.l1_pct, "first_mismatch": r.first_mismatch}
        for t, rep in reports.items() for m, r in rep.modes.items()]
pd.DataFrame(rows).pivot(index="ticker", columns="mode", values=["L1_exact_%","top10_exact_%"]).round(2)'''),
("md", """`snapshot_resync` is exact by construction; its counters say how much the
10-level truncation hides (levels entering/leaving the window, quantities changed off-file)."""),
("code", '''pd.DataFrame({t: {k: v for k, v in rep.modes["snapshot_resync"].stats.items() if k.startswith("resync") or k in ("unknown_id","priority_purges")}
              for t, rep in reports.items()}).T'''),
("code", '''feat, raw = replay_day("AMZN", depth_k=10)
t = pd.to_datetime(feat.ts_ns, unit="ns")
fig, ax = plt.subplots(3, 1, figsize=(12, 8), sharex=True)
ax[0].plot(t, feat.best_bid/1e4, lw=.5, label="bid"); ax[0].plot(t, feat.best_ask/1e4, lw=.5, label="ask"); ax[0].legend(); ax[0].set_title("AMZN L1")
ax[1].plot(t, feat.spread_ticks, lw=.4); ax[1].set_ylabel("spread (ticks)")
ax[2].plot(t, feat.bid_qty, lw=.4, label="bid qty"); ax[2].plot(t, feat.ask_qty, lw=.4, label="ask qty"); ax[2].legend(); ax[2].set_ylabel("shares")
fig.tight_layout(); fig.savefig(RESULTS/"amzn_l1.png", dpi=110)
feat[["mid","spread_ticks","bid_qty","ask_qty"]].describe().round(2)'''),
("code", '''# Throughput of the pure C++ replay from Python (includes numpy hand-off)
for t in ["AMZN","MSFT"]:
    m = D.load_messages(t)
    t0 = time.perf_counter(); r = lob.replay(*m.as_args()); dt = time.perf_counter()-t0
    print(f"{t}: {len(m):,} msgs in {dt*1e3:.1f} ms -> {len(m)/dt/1e6:.1f} M msgs/s")'''),
], OUT / "01_data_and_book.ipynb")

# ----------------------------------------------------------------------------- 02
nb([
("md", """# 02 · Order-flow imbalance (Cont, Kukanov & Stoikov 2014)

Per-event term computed in C++ during replay:

$e_n = \\mathbb{1}[P^b_n \\ge P^b_{n-1}]\\,q^b_n - \\mathbb{1}[P^b_n \\le P^b_{n-1}]\\,q^b_{n-1} - \\mathbb{1}[P^a_n \\le P^a_{n-1}]\\,q^a_n + \\mathbb{1}[P^a_n \\ge P^a_{n-1}]\\,q^a_{n-1}$

CKS show that over a bucket, $\\Delta P_k \\approx \\beta\\,\\mathrm{OFI}_k$ with $\\beta \\propto 1/\\text{depth}$.
Two checks: the contemporaneous regression (does OFI explain price changes?) and an
event-time predictive check (does past OFI correlate with *future* mid changes?)."""),
("code", SETUP),
("code", '''from lob.research import bucket_sweep, ofi_regression, predictive_table, plot_ofi
pd.concat([bucket_sweep(t) for t in ["AMZN","AAPL","GOOG","INTC","MSFT"]]).round(5)'''),
("md", """`slope_x_depth` is roughly constant *within* the small-tick (AMZN/AAPL/GOOG) and
large-tick (INTC/MSFT) groups, which is the CKS depth-scaling prediction. R² rises
with the bucket size, as in the paper."""),
("code", '''for t in ["AMZN","INTC"]:
    plot_ofi(t, 10.0, RESULTS/f"ofi_{t.lower()}.png"); plt.show()'''),
("code", '''pred = pd.concat([predictive_table(t) for t in ["AMZN","AAPL","INTC"]])
pred.pivot_table(index=["ticker","signal"], columns="horizon_events", values="corr").round(4)'''),
("md", """Predictive correlations are positive but small (a few percent). Contemporaneous
explanatory power is much larger than predictive power, which is the first hint that a
naive OFI-threshold strategy will struggle net of spread and adverse selection."""),
], OUT / "02_ofi_regression.ipynb")

# ----------------------------------------------------------------------------- 03
nb([
("md", """# 03 · Event-driven backtest of an OFI threshold strategy

`OfiStrategy` (C++): rolling OFI over `window_events`, z-scored over `zscore_window`;
enter when |z| > `entry_z` (passive = join the touch, aggressive = cross); exit with a
market order when the signal fades below `exit_z` or after `max_hold`. Fills come from
the queue-position model; every action is delayed by latency."""),
("code", SETUP),
("code", '''from lob.backtest import RunSpec, run, sweep, latency_sweep, markouts, PyOfiStrategy
cols = ["ticker","label","latency_us","total_pnl","fees","n_orders","n_fills","fill_rate","volume","pnl_per_share","max_drawdown","forced_close","runtime_s"]
base = run(RunSpec("AMZN"))
pd.Series(base.row())'''),
("code", '''fig, ax = plt.subplots(2, 1, figsize=(12, 6), sharex=True)
ax[0].plot(base.equity.time, base.equity.equity, lw=.8); ax[0].set_ylabel("equity ($)"); ax[0].set_title("AMZN · passive OFI strategy · 500 µs latency")
ax[1].step(base.equity.time, base.equity.position, lw=.6); ax[1].set_ylabel("position")
fig.tight_layout(); fig.savefig(RESULTS/"amzn_equity.png", dpi=110)
feat, _ = replay_day("AMZN")
print("mean AMZN spread:", round(float(feat.spread_ticks.mean()), 1), "ticks")
markouts(base, feat)'''),
("md", """**Mark-outs** read like this: passive entries buy ~2 ticks inside the 13-tick spread
but the mid then moves *against* them (negative mark-outs growing with horizon): we get
filled precisely when someone informed is sweeping our side. Exits pay half the spread
(~6 ticks vs mid) and the mid keeps going the way we were fleeing from. Net: about
−6 ticks per round trip before fees, which the fee schedule barely changes."""),
("md", """## Latency
Passive entries barely depend on latency (we join the back of the queue either way).
Aggressive entries do: with zero latency you react to a print and hit liquidity that was
there *at the same nanosecond*, which nobody can do live. Because this strategy loses,
lower latency simply buys more fills of a bad signal, so PnL *improves* as latency grows;
the point is the dependence, not its sign. A profitable aggressive signal would show the
mirror image, which is exactly the look-ahead inflation a zero-latency backtest hides."""),
("code", '''lat = (0, 50, 500, 5000, 50000)
p = latency_sweep("AMZN", lat, passive_entry=True)[cols]; p["mode"] = "passive"
a = latency_sweep("AMZN", lat, passive_entry=False, entry_z=2.5)[cols]; a["mode"] = "aggressive"
both = pd.concat([p, a]); both'''),
("code", '''fig, ax = plt.subplots(figsize=(7,4))
for m, g in both.groupby("mode"): ax.plot(g.latency_us, g.total_pnl, marker="o", label=m)
ax.set_xscale("symlog", linthresh=10); ax.set_xlabel("one-way latency (µs)"); ax.set_ylabel("PnL ($)"); ax.legend(); ax.set_title("AMZN · PnL vs latency")
fig.tight_layout(); fig.savefig(RESULTS/"amzn_latency.png", dpi=110)'''),
("md", """## Queue-position assumption for cancellations ahead of us"""),
("code", '''sweep([RunSpec("AMZN", cancel_assumption=c, label=c) for c in ("Pessimistic","ProRata","Optimistic")])[cols]'''),
("md", """## All tickers, and gross vs net of fees"""),
("code", '''rows = []
for t in ["AMZN","AAPL","GOOG","INTC","MSFT"]:
    r = run(RunSpec(t, label="net")); rows.append(r.row() | {"gross_pnl": r.metrics["total_pnl"] + r.metrics["fees"]})
pd.DataFrame(rows)[cols + ["gross_pnl"]]'''),
("md", """## Threshold sweep (fewer, stronger signals)"""),
("code", '''sweep([RunSpec("AMZN", entry_z=z, label=f"z={z}") for z in (2.0, 2.5, 3.0, 4.0)])[cols]'''),
("md", """## Python strategy through the pybind11 trampoline
Same rules in Python (`PyOfiStrategy`). Fills must match the C++ version exactly; the
runtime difference is the cost of one Python callback per market event."""),
("code", '''spec = RunSpec("AMZN")
cpp = run(spec); py = run(spec, strategy=PyOfiStrategy(spec.params()))
same = cpp.fills[["ts","side","price","qty"]].reset_index(drop=True).equals(py.fills[["ts","side","price","qty"]].reset_index(drop=True))
print(f"identical fills: {same} | C++ {cpp.seconds*1e3:.0f} ms vs Python {py.seconds*1e3:.0f} ms ({py.seconds/cpp.seconds:.0f}x), {len(D.load_messages('AMZN')):,} events")'''),
("md", """## Reading the results
* The signal is real (t-stats of 4 to 21 contemporaneously) but the naive rule loses a few
  cents per share: passive entries get filled mostly when the price is about to move
  against them (mark-out about −2 ticks after 1 s, −6 after 30 s on AMZN), and exits
  pay half of a 13-tick spread.
* Fees are a small part of the loss; adverse selection dominates.
* This is the point of an event-driven backtester with realistic fills: a vectorised
  backtest that fills every limit order at its price would have shown a profit here."""),
], OUT / "03_backtest_results.ipynb")
