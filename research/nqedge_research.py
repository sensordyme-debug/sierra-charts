#!/usr/bin/env python
"""NQ Edge research loop.

Loads the feature logs written by the "NQ Edge: Feature Logger" study, labels direction from
forward returns, trains walk-forward models (regularized logistic regression first, gradient
boosting second), reports hit rate / expectancy in R / profit factor / calibration per setup and
per regime plus feature importances, and exports NQEdge_weights.txt in the format the C++ DCS
study hot-reloads.

One command:
    python research/nqedge_research.py --data "C:/SierraChart/Data" --deploy

Walk-forward only: rolling train window -> next test window, never shuffled, never look-ahead.
"""
from __future__ import annotations

import argparse
import glob
import os
import sys
from dataclasses import dataclass

import numpy as np
import pandas as pd

try:
    from sklearn.linear_model import LogisticRegression
    from sklearn.preprocessing import StandardScaler
except ImportError:  # pragma: no cover
    print("scikit-learn is required: pip install -r research/requirements.txt", file=sys.stderr)
    raise

try:
    import lightgbm as lgb  # type: ignore
    HAVE_LGB = True
except Exception:  # pragma: no cover
    HAVE_LGB = False
    from sklearn.ensemble import HistGradientBoostingClassifier

# ---------------------------------------------------------------------------------------------
# Feature catalogue (must match kFeatureNames / kFeatureGroup in NQEdgeSuite.cpp)
FEATURES = [
    "vwapPos", "vwapSlope", "vwapAccept", "structTrend", "bos", "vaPos", "pocPos", "ibPos",
    "valueMig", "openType", "delta", "cvdZ", "cvdDiv", "absorb", "exhaust", "imbalance", "trapped",
    "largeTrade", "regimeTrend", "mtfBias", "rsIndex", "smt", "tickCum", "tickExt", "tickDiv", "megaCap",
    "legEff", "legVol", "pullback", "absorbQ", "auction", "leadLag",
]
GROUPS = {
    "vwapPos": "location", "vwapSlope": "trend", "vwapAccept": "trend", "structTrend": "trend", "bos": "trend",
    "vaPos": "location", "pocPos": "location", "ibPos": "location", "valueMig": "context", "openType": "context",
    "delta": "flow", "cvdZ": "flow", "cvdDiv": "reversal", "absorb": "reversal", "exhaust": "reversal",
    "imbalance": "flow", "trapped": "reversal", "largeTrade": "flow", "regimeTrend": "trend", "mtfBias": "trend",
    "rsIndex": "intermarket", "smt": "reversal", "tickCum": "intermarket", "tickExt": "intermarket",
    "tickDiv": "reversal", "megaCap": "intermarket",
    "legEff": "flow", "legVol": "flow", "pullback": "location", "absorbQ": "reversal", "auction": "trend", "leadLag": "intermarket",
}
GROUP_NAMES = ["trend", "flow", "reversal", "location", "intermarket", "context"]
REGIME_NAMES = {0: "none", 1: "trend_up", 2: "trend_down", 3: "balance", 4: "chop"}
REGIME_KEY = {1: "trend", 2: "trend", 3: "balance", 4: "chop"}
SETUP_NAMES = {0: "none", 1: "trend_pullback", 2: "value_edge", 3: "failed_breakout", 4: "break_accept", 5: "divergence"}
DEFAULT_GATES = {
    "trend": {"trend": 1.2, "flow": 1.0, "reversal": 0.5, "location": 0.5, "intermarket": 1.0, "context": 1.0},
    "balance": {"trend": 0.6, "flow": 1.0, "reversal": 1.3, "location": -0.8, "intermarket": 1.0, "context": 1.0},
    "chop": {g: 0.5 for g in GROUP_NAMES},
}


# ---------------------------------------------------------------------------------------------
def load_logs(data_dir: str, pattern: str) -> pd.DataFrame:
    files = sorted(glob.glob(os.path.join(data_dir, pattern)))
    if not files:
        raise SystemExit(f"No feature logs matching {pattern!r} in {data_dir!r}. Run the Feature Logger study first.")
    frames = []
    for f in files:
        df = pd.read_csv(f)
        df["symbol"] = os.path.basename(f).replace("NQEdge_features_", "").replace(".csv", "")
        frames.append(df)
    df = pd.concat(frames, ignore_index=True)
    df["time"] = pd.to_datetime(df["time"])
    df = df.sort_values(["symbol", "time"]).drop_duplicates(["symbol", "time"], keep="last").reset_index(drop=True)
    for c in FEATURES:
        if c not in df.columns:
            df[c] = np.nan
    return df


