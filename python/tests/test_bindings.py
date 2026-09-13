import numpy as np

import lob


def test_order_book_basic():
    b = lob.OrderBook()
    assert b.add(1, lob.Side.Buy, 1000, 100)
    assert not b.add(1, lob.Side.Buy, 1000, 100)      # duplicate id
    assert b.add(2, lob.Side.Sell, 1100, 50)
    assert b.best_bid_price() == 1000 and b.best_ask_price() == 1100
    assert b.spread() == 100 and b.mid() == 1050.0
    assert b.cancel(1, 30) and b.order(1)["qty"] == 70
    assert b.execute(2, 50) and b.best_ask_price() == lob.NO_ASK
    assert b.check_invariants()
    assert b.stats()["duplicate_ids"] == 1


def test_apply_message_unknown_id_goes_to_anon():
    b = lob.OrderBook()
    b.add_anon(lob.Side.Sell, 1100, 500)
    lob.apply_message(b, 1, 4, 999, 100, 1100, -1)
    assert b.best_ask_qty() == 400


def test_replay_small():
    ts = np.array([1, 2, 3, 4], dtype=np.int64)
    typ = np.array([1, 1, 1, 4], dtype=np.int8)
    oid = np.array([1, 2, 3, 2], dtype=np.int64)
    size = np.array([100, 100, 50, 30], dtype=np.int32)
    price = np.array([1000, 1100, 1000, 1100], dtype=np.int64)
    d = np.array([1, -1, 1, -1], dtype=np.int8)
    r = lob.replay(ts, typ, oid, size, price, d, depth_k=2)
    assert r["best_bid"].tolist() == [1000, 1000, 1000, 1000]
    assert r["ask_qty"].tolist() == [0, 100, 100, 70]
    assert r["ofi_e"][2] == 50 and r["ofi_e"][3] == 30
    assert r["ask_px"].shape == (4, 2) and r["bid_px"][3, 1] == lob.NO_BID
    assert r["stats"]["adds"] == 3


def test_replay_accepts_non_native_dtypes():
    # forcecast: int lists / float arrays get converted (one copy) instead of raising
    r = lob.replay([1, 2], np.array([1, 1]), [1, 2], [10, 10], [1000, 1100], [1, -1])
    assert r["best_ask"][1] == 1100
