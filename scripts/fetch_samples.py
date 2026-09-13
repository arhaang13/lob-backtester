#!/usr/bin/env python
"""Download the LOBSTER free sample files (2012-06-21, level 10) into data/raw.

lobsterdata.com now serves samples behind a request form, so this pulls the
identical files from a public Hugging Face mirror. If that fails, request the
samples at https://lobsterdata.com/info/DataSamples.php and unzip them into
data/raw/ manually.
"""
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "data" / "raw"
RAW.mkdir(parents=True, exist_ok=True)
TICKERS = ["AMZN", "AAPL", "GOOG", "INTC", "MSFT"]
BASE = "https://huggingface.co/datasets/totalorganfailure/lobster-data/resolve/main"

for t in TICKERS:
    folder = f"LOBSTER_SampleFile_{t}_2012-06-21_10"
    for kind in ("message", "orderbook"):
        name = f"{t}_2012-06-21_34200000_57600000_{kind}_10.csv"
        dest = RAW / name
        if dest.exists() and dest.stat().st_size > 1_000_000:
            print(f"exists  {name}")
            continue
        url = f"{BASE}/{folder}/{name}"
        try:
            urllib.request.urlretrieve(url, dest)
            print(f"fetched {name} ({dest.stat().st_size/1e6:.1f} MB)")
        except Exception as e:  # noqa: BLE001
            print(f"FAILED  {name}: {e}", file=sys.stderr)
