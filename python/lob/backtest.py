"""Backtest runner: configuration -> BacktestResult -> tidy DataFrames and a report."""
from __future__ import annotations

import time
from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from . import _lob
from . import data as D

S = _lob.NS_PER_SEC


@dataclass
class RunSpec:
    ticker: str
    latency_us: float = 500.0
    passive_entry: bool = True
    entry_z: float = 2.0
    exit_z: float = 0.5
    window_events: int = 50
    zscore_window: int = 1000
    order_qty: int = 100
    max_position: int = 100
    order_ttl_s: float = 2.0
    max_hold_s: float = 60.0
    cooldown_ms: float = 100.0
    cancel_assumption: str = "ProRata"
    maker_fee: float = -0.0020
    taker_fee: float = 0.0030
    use_snapshot: bool = True
    label: str = ""

    def params(self) -> _lob.OfiStrategyParams:
        p = _lob.OfiStrategyParams()
        p.window_events = self.window_events; p.zscore_window = self.zscore_window
        p.entry_z = self.entry_z; p.exit_z = self.exit_z; p.order_qty = self.order_qty
        p.passive_entry = self.passive_entry
        p.order_ttl_ns = int(self.order_ttl_s * S); p.max_hold_ns = int(self.max_hold_s * S)
        p.cooldown_ns = int(self.cooldown_ms * 1e6)
        return p

    def config(self) -> _lob.BacktestConfig:
        c = _lob.BacktestConfig()
        c.order_latency_ns = int(self.latency_us * 1000)
        c.cancel_assumption = getattr(_lob.CancelAssumption, self.cancel_assumption)
        c.fees.maker_fee_per_share = self.maker_fee; c.fees.taker_fee_per_share = self.taker_fee
        c.max_position = self.max_position
        c.start_index = 1
        return c


@dataclass
class RunResult:
    spec: RunSpec
    metrics: dict
    fills: pd.DataFrame
    equity: pd.DataFrame
    stats: dict
    zscores: np.ndarray | None = None
    seconds: float = 0.0
    extra: dict = field(default_factory=dict)

    def row(self) -> dict:
        r = {"ticker": self.spec.ticker, "label": self.spec.label or ("passive" if self.spec.passive_entry else "aggressive"),
             "latency_us": self.spec.latency_us, "entry_z": self.spec.entry_z, "cancel": self.spec.cancel_assumption}
        r.update(self.metrics)
        r["forced_close"] = self.extra.get("forced_close")
        r["runtime_s"] = round(self.seconds, 3)
        return r


_CACHE: dict[str, tuple] = {}


def _inputs(ticker: str):
    if ticker not in _CACHE:
        msgs = D.load_messages(ticker)
        ob = D.load_orderbook(ticker)
        _CACHE[ticker] = (msgs, D.seed_from_orderbook_row(ob, 0), D.orderbook_arrays(ob))
    return _CACHE[ticker]


def run(spec: RunSpec, strategy: _lob.Strategy | None = None) -> RunResult:
    """Run one backtest. `strategy` defaults to the C++ OfiStrategy built from `spec`."""
    msgs, seed, snap = _inputs(spec.ticker)
    strat = strategy or _lob.OfiStrategy(spec.params())
    bt = _lob.Backtester(*msgs.as_args(), seed=seed, config=spec.config(), snapshot=snap if spec.use_snapshot else None)
    t0 = time.perf_counter()
    res = bt.run(strat)
    dt = time.perf_counter() - t0
    f = pd.DataFrame(res.fills())
    if len(f):
        f["price_usd"] = f.price / _lob.PRICE_SCALE
        f["time"] = pd.to_datetime(f.ts, unit="ns")
    s = pd.DataFrame(res.samples())
    s["time"] = pd.to_datetime(s.ts, unit="ns")
    z = strat.zscores() if isinstance(strat, _lob.OfiStrategy) else None
    return RunResult(spec, res.metrics.to_dict(), f, s, dict(res.stats()), z, dt,
                     {"forced_close": res.forced_close, "orders_submitted": res.orders_submitted,
                      "orders_rejected": res.orders_rejected, "orders_cancelled": res.orders_cancelled})


def sweep(specs: list[RunSpec]) -> pd.DataFrame:
    return pd.DataFrame([run(s).row() for s in specs])


def latency_sweep(ticker: str, latencies_us=(0, 50, 500, 5000, 50000), **kw) -> pd.DataFrame:
    return sweep([RunSpec(ticker, latency_us=l, label=f"lat{l}us", **kw) for l in latencies_us])


def adverse_selection(result: RunResult, replay_df: pd.DataFrame, horizon_s: float = 1.0) -> float:
    """Mean signed mid move (ticks) `horizon_s` after each fill: negative = we got picked off."""
    if result.fills.empty:
        return float("nan")
    mids = replay_df.dropna(subset=["mid"])
    ts = mids.ts_ns.to_numpy()
    mid = mids.mid.to_numpy()
    idx_now = np.searchsorted(ts, result.fills.ts.to_numpy(), side="right") - 1
    idx_fwd = np.minimum(np.searchsorted(ts, result.fills.ts.to_numpy() + int(horizon_s * S), side="right") - 1, len(ts) - 1)
    move = (mid[idx_fwd] - mid[idx_now]) * _lob.PRICE_SCALE / _lob.TICK
    return float(np.mean(move * result.fills.side.to_numpy()))


