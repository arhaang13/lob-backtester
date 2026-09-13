"""Order-flow features built from the per-event replay output."""
from __future__ import annotations

import numpy as np
import pandas as pd

from . import _lob

TICK = _lob.TICK


def bucket_ofi(df: pd.DataFrame, bucket_s: float = 10.0) -> pd.DataFrame:
    """Aggregate per-event OFI terms into fixed time buckets.

    For bucket k: OFI_k = sum of e_n over events in k; dmid_k = (mid at end of k
    - mid at end of k-1) in ticks; depth_k = mean best-level size (for the CKS
    depth scaling). Buckets with no events are dropped.
    """
    d = df.dropna(subset=["mid"]).copy()
    bucket_ns = int(bucket_s * _lob.NS_PER_SEC)
    d["bucket"] = (d.ts_ns // bucket_ns) * bucket_ns
    g = d.groupby("bucket", sort=True)
    out = pd.DataFrame({
        "ofi": g.ofi_e.sum(),
        "mid_end": g.mid.last(),
        "depth": (g.bid_qty.mean() + g.ask_qty.mean()) / 2.0,
        "n_events": g.size(),
        "trade_qty": g.trade_qty.sum(),
    })
    out["dmid_ticks"] = out.mid_end.diff() * _lob.PRICE_SCALE / TICK
    out["dmid_next_ticks"] = out.dmid_ticks.shift(-1)          # for predictive (not contemporaneous) tests
    return out.dropna(subset=["dmid_ticks"])


def queue_imbalance(df: pd.DataFrame) -> pd.Series:
    """(Qb - Qa) / (Qb + Qa) at the touch, in [-1, 1]."""
    tot = df.bid_qty + df.ask_qty
    return ((df.bid_qty - df.ask_qty) / tot.where(tot > 0)).astype(float)


def rolling_ofi(df: pd.DataFrame, window_events: int) -> pd.Series:
    return df.ofi_e.rolling(window_events, min_periods=window_events).sum()


def forward_mid_change(df: pd.DataFrame, horizon_events: int) -> pd.Series:
    """Mid change `horizon_events` events ahead, in ticks (what a signal should predict)."""
    return (df.mid.shift(-horizon_events) - df.mid) * _lob.PRICE_SCALE / TICK
