"""LOBSTER CSV -> Parquet pipeline and typed loaders.

LOBSTER file pair for one ticker-day (level k):
  {TICKER}_{DATE}_{start_ms}_{end_ms}_message_{k}.csv
      time, type, order_id, size, price, direction          (no header)
  {TICKER}_{DATE}_{start_ms}_{end_ms}_orderbook_{k}.csv
      ask_px_1, ask_sz_1, bid_px_1, bid_sz_1, ..., level k   (no header)
  Row i of the orderbook file is the book state AFTER message i.

Why Parquet: it is columnar (we only ever need a few columns at a time), typed
(int64 nanoseconds instead of a float string), compressed (5-10x smaller than
CSV), and pyarrow hands the columns to numpy without copying, which is what the
zero-copy path into C++ relies on.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

ROOT = Path(__file__).resolve().parents[2]
RAW_DIR = ROOT / "data" / "raw"
PARQUET_DIR = ROOT / "data" / "parquet"

TICKERS = ["AMZN", "AAPL", "GOOG", "INTC", "MSFT"]
DATE = "2012-06-21"
LEVELS = 10

NS_PER_SEC = 1_000_000_000

MESSAGE_SCHEMA = pa.schema([
    ("ts_ns", pa.int64()),      # nanoseconds since midnight
    ("type", pa.int8()),        # 1..7
    ("order_id", pa.int64()),
    ("size", pa.int32()),
    ("price", pa.int64()),      # dollars * 10_000
    ("direction", pa.int8()),   # +1 buy / -1 sell
])

_FNAME = re.compile(r"(?P<ticker>[A-Z]+)_(?P<date>\d{4}-\d{2}-\d{2})_(?P<start>\d+)_(?P<end>\d+)_(?P<kind>message|orderbook)_(?P<levels>\d+)\.csv")


@dataclass(frozen=True)
class DayFiles:
    ticker: str
    date: str
    levels: int
    message_csv: Path
    orderbook_csv: Path

    @property
    def message_parquet(self) -> Path:
        return PARQUET_DIR / f"{self.ticker}_{self.date}_message_{self.levels}.parquet"

    @property
    def orderbook_parquet(self) -> Path:
        return PARQUET_DIR / f"{self.ticker}_{self.date}_orderbook_{self.levels}.parquet"


def find_days(raw_dir: Path = RAW_DIR) -> list[DayFiles]:
    """Pair up message/orderbook CSVs found in raw_dir."""
    found: dict[tuple, dict] = {}
    for p in sorted(raw_dir.glob("*.csv")):
        m = _FNAME.match(p.name)
        if not m:
            continue
        key = (m["ticker"], m["date"], int(m["levels"]))
        found.setdefault(key, {})[m["kind"]] = p
    out = []
    for (t, d, k), kinds in found.items():
        if "message" in kinds and "orderbook" in kinds:
            out.append(DayFiles(t, d, k, kinds["message"], kinds["orderbook"]))
    return out


def day(ticker: str, date: str = DATE, levels: int = LEVELS) -> DayFiles:
    for d in find_days():
        if d.ticker == ticker and d.date == date and d.levels == levels:
            return d
    raise FileNotFoundError(f"no LOBSTER files for {ticker} {date} level {levels} in {RAW_DIR}")


# --------------------------------------------------------------------------- parsing

def parse_time_to_ns(time_strings: pa.Array | pa.ChunkedArray) -> pa.Array:
    """'34200.017459617' -> 34200017459617 exactly.

    Parsing the decimal string as float64 would lose precision: 34200.017459617
    has 14 significant digits and float64 only guarantees ~15.9, so the last
    nanosecond digit is at risk (and floating rounding differs across parsers).
    Splitting on '.' and doing integer arithmetic is exact.
    """
    s = pd.Series(time_strings.to_pandas() if hasattr(time_strings, "to_pandas") else time_strings, dtype="string")
    parts = s.str.split(".", n=1, expand=True)
    secs = parts[0].astype("int64")
    frac = parts[1].fillna("").str.ljust(9, "0").str.slice(0, 9)
    frac = frac.where(frac != "", "0").astype("int64")
    ns = secs.to_numpy() * NS_PER_SEC + frac.to_numpy()
    return pa.array(ns, type=pa.int64())


def convert_messages(csv_path: Path, out_path: Path, ticker: str, date: str, levels: int) -> int:
    """Read a LOBSTER message CSV and write a typed Parquet file. Returns row count."""
    read_opts = pacsv.ReadOptions(column_names=["time", "type", "order_id", "size", "price", "direction"])
    conv_opts = pacsv.ConvertOptions(column_types={
        "time": pa.string(),        # keep exact; converted below
        "type": pa.int8(),
        "order_id": pa.int64(),
        "size": pa.int32(),
        "price": pa.int64(),
        "direction": pa.int8(),
    })
    tbl = pacsv.read_csv(csv_path, read_options=read_opts, convert_options=conv_opts)
    ts = parse_time_to_ns(tbl.column("time"))
    out = pa.table({
        "ts_ns": ts,
        "type": tbl.column("type"),
        "order_id": tbl.column("order_id"),
        "size": tbl.column("size"),
        "price": tbl.column("price"),
        "direction": tbl.column("direction"),
    }, schema=MESSAGE_SCHEMA)
    meta = {b"ticker": ticker.encode(), b"date": date.encode(), b"levels": str(levels).encode(),
            b"source": b"LOBSTER sample", b"time_unit": b"ns since midnight", b"price_unit": b"USD*1e4"}
    out = out.replace_schema_metadata(meta)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(out, out_path, compression="snappy", row_group_size=1 << 16)
    return out.num_rows


def orderbook_columns(levels: int) -> list[str]:
    cols = []
    for k in range(1, levels + 1):
        cols += [f"ask_px_{k}", f"ask_sz_{k}", f"bid_px_{k}", f"bid_sz_{k}"]
    return cols


def convert_orderbook(csv_path: Path, out_path: Path, ticker: str, date: str, levels: int) -> int:
    cols = orderbook_columns(levels)
    types = {c: (pa.int64() if "_px_" in c else pa.int32()) for c in cols}
    tbl = pacsv.read_csv(csv_path, read_options=pacsv.ReadOptions(column_names=cols),
                         convert_options=pacsv.ConvertOptions(column_types=types))
    meta = {b"ticker": ticker.encode(), b"date": date.encode(), b"levels": str(levels).encode(),
            b"empty_ask_sentinel": b"9999999999", b"empty_bid_sentinel": b"-9999999999"}
    tbl = tbl.replace_schema_metadata(meta)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(tbl, out_path, compression="snappy", row_group_size=1 << 16)
    return tbl.num_rows


def convert_day(d: DayFiles, force: bool = False) -> tuple[int, int]:
    n_msg = n_ob = -1
    if force or not d.message_parquet.exists():
        n_msg = convert_messages(d.message_csv, d.message_parquet, d.ticker, d.date, d.levels)
    if force or not d.orderbook_parquet.exists():
        n_ob = convert_orderbook(d.orderbook_csv, d.orderbook_parquet, d.ticker, d.date, d.levels)
    return n_msg, n_ob


# --------------------------------------------------------------------------- loading

@dataclass
class Messages:
    """Contiguous numpy columns ready to hand to the C++ engine (zero-copy)."""
    ts_ns: np.ndarray
    type: np.ndarray
    order_id: np.ndarray
    size: np.ndarray
    price: np.ndarray
    direction: np.ndarray

    def __len__(self) -> int:
        return len(self.ts_ns)

    def as_args(self) -> tuple:
        return (self.ts_ns, self.type, self.order_id, self.size, self.price, self.direction)

    def to_frame(self) -> pd.DataFrame:
        return pd.DataFrame({"ts_ns": self.ts_ns, "type": self.type, "order_id": self.order_id,
                             "size": self.size, "price": self.price, "direction": self.direction})


def load_messages(ticker: str, date: str = DATE, levels: int = LEVELS) -> Messages:
    path = day(ticker, date, levels).message_parquet
    tbl = pq.read_table(path)
    col = lambda name: np.ascontiguousarray(tbl.column(name).to_numpy(zero_copy_only=False))
    return Messages(col("ts_ns"), col("type"), col("order_id"), col("size"), col("price"), col("direction"))


def load_orderbook(ticker: str, date: str = DATE, levels: int = LEVELS) -> pd.DataFrame:
    return pq.read_table(day(ticker, date, levels).orderbook_parquet).to_pandas()


def orderbook_arrays(ob: pd.DataFrame, levels: int = LEVELS) -> dict[str, np.ndarray]:
    """Orderbook DataFrame -> four (n, levels) int arrays."""
    return {
        "ask_px": np.column_stack([ob[f"ask_px_{k}"].to_numpy() for k in range(1, levels + 1)]).astype(np.int64),
        "ask_sz": np.column_stack([ob[f"ask_sz_{k}"].to_numpy() for k in range(1, levels + 1)]).astype(np.int32),
        "bid_px": np.column_stack([ob[f"bid_px_{k}"].to_numpy() for k in range(1, levels + 1)]).astype(np.int64),
        "bid_sz": np.column_stack([ob[f"bid_sz_{k}"].to_numpy() for k in range(1, levels + 1)]).astype(np.int32),
    }


def seed_from_orderbook_row(ob: pd.DataFrame, row: int = 0, levels: int = LEVELS) -> list[tuple[int, int, int]]:
    """Top-k visible liquidity at `row` as (side, price, qty) tuples for the engine.

    Used to pre-load the book with the liquidity that was already resting when
    the message file starts (its orders were placed before 09:30 and never
    appear as type-1 messages).
    """
    r = ob.iloc[row]
    seed = []
    for k in range(1, levels + 1):
        ap, asz, bp, bsz = int(r[f"ask_px_{k}"]), int(r[f"ask_sz_{k}"]), int(r[f"bid_px_{k}"]), int(r[f"bid_sz_{k}"])
        if asz > 0 and ap < 9_999_999_999:
            seed.append((-1, ap, asz))
        if bsz > 0 and bp > -9_999_999_999:
            seed.append((+1, bp, bsz))
    return seed