class PyOfiStrategy(_lob.Strategy):
    """The same OFI rules written in Python via the pybind11 trampoline.

    Exists to (a) show how to write a strategy in Python and (b) measure the cost
    of a per-event Python callback versus the C++ implementation.
    """

    def __init__(self, p: _lob.OfiStrategyParams):
        super().__init__()
        self.p = p
        self.e = np.zeros(p.window_events, dtype=np.int64); self.e_pos = 0; self.e_sum = 0; self.e_n = 0
        self.w = np.zeros(p.zscore_window); self.w_pos = 0; self.w_sum = 0.0; self.w_sumsq = 0.0; self.w_n = 0
        self.z = 0.0
        self.working = 0; self.exiting = False; self.entry_ts = 0; self.last_action = 0

    def on_start(self, ctx):
        self.last_action = ctx.now()

    def _update(self, e):
        self.e_sum += e - self.e[self.e_pos]; self.e[self.e_pos] = e
        self.e_pos = (self.e_pos + 1) % len(self.e); self.e_n = min(self.e_n + 1, len(self.e))
        w = float(self.e_sum); old = self.w[self.w_pos]
        self.w_sum += w - old; self.w_sumsq += w * w - old * old; self.w[self.w_pos] = w
        self.w_pos = (self.w_pos + 1) % len(self.w); self.w_n = min(self.w_n + 1, len(self.w))
        if self.w_n < len(self.w) or self.e_n < len(self.e):
            self.z = 0.0; return
        mean = self.w_sum / self.w_n; var = self.w_sumsq / self.w_n - mean * mean
        self.z = 0.0 if var < 1e-12 else (w - mean) / var ** 0.5

    def on_market(self, ctx, msg, e):
        self._update(e)
        if ctx.closing():
            return
        now = ctx.now(); p = self.p
        if self.working:
            o = ctx.order(self.working)
            if o is None or not o.open():
                self.working = 0
            elif not self.exiting and now - o.submit_ts > p.order_ttl_ns:
                ctx.cancel(self.working); self.working = 0
        pos = ctx.position()
        if pos != 0:
            if self.exiting and self.working:
                return
            faded = self.z < p.exit_z if pos > 0 else self.z > -p.exit_z
            if faded or now - self.entry_ts > p.max_hold_ns:
                if self.working:
                    ctx.cancel(self.working)
                self.working = ctx.submit_market(_lob.Side.Sell if pos > 0 else _lob.Side.Buy, abs(pos))
                self.exiting = True; self.last_action = now
            return
        if self.working or now - self.last_action < p.cooldown_ns:
            return
        b = ctx.book()
        if b.best_bid_price() == _lob.NO_BID or b.best_ask_price() == _lob.NO_ASK:
            return
        side = _lob.Side.Buy if self.z > p.entry_z else _lob.Side.Sell if self.z < -p.entry_z else None
        if side is None:
            return
        if p.passive_entry:
            px = b.best_bid_price() if side == _lob.Side.Buy else b.best_ask_price()
            self.working = ctx.submit_limit(side, px, p.order_qty, _lob.TimeInForce.GTC)
        else:
            px = b.best_ask_price() if side == _lob.Side.Buy else b.best_bid_price()
            self.working = ctx.submit_limit(side, px, p.order_qty, _lob.TimeInForce.IOC)
        self.exiting = False; self.last_action = now

    def on_fill(self, ctx, f):
        if ctx.position() != 0 and self.entry_ts == 0:
            self.entry_ts = f.ts
        if ctx.position() == 0:
            self.entry_ts = 0; self.exiting = False


def markouts(result: RunResult, replay_df: pd.DataFrame, horizons_s=(0.1, 1.0, 5.0, 30.0)) -> pd.DataFrame:
    """Per fill type: price paid vs mid at the fill (ticks, + = better than mid) and the
    signed mid move after the fill at several horizons (ticks, + = in our favour)."""
    if result.fills.empty:
        return pd.DataFrame()
    f = result.fills.copy()
    mids = replay_df.dropna(subset=["mid"])
    ts = mids.ts_ns.to_numpy(); mid = mids.mid.to_numpy()
    side = f.side.to_numpy()
    i0 = np.searchsorted(ts, f.ts.to_numpy(), side="right") - 1
    f["vs_mid_ticks"] = (mid[i0] - f.price.to_numpy() / _lob.PRICE_SCALE) * _lob.PRICE_SCALE / _lob.TICK * side
    for h in horizons_s:
        i1 = np.minimum(np.searchsorted(ts, f.ts.to_numpy() + int(h * S), side="right") - 1, len(ts) - 1)
        f[f"markout_{h:g}s"] = (mid[i1] - mid[i0]) * _lob.PRICE_SCALE / _lob.TICK * side
    f["kind"] = np.where(f.passive.astype(bool), "passive (entries)", "aggressive (exits)")
    cols = ["vs_mid_ticks"] + [f"markout_{h:g}s" for h in horizons_s]
    out = f.groupby("kind")[cols].mean().round(3)
    out["n_fills"] = f.groupby("kind").size()
    return out