def make_synthetic(n: int, seed: int = 7) -> pd.DataFrame:
    """Synthetic log for smoke-testing the pipeline (no market data needed)."""
    rng = np.random.default_rng(seed)
    t = pd.date_range("2026-01-05 09:30", periods=n, freq="1min")
    X = np.clip(rng.normal(0, 0.5, size=(n, len(FEATURES))), -1, 1)
    true_w = rng.normal(0, 1, size=len(FEATURES))
    regime = rng.choice([1, 2, 3, 4], size=n, p=[0.3, 0.3, 0.3, 0.1])
    signal = X @ true_w / np.sqrt(len(FEATURES))
    fwd15 = signal * 0.6 + rng.normal(0, 1.0, size=n)
    df = pd.DataFrame(X, columns=FEATURES)
    df.insert(0, "time", t)
    df["idx"] = np.arange(n)
    df["trading_day"] = df["time"].dt.strftime("%Y%m%d").astype(int)
    df["open"] = df["high"] = df["low"] = df["close"] = 7000 + np.cumsum(rng.normal(0, 1, n))
    df["volume"] = rng.integers(100, 1000, n)
    df["atr"] = 2.0
    df["dcs"] = np.clip(100 * signal, -100, 100)
    df["regime"] = regime
    df["setup"] = np.where(rng.random(n) < 0.03, rng.integers(1, 6, n), 0)
    df["dir"] = np.where(df["setup"] > 0, np.sign(signal + 1e-9), 0).astype(int)
    df["fwd5"] = fwd15 * 0.5 + rng.normal(0, 0.5, n)
    df["fwd15"] = fwd15
    df["fwd30"] = fwd15 * 1.3 + rng.normal(0, 0.8, n)
    df["fwd60"] = fwd15 * 1.6 + rng.normal(0, 1.2, n)
    df["mfe"] = np.abs(fwd15) + 0.3
    df["mae"] = np.abs(rng.normal(0, 0.5, n))
    df["symbol"] = "SYNTH"
    return df


# ---------------------------------------------------------------------------------------------
@dataclass
class FoldResult:
    test_idx: np.ndarray
    p_lr: np.ndarray
    p_gb: np.ndarray
    coef_lr: np.ndarray       # raw-scale coefficients (per unit of feature)
    imp_gb: np.ndarray


