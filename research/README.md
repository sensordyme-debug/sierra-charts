# NQ Edge research loop

Turns the feature logs written by **NQ Edge: Feature Logger** into learned weights for the
Directional Conviction Score.

## One command

```powershell
pip install -r research\requirements.txt
python research\nqedge_research.py --data "C:\SierraChart\Data" --deploy
```

`--deploy` copies the learned `NQEdge_weights.txt` into Sierra's Data folder; the DCS study
hot-reloads it (default check every 5 s) and recomputes the score history. Without `--deploy`
the file lands in `research/output/` for inspection first.

Smoke test without market data: `python research\nqedge_research.py --synthetic 20000`.

## What it does

1. Loads every `NQEdge_features_<symbol>.csv` (one row per closed bar: OHLCV, ATR, the 26
   engine features, DCS, regime, setup flags, forward returns at +5/+15/+30/+60 min in ATR
   units, MFE/MAE).
2. Labels direction: `fwd<horizon>` beyond `±label-atr` ATR → up / down; smaller moves are not
   used for training.
3. Trains a regularized (L2) logistic regression first (interpretable raw-scale coefficients),
   then LightGBM (or scikit-learn HistGradientBoosting when LightGBM is absent).
4. **Walk-forward only**: `--train-days` of trading days → the next `--test-days`, rolled
   forward; no shuffling; scalers fit on the train window.
5. Reports hit rate, expectancy in R (1 R = `label-atr` ATR), profit factor, coverage and
   calibration per model, per setup and per regime, plus feature importances
   (`research/output/report.md`, `metrics.csv`, `calibration_*.csv`, `importances.csv`).
   The row `suite_signals` scores the signals the C++ study actually drew.
6. Exports `NQEdge_weights.txt`: `w.<feature>` from the pooled logistic coefficients
   (normalized to max |w| = 1), `gate.<trend|balance|chop>.<group>` from per-regime fits
   relative to the pooled fit (defaults kept when a regime has < 1500 labelled rows), and
   `thr.signal` = smallest |DCS| at which the test hit rate reaches 55 %.

## Options

`--horizon {5,15,30,60}` label horizon · `--label-atr` threshold · `--rth-only` ·
`--train-days/--test-days` window sizes · `--C` logistic regularization · `--gb-rounds` ·
`--margin` probability margin to take a side when scoring · `--out` output folder.

## Honest-statistics notes

* The logger writes a bar only once its +60 min forward window has closed, so there is no
  look-ahead in the rows themselves; the walk-forward split adds no leakage across windows.
* Expectancy here uses forward returns, not the structural stops of the live setups. The
  on-chart **Signal Validation** study is the authority for setup statistics; use this loop to
  learn which *features* carry information and to set weights/thresholds.
* Need ≥ 25–30 trading days of 1-tick data before the numbers mean much.
