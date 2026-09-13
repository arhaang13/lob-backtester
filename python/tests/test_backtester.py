import numpy as np
import pytest

import lob
from conftest import requires_amzn
from lob import data as D

S = lob.NS_PER_SEC


def cols():
    rows = [  # ts, type, id, size, price, dir
        (1 * S, 1, 1, 100, 1000, 1),
        (2 * S, 1, 2, 100, 1100, -1),
        (3 * S, 4, 1, 100, 1000, 1),
        (4 * S, 1, 3, 500, 1000, 1),
        (5 * S, 4, 3, 50, 1000, 1),
    ]
    a = np.array(rows, dtype=np.int64)
    return (a[:, 0], a[:, 1].astype(np.int8), a[:, 2], a[:, 3].astype(np.int32), a[:, 4], a[:, 5].astype(np.int8))


class JoinBid(lob.Strategy):
    """Python strategy: join the best bid once, record callbacks."""

    def __init__(self):
        super().__init__()
        self.events = []
        self.fills = []
        self.sent = False

    def on_market(self, ctx, msg, ofi_e):
        self.events.append((msg.ts, int(msg.type), ofi_e))
        if not self.sent and ctx.book().best_ask_price() != lob.NO_ASK:
            ctx.submit_limit(lob.Side.Buy, ctx.book().best_bid_price(), 50)
            self.sent = True

    def on_fill(self, ctx, fill):
        self.fills.append((fill.ts, fill.price, fill.qty, fill.passive))


def short_cfg():
    c = lob.BacktestConfig()
    c.start_ts = 0
    c.end_ts = 100 * S
    c.flatten_before_close_ns = 5 * S
    return c


def test_python_strategy_trampoline():
    strat = JoinBid()
    bt = lob.Backtester(*cols(), seed=[], config=short_cfg())
    res = bt.run(strat)
    assert len(strat.events) == 5
    assert strat.fills[0] == (5 * S, 1000, 50, True)      # filled once queue ahead (100) is gone
    f = res.fills()
    assert f["qty"].tolist() == [50, 50] and f["passive"].tolist() == [1, 0]
    assert res.portfolio.position == 0
    assert res.metrics.total_pnl == pytest.approx(50 * 0.0020 - 50 * 0.0030)


def cols_ask_vanishes():
    rows = [  # ask 1100 exists from t=2s and is deleted at t=3s
        (1 * S, 1, 1, 100, 1000, 1),
        (2 * S, 1, 2, 100, 1100, -1),
        (3 * S, 3, 2, 100, 1100, -1),
        (4 * S, 1, 4, 100, 1200, -1),
    ]
    a = np.array(rows, dtype=np.int64)
    return (a[:, 0], a[:, 1].astype(np.int8), a[:, 2], a[:, 3].astype(np.int32), a[:, 4], a[:, 5].astype(np.int8))


def test_latency_kills_the_fill():
    class Ioc(lob.Strategy):
        def __init__(self):
            super().__init__(); self.sent = False
        def on_market(self, ctx, msg, e):
            if not self.sent and ctx.book().best_ask_price() != lob.NO_ASK:
                ctx.submit_limit(lob.Side.Buy, 1100, 10, lob.TimeInForce.IOC); self.sent = True
    for lat, n in ((0, 2), (2 * S, 0)):        # 2 s latency: arrives after the ask is gone
        cfg = short_cfg(); cfg.order_latency_ns = lat
        res = lob.Backtester(*cols_ask_vanishes(), seed=[], config=cfg).run(Ioc())
        assert res.metrics.n_fills == n


@requires_amzn
def test_ofi_strategy_on_amzn_with_snapshot():
    msgs = D.load_messages("AMZN")
    ob = D.load_orderbook("AMZN")
    snap = D.orderbook_arrays(ob)
    seed = D.seed_from_orderbook_row(ob, 0)
    p = lob.OfiStrategyParams(); p.passive_entry = False; p.entry_z = 2.5
    cfg = lob.BacktestConfig(); cfg.start_index = 1; cfg.order_latency_ns = 500_000
    res = lob.Backtester(*msgs.as_args(), seed=seed, config=cfg, snapshot=snap).run(lob.OfiStrategy(p))
    assert res.orders_submitted > 0
    assert res.portfolio.position == 0
    s = res.samples()
    assert len(s["ts"]) == 23400 + 1                     # 09:30..16:00 inclusive at 1 s
    assert res.stats()["resync_rows"] == len(msgs) - 1
