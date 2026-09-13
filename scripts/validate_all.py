#!/usr/bin/env python
"""Validate the reconstructed book against LOBSTER's orderbook file for every ticker."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from lob import data as D  # noqa: E402
from lob.validate import validate  # noqa: E402

tickers = [a for a in sys.argv[1:] if not a.startswith("-")] or [d.ticker for d in D.find_days()]
for t in tickers:
    t0 = time.perf_counter()
    rep = validate(t)
    print(rep.summary(), f"| {time.perf_counter()-t0:.2f}s")
