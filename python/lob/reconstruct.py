"""Parquet -> numpy -> C++ replay -> pandas. (Named `reconstruct` so it does not shadow the `lob.replay` function.)"""
from __future__ import annotations

import numpy as np
import pandas as pd

from . import _lob
from . import data as D


def replay_day(ticker: str, date: str = D.DATE, levels: int = D.LEVELS, *, depth_k: int = 0,
               seed: bool = True, snapshot: bool = True, price_priority_purge: bool = True) -> tuple[pd.DataFrame, dict]:
    """Replay one ticker-day through the C++ book.

    Returns (features DataFrame indexed by message row, raw dict with optional
    depth arrays and engine stats). With ``seed=True`` the book is pre-loaded
    from orderbook row 0 and replay starts at message 1 (row 0 is a hidden
    execution in every sample file, so row 0 of the orderbook file is the
    opening state). With ``snapshot=True`` the book is reconciled with the
    orderbook file at the 10-level window boundary after every message, which
    makes the reconstruction exact (see docs/LOBSTER_FORMAT.md).
    """
    msgs = D.load_messages(ticker, date, levels)
    start = 0
    seed_levels: list = []
    snap = None
    if seed or snapshot:
        ob = D.load_orderbook(ticker, date, levels)
        if seed:
            seed_levels = D.seed_from_orderbook_row(ob, 0, levels)
            start = 1
        if snapshot:
            snap = D.orderbook_arrays(ob, levels)
    raw = _lob.replay(*msgs.as_args(), seed=seed_levels, start=start, depth_k=depth_k,
                      price_priority_purge=price_priority_purge, snapshot=snap)
    df = pd.DataFrame({
        "ts_ns": msgs.ts_ns,
        "type": msgs.type,
        "best_bid": raw["best_bid"],
        "best_ask": raw["best_ask"],
        "bid_qty": raw["bid_qty"],
        "ask_qty": raw["ask_qty"],
        "ofi_e": raw["ofi_e"],
        "trade_qty": raw["trade_qty"],
        "trade_side": raw["trade_side"],
    })
    both = (df.best_bid > _lob.NO_BID) & (df.best_ask < _lob.NO_ASK)
    df["mid"] = np.where(both, (df.best_bid + df.best_ask) / 2.0 / _lob.PRICE_SCALE, np.nan)
    df["spread_ticks"] = np.where(both, (df.best_ask - df.best_bid) / _lob.TICK, np.nan)
    return df, raw
