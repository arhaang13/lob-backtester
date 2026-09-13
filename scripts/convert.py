#!/usr/bin/env python
"""Convert every LOBSTER CSV pair in data/raw to typed Parquet in data/parquet."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from lob import data as D  # noqa: E402

force = "--force" in sys.argv
days = D.find_days()
if not days:
    sys.exit(f"no LOBSTER CSVs found in {D.RAW_DIR}; run scripts/fetch_samples.py first")
for d in days:
    t0 = time.perf_counter()
    n_msg, n_ob = D.convert_day(d, force=force)
    dt = time.perf_counter() - t0
    csv_mb = (d.message_csv.stat().st_size + d.orderbook_csv.stat().st_size) / 1e6
    pq_mb = (d.message_parquet.stat().st_size + d.orderbook_parquet.stat().st_size) / 1e6
    state = "skipped (exists)" if n_msg < 0 else f"{n_msg} messages, {n_ob} book rows"
    print(f"{d.ticker} {d.date}: {state} | csv {csv_mb:.1f} MB -> parquet {pq_mb:.1f} MB | {dt:.1f}s")
