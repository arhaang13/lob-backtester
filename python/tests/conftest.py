import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from lob import data as D  # noqa: E402


def has_day(ticker: str) -> bool:
    try:
        d = D.day(ticker)
        return d.message_parquet.exists() and d.orderbook_parquet.exists()
    except FileNotFoundError:
        return False


requires_amzn = pytest.mark.skipif(not has_day("AMZN"), reason="AMZN parquet not present; run scripts/convert.py")