def walk_forward(df: pd.DataFrame, X: np.ndarray, y: np.ndarray, usable: np.ndarray, train_days: int, test_days: int,
                 min_train: int, C: float, gb_rounds: int) -> list[FoldResult]:
    days = np.sort(df["trading_day"].unique())
    results: list[FoldResult] = []
    start = 0
    while True:
        tr_days = days[start:start + train_days]
        te_days = days[start + train_days:start + train_days + test_days]
        if len(te_days) == 0 or len(tr_days) < max(2, train_days // 2):
            break
        tr = df["trading_day"].isin(tr_days).to_numpy() & usable
        te = df["trading_day"].isin(te_days).to_numpy()
        if tr.sum() >= min_train and te.sum() > 0:
            scaler = StandardScaler().fit(X[tr])
            Xtr, Xte = scaler.transform(X[tr]), scaler.transform(X[te])
            lr = LogisticRegression(C=C, class_weight="balanced", max_iter=500)   # L2 (default)
            lr.fit(Xtr, y[tr])
            p_lr = lr.predict_proba(Xte)[:, 1]
            scale = np.where(scaler.scale_ > 0, scaler.scale_, 1.0)
            coef_raw = lr.coef_[0] / scale
            if HAVE_LGB:
                gb = lgb.LGBMClassifier(n_estimators=gb_rounds, learning_rate=0.05, num_leaves=15, max_depth=4,
                                        min_child_samples=50, subsample=0.8, subsample_freq=1, colsample_bytree=0.8,
                                        reg_lambda=5.0, verbose=-1)
                gb.fit(X[tr], y[tr])
                p_gb = gb.predict_proba(X[te])[:, 1]
                imp = gb.booster_.feature_importance(importance_type="gain").astype(float)
            else:
                gb = HistGradientBoostingClassifier(max_iter=gb_rounds, learning_rate=0.05, max_depth=4, l2_regularization=5.0)
                gb.fit(X[tr], y[tr])
                p_gb = gb.predict_proba(X[te])[:, 1]
                imp = np.zeros(X.shape[1])
            results.append(FoldResult(np.where(te)[0], p_lr, p_gb, coef_raw, imp))
        start += test_days
    if not results:
        raise SystemExit("Not enough data for a single walk-forward fold. Lower --train-days / --test-days or log more days.")
    return results


# ---------------------------------------------------------------------------------------------
def evaluate(df: pd.DataFrame, fwd: np.ndarray, usable: np.ndarray, p: np.ndarray, mask: np.ndarray, risk_atr: float,
             margin: float) -> dict:
    """Hit rate / expectancy in R / profit factor for rows where the model takes a side."""
    side = np.where(p >= 0.5 + margin, 1, np.where(p <= 0.5 - margin, -1, 0))
    take = mask & (side != 0) & usable
    n = int(take.sum())
    if n == 0:
        return {"n": 0, "hit": np.nan, "expR": np.nan, "pf": np.nan, "coverage": 0.0}
    r = side[take] * fwd[take] / risk_atr
    hits = (r > 0).mean()
    wins, losses = r[r > 0].sum(), -r[r < 0].sum()
    return {"n": n, "hit": float(hits), "expR": float(r.mean()), "pf": float(wins / losses) if losses > 0 else np.inf,
            "coverage": float(take.sum() / max(1, mask.sum()))}


def calibration(p: np.ndarray, y: np.ndarray, mask: np.ndarray, bins: int = 10) -> pd.DataFrame:
    edges = np.linspace(0, 1, bins + 1)
    rows = []
    for a, b in zip(edges[:-1], edges[1:]):
        m = mask & (p >= a) & (p < b if b < 1 else p <= b)
        if m.sum() == 0:
            continue
        rows.append({"p_lo": a, "p_hi": b, "n": int(m.sum()), "pred_mean": float(p[m].mean()), "actual_up": float(y[m].mean())})
    return pd.DataFrame(rows)


def dcs_from_weights(X: np.ndarray, w: np.ndarray, gates: np.ndarray) -> np.ndarray:
    """Replicates the C++ composite on logged features (NaN -> excluded, re-weighted)."""
    Xn = np.nan_to_num(X, nan=0.0)
    avail = ~np.isnan(X)
    ww = w[None, :] * gates
    num = (ww * Xn).sum(axis=1)
    den = (np.abs(ww) * avail).sum(axis=1)
    return np.where(den > 0, 100 * num / np.maximum(den, 1e-9), 0.0)


# ---------------------------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=r"C:\SierraChart\Data", help="Sierra Chart Data folder (feature logs live here)")
    ap.add_argument("--pattern", default="NQEdge_features_*.csv")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "output"))
    ap.add_argument("--horizon", choices=["5", "15", "30", "60"], default="15", help="forward-return horizon (minutes) used as the label")
    ap.add_argument("--label-atr", type=float, default=0.5, help="|forward return| in ATR beyond which a bar is labelled up/down")
    ap.add_argument("--train-days", type=int, default=20)
    ap.add_argument("--test-days", type=int, default=5)
    ap.add_argument("--min-train", type=int, default=1500)
    ap.add_argument("--C", type=float, default=0.1, help="inverse L2 strength for logistic regression")
    ap.add_argument("--gb-rounds", type=int, default=300)
    ap.add_argument("--margin", type=float, default=0.08, help="probability margin around 0.5 to take a side when scoring")
    ap.add_argument("--rth-only", action="store_true", help="train/evaluate on RTH bars only (09:30-16:00 chart time)")
    ap.add_argument("--deploy", action="store_true", help="also copy NQEdge_weights.txt into --data (Sierra hot-reloads it)")
    ap.add_argument("--synthetic", type=int, default=0, help="smoke test with N synthetic rows instead of real logs")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    df = make_synthetic(args.synthetic) if args.synthetic > 0 else load_logs(args.data, args.pattern)
    if args.rth_only:
        tod = df["time"].dt.hour * 60 + df["time"].dt.minute
        df = df[(tod >= 570) & (tod < 960)].reset_index(drop=True)

    fwd_col = f"fwd{args.horizon}"
    fwd = df[fwd_col].to_numpy(dtype=float)
    X = df[FEATURES].to_numpy(dtype=float)
    y = (fwd > 0).astype(int)
    usable = np.abs(fwd) >= args.label_atr            # directional label beyond the ATR threshold
    print(f"rows={len(df)}  days={df['trading_day'].nunique()}  usable(|{fwd_col}|>={args.label_atr} ATR)={int(usable.sum())}  up-share={y[usable].mean():.3f}")

    Xf = np.nan_to_num(X, nan=0.0)
    folds = walk_forward(df, Xf, y, usable, args.train_days, args.test_days, args.min_train, args.C, args.gb_rounds)
    test_mask = np.zeros(len(df), dtype=bool)
    p_lr = np.full(len(df), np.nan); p_gb = np.full(len(df), np.nan)
    for fr in folds:
        test_mask[fr.test_idx] = True
        p_lr[fr.test_idx] = fr.p_lr; p_gb[fr.test_idx] = fr.p_gb
    print(f"walk-forward folds={len(folds)}  test rows={int(test_mask.sum())}")

    # ---- metrics per model / setup / regime ----
    rows = []
    for model, p in (("logistic", p_lr), ("boosting", p_gb)):
        pm = np.nan_to_num(p, nan=0.5)
        rows.append({"model": model, "slice": "all", **evaluate(df, fwd, usable, pm, test_mask, args.label_atr, args.margin)})
        for s, name in SETUP_NAMES.items():
            m = test_mask & (df["setup"].to_numpy() == s)
            if m.sum() > 0:
                rows.append({"model": model, "slice": f"setup:{name}", **evaluate(df, fwd, usable, pm, m, args.label_atr, args.margin)})
        for r, name in REGIME_NAMES.items():
            m = test_mask & (df["regime"].to_numpy() == r)
            if m.sum() > 0:
                rows.append({"model": model, "slice": f"regime:{name}", **evaluate(df, fwd, usable, pm, m, args.label_atr, args.margin)})
    # the suite's own signals (setup != 0, dir != 0): hit rate of the drawn signals in the test windows
    sig = test_mask & (df["setup"].to_numpy() > 0) & (df["dir"].to_numpy() != 0)
    if sig.sum() > 0:
        r = df["dir"].to_numpy()[sig] * fwd[sig] / args.label_atr
        rows.append({"model": "suite_signals", "slice": "all", "n": int(sig.sum()), "hit": float((r > 0).mean()), "expR": float(r.mean()),
                     "pf": float(r[r > 0].sum() / max(1e-9, -r[r < 0].sum())), "coverage": 1.0})
    metrics = pd.DataFrame(rows)
    metrics.to_csv(os.path.join(args.out, "metrics.csv"), index=False)

    cal = calibration(np.nan_to_num(p_lr, nan=0.5), y, test_mask & usable)
    cal.to_csv(os.path.join(args.out, "calibration_logistic.csv"), index=False)
    cal_gb = calibration(np.nan_to_num(p_gb, nan=0.5), y, test_mask & usable)
    cal_gb.to_csv(os.path.join(args.out, "calibration_boosting.csv"), index=False)

    # ---- importances / learned weights ----
    coef = np.mean([fr.coef_lr for fr in folds], axis=0)
    imp = np.mean([fr.imp_gb for fr in folds], axis=0)
    importances = pd.DataFrame({"feature": FEATURES, "group": [GROUPS[f] for f in FEATURES], "lr_coef_raw": coef,
                                "gb_gain": imp / max(1e-9, imp.sum())}).sort_values("lr_coef_raw", key=np.abs, ascending=False)
    importances.to_csv(os.path.join(args.out, "importances.csv"), index=False)

    w = coef / max(1e-9, np.max(np.abs(coef)))          # normalized to max |w| = 1 (the composite is scale-free)
    # regime gates: per-regime logistic fits relative to the pooled fit, averaged per feature group
    gates = {k: dict(v) for k, v in DEFAULT_GATES.items()}
    pooled = np.abs(coef)
    for rkey in ("trend", "balance", "chop"):
        rmask = usable & df["regime"].isin([k for k, v in REGIME_KEY.items() if v == rkey]).to_numpy()
        if rmask.sum() < 1500:
            continue
        sc_ = StandardScaler().fit(Xf[rmask])
        lr = LogisticRegression(C=args.C, class_weight="balanced", max_iter=500).fit(sc_.transform(Xf[rmask]), y[rmask])
        rc = lr.coef_[0] / np.where(sc_.scale_ > 0, sc_.scale_, 1.0)
        for g in GROUP_NAMES:
            idx = [i for i, f in enumerate(FEATURES) if GROUPS[f] == g]
            num = float(np.sum(rc[idx] * np.sign(coef[idx]) * pooled[idx]))
            den = float(np.sum(pooled[idx] * pooled[idx]))
            if den > 0:
                gates[rkey][g] = float(np.clip(num / den, -2.0, 2.0))

    # threshold: smallest |DCS| level whose test hit rate reaches 55 % (bounded 20..70)
    gate_mat = np.ones((len(df), len(FEATURES)))
    reg = df["regime"].to_numpy()
    for i, f in enumerate(FEATURES):
        g = GROUPS[f]
        gate_mat[:, i] = np.where(np.isin(reg, [1, 2]), gates["trend"][g], np.where(reg == 3, gates["balance"][g], np.where(reg == 4, gates["chop"][g], 1.0)))
    dcs = dcs_from_weights(X, w, gate_mat)
    thr = 40.0
    for cand in range(20, 71, 5):
        m = test_mask & usable & (np.abs(dcs) >= cand)
        if m.sum() >= 100:
            hit = (np.sign(dcs[m]) * fwd[m] > 0).mean()
            if hit >= 0.55:
                thr = float(cand)
                break
    weights_path = os.path.join(args.out, "NQEdge_weights.txt")
    with open(weights_path, "w", encoding="ascii") as fh:
        fh.write("# NQ Edge Suite composite weights - learned by research/nqedge_research.py\n")
        fh.write(f"# rows={len(df)} folds={len(folds)} horizon={args.horizon}m label_atr={args.label_atr}\n")
        fh.write("version = 2\n")
        for f, wf in zip(FEATURES, w):
            fh.write(f"w.{f} = {wf:.4f}\n")
        for rkey in ("trend", "balance", "chop"):
            for g in GROUP_NAMES:
                fh.write(f"gate.{rkey}.{g} = {gates[rkey][g]:.3f}\n")
        fh.write(f"thr.signal = {thr:.0f}\n")
        fh.write(f"thr.fade = {thr / 2:.0f}\n")

    # ---- report ----
    def fmt(v):
        return "n/a" if (isinstance(v, float) and np.isnan(v)) else (f"{v:.3f}" if isinstance(v, float) else str(v))
    lines = ["# NQ Edge research report", "", f"Rows: {len(df)}  Days: {df['trading_day'].nunique()}  Folds: {len(folds)}  Horizon: {args.horizon} min  Label: |fwd| >= {args.label_atr} ATR",
             "", "## Metrics (walk-forward test windows only)", "", "| model | slice | n | hit | expR | PF | coverage |", "|---|---|---|---|---|---|---|"]
    for _, r in metrics.iterrows():
        lines.append(f"| {r['model']} | {r['slice']} | {r['n']} | {fmt(r['hit'])} | {fmt(r['expR'])} | {fmt(r['pf'])} | {fmt(r['coverage'])} |")
    lines += ["", "## Calibration (logistic)", "", cal.to_string(index=False), "", "## Feature importances", "", importances.to_string(index=False),
              "", f"## Exported weights -> {weights_path}", "", f"thr.signal = {thr:.0f}", ""]
    with open(os.path.join(args.out, "report.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print("\n".join(lines[:4 + len(metrics) + 4]))
    print(f"\nwrote {args.out}/report.md, metrics.csv, calibration_*.csv, importances.csv, NQEdge_weights.txt")

    if args.deploy and args.synthetic == 0:
        dst = os.path.join(args.data, "NQEdge_weights.txt")
        with open(weights_path, "r", encoding="ascii") as src, open(dst, "w", encoding="ascii") as out:
            out.write(src.read())
        print(f"deployed -> {dst} (the DCS study hot-reloads it within its reload interval)")


if __name__ == "__main__":
    main()
