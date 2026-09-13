"""Signal research: does OFI explain / predict mid-price changes?"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd

from . import data as D
from .reconstruct import replay_day
from .signals import bucket_ofi, forward_mid_change, queue_imbalance, rolling_ofi


@dataclass
class RegResult:
    ticker: str
    bucket_s: float
    n: int
    slope: float          # ticks per unit OFI (x1e4 for readability in tables)
    t_stat: float
    r2: float
    mean_depth: float
    slope_x_depth: float  # CKS: slope ~ c / depth, so slope*depth should be roughly constant


def ols_hac(x: np.ndarray, y: np.ndarray, lags: int = 5) -> tuple[float, float, float, float]:
    """Slope, intercept, HAC t-stat for slope, R^2 (statsmodels if available)."""
    try:
        import statsmodels.api as sm
        X = sm.add_constant(x)
        fit = sm.OLS(y, X).fit(cov_type="HAC", cov_kwds={"maxlags": lags})
        return float(fit.params[1]), float(fit.params[0]), float(fit.tvalues[1]), float(fit.rsquared)
    except ImportError:  # pragma: no cover
        X = np.column_stack([np.ones_like(x), x])
        beta, *_ = np.linalg.lstsq(X, y, rcond=None)
        resid = y - X @ beta
        r2 = 1 - resid.var() / y.var()
        se = np.sqrt(resid.var() / ((x - x.mean()) ** 2).sum())
        return float(beta[1]), float(beta[0]), float(beta[1] / se), float(r2)


def ofi_regression(ticker: str, bucket_s: float = 10.0, df: pd.DataFrame | None = None) -> RegResult:
    """Contemporaneous regression dmid_k = a + b * OFI_k (Cont-Kukanov-Stoikov 2014, eq. 2)."""
    if df is None:
        df, _ = replay_day(ticker, seed=True, snapshot=True)
    b = bucket_ofi(df, bucket_s)
    slope, _, t, r2 = ols_hac(b.ofi.to_numpy(float), b.dmid_ticks.to_numpy(float))
    depth = float(b.depth.mean())
    return RegResult(ticker, bucket_s, len(b), slope, t, r2, depth, slope * depth)


def bucket_sweep(ticker: str, buckets=(1, 5, 10, 30, 60)) -> pd.DataFrame:
    df, _ = replay_day(ticker, seed=True, snapshot=True)
    rows = [ofi_regression(ticker, s, df) for s in buckets]
    return pd.DataFrame([r.__dict__ for r in rows])


def predictive_table(ticker: str, windows=(20, 50, 100), horizons=(10, 50, 200)) -> pd.DataFrame:
    """Event-time predictive check: corr(rolling OFI over W events, mid change over next H events)."""
    df, _ = replay_day(ticker, seed=True, snapshot=True)
    qi = queue_imbalance(df)
    rows = []
    for w in windows:
        s = rolling_ofi(df, w)
        for h in horizons:
            fwd = forward_mid_change(df, h)
            ok = s.notna() & fwd.notna()
            rows.append({"ticker": ticker, "signal": f"ofi_w{w}", "horizon_events": h,
                         "corr": float(np.corrcoef(s[ok], fwd[ok])[0, 1]), "n": int(ok.sum())})
    for h in horizons:
        fwd = forward_mid_change(df, h)
        ok = qi.notna() & fwd.notna()
        rows.append({"ticker": ticker, "signal": "queue_imbalance", "horizon_events": h,
                     "corr": float(np.corrcoef(qi[ok], fwd[ok])[0, 1]), "n": int(ok.sum())})
    return pd.DataFrame(rows)


def plot_ofi(ticker: str, bucket_s: float = 10.0, out_path=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    df, _ = replay_day(ticker, seed=True, snapshot=True)
    b = bucket_ofi(df, bucket_s)
    r = ofi_regression(ticker, bucket_s, df)
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    ax[0].scatter(b.ofi, b.dmid_ticks, s=6, alpha=0.4)
    xs = np.linspace(b.ofi.min(), b.ofi.max(), 50)
    ax[0].plot(xs, r.slope * xs, color="C1", label=f"slope={r.slope:.2e}, t={r.t_stat:.1f}, R²={r.r2:.2f}")
    ax[0].set_xlabel(f"OFI per {bucket_s:.0f}s bucket (shares)"); ax[0].set_ylabel("mid change (ticks)")
    ax[0].set_title(f"{ticker}: Δmid vs OFI"); ax[0].legend(fontsize=8)
    t = pd.to_datetime(df.ts_ns, unit="ns")
    ax[1].plot(t, df.mid, lw=0.6); ax[1].set_title(f"{ticker} mid, {D.DATE}"); ax[1].set_ylabel("USD")
    fig.tight_layout()
    if out_path:
        fig.savefig(out_path, dpi=120)
    return fig
