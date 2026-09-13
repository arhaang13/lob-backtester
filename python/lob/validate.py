"""Row-by-row validation of the reconstructed book against LOBSTER's orderbook file.

Three reconstruction modes are compared:
  message_only      pure message replay (seeded from row 0). Exact until a level
                    leaves the 10-level window, is changed off-file, and returns.
  priority_purge    + price-priority inference: phantoms are removed as soon as a
                    trade or resting order proves they cannot exist. Message-only.
  snapshot_resync   + window-boundary reconciliation with the orderbook file.
                    Exact by construction; its counters measure how much the
                    level-k truncation actually hides.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import data as D
from . import _lob

MODES = ("message_only", "priority_purge", "snapshot_resync")


@dataclass
class ModeReport:
    mode: str
    n_rows: int
    n_exact: int                      # rows where all top-k (price, size) match on both sides
    n_l1_exact: int                   # rows where level 1 matches on both sides
    first_mismatch: int | None
    mismatch_by_level: dict[int, int] = field(default_factory=dict)
    stats: dict = field(default_factory=dict)

    @property
    def exact_pct(self) -> float:
        return 100.0 * self.n_exact / max(1, self.n_rows)

    @property
    def l1_pct(self) -> float:
        return 100.0 * self.n_l1_exact / max(1, self.n_rows)


@dataclass
class ValidationReport:
    ticker: str
    n_rows: int
    modes: dict[str, ModeReport]

    def summary(self) -> str:
        lines = [f"{self.ticker}: {self.n_rows} rows"]
        for m in MODES:
            r = self.modes[m]
            fm = "none" if r.first_mismatch is None else str(r.first_mismatch)
            extra = ""
            if m == "priority_purge":
                extra = f" | priority purges {r.stats.get('priority_purges')}"
            if m == "snapshot_resync":
                extra = (f" | resync: levels added {r.stats.get('resync_levels_added')}, purged {r.stats.get('resync_levels_purged')},"
                         f" qty adjusted {r.stats.get('resync_qty_adjusted')}, orders trimmed {r.stats.get('resync_orders_trimmed')}")
            lines.append(f"  {m:16s} top-10 exact {r.exact_pct:7.3f}% | L1 exact {r.l1_pct:7.3f}% | first mismatch {fm:>7s}"
                         f" | unknown-id {r.stats.get('unknown_id')}{extra}")
        return "\n".join(lines)


def _compare(raw: dict, ref: dict, levels: int) -> tuple[np.ndarray, np.ndarray, dict[int, int]]:
    eq = np.ones(raw["ask_px"].shape[0], dtype=bool)
    l1 = None
    per_level: dict[int, int] = {}
    for k in range(levels):
        ok = ((raw["ask_px"][:, k] == ref["ask_px"][:, k]) & (raw["ask_sz"][:, k] == ref["ask_sz"][:, k]) &
              (raw["bid_px"][:, k] == ref["bid_px"][:, k]) & (raw["bid_sz"][:, k] == ref["bid_sz"][:, k]))
        per_level[k + 1] = int((~ok).sum())
        eq &= ok
        if k == 0:
            l1 = ok
    return eq, l1, per_level


def run_mode(mode: str, msgs: D.Messages, ref: dict, seed: list, levels: int) -> dict:
    kw = dict(seed=seed, start=1, depth_k=levels)
    if mode == "message_only":
        return _lob.replay(*msgs.as_args(), price_priority_purge=False, **kw)
    if mode == "priority_purge":
        return _lob.replay(*msgs.as_args(), price_priority_purge=True, **kw)
    if mode == "snapshot_resync":
        return _lob.replay(*msgs.as_args(), price_priority_purge=True, snapshot=ref, **kw)
    raise ValueError(mode)


def validate(ticker: str, date: str = D.DATE, levels: int = D.LEVELS, modes=MODES) -> ValidationReport:
    msgs = D.load_messages(ticker, date, levels)
    ob = D.load_orderbook(ticker, date, levels)
    ref = D.orderbook_arrays(ob, levels)
    seed = D.seed_from_orderbook_row(ob, 0, levels)
    out: dict[str, ModeReport] = {}
    for mode in modes:
        raw = run_mode(mode, msgs, ref, seed, levels)
        eq, l1, per_level = _compare(raw, ref, levels)
        bad = np.flatnonzero(~eq)
        out[mode] = ModeReport(mode, len(msgs), int(eq.sum()), int(l1.sum()),
                               int(bad[0]) if len(bad) else None, per_level, dict(raw["stats"]))
    return ValidationReport(ticker, len(msgs), out)


def explain_row(ticker: str, row: int, date: str = D.DATE, levels: int = D.LEVELS, k: int = 3,
                mode: str = "priority_purge"):
    """Print reconstructed vs reference top-k at `row` plus the message that produced it."""
    msgs = D.load_messages(ticker, date, levels)
    ob = D.load_orderbook(ticker, date, levels)
    ref = D.orderbook_arrays(ob, levels)
    seed = D.seed_from_orderbook_row(ob, 0, levels)
    raw = run_mode(mode, msgs, ref, seed, levels)
    m = msgs.to_frame().iloc[row]
    print(f"message {row}: {m.to_dict()}")
    for side in ("ask", "bid"):
        print(f"  {side} rebuilt : ", [(int(p), int(s)) for p, s in zip(raw[f'{side}_px'][row, :k], raw[f'{side}_sz'][row, :k])])
        print(f"  {side} lobster : ", [(int(p), int(s)) for p, s in zip(ref[f'{side}_px'][row, :k], ref[f'{side}_sz'][row, :k])])
