# Changelog

## Phase 1 — Scaffolding
- `docs/ARCHITECTURE.md`: structs, data flow, calculation model, complete input list.
- `docs/DECISIONS.md`: design decisions and unverified API calls.
- `scripts/check.ps1` / `check.sh`: syntax-only compile against `third_party/sierra` (clang via portable llvm-mingw, g++, or MSVC). `-Bootstrap` downloads the toolchain.
- `scripts/deploy.ps1`, `scripts/watch.ps1`.
- `src/sierra/NQEdgeSuite.cpp`: DLL skeleton with the per-chart registry, data stamp, params versioning, base pass (ATR, trading day, RTH sessions), drawing helpers, runtime warnings, GDI HUD, DCS bar painter, and all nine study functions with their full input sets. Engines 6–13 are stubs that size their arrays.
- `config/NQEdge_weights.txt`: default weights/gates/thresholds.

## Phase 2 — VWAP Engine + Auction/Structure Engine
- Auction engine: RTH volume profile from VAP (fallback: bar volume spread over H..L), developing POC/VAH/VAL, prior-day POC/VA/H/L, naked POCs (tested-through removal), TPO single prints (tails excluded), overnight H/L, initial balance + extensions, open-type classification, ATR-filtered N-bar swings confirmed N bars late, BOS/CHoCH, equal-high/low liquidity pools, location features (`vaPos`, `pocPos`, `ibPos`), value migration, level snapshot for targets.
- VWAP engine: session VWAP (RTH or trading-day anchor) with variance bands, overnight-anchored, RTH-anchored and swing-anchored VWAPs, ATR-normalized slope, acceptance/rejection state.
- Engine infrastructure: forming-bar fingerprint (`UpToDate/Stamp`) so repeated `Ensure*` calls in one update skip unchanged work; `dirtyFrom` so late-confirmed history (pivots, anchored VWAP back-fill) is mirrored to subgraphs.
- Auction study draws naked-POC rays, single-print and liquidity rectangles; swing/BOS/CHoCH markers as subgraphs.

## Phase 3 — Order Flow Engine
- Per-bar delta / delta %, session CVD (RTH / trading day / never), rolling z-scores for volume and CVD slope via prefix sums over closed bars.
- Absorption zones (volume z, range/ATR, close back inside, new extreme), VAP exhaustion after a run, stacked diagonal imbalances (Numbers-Bars definition, ask@P vs bid@P-1), trapped traders (strong-delta breakout reversed within K bars), CVD divergence at freshly confirmed swings.
- Large trades: live from Time & Sales (rolling percentile, merged per price/bar) and historical from VAP average trade size per level; bubbles sized by volume. Zones die when traded through; drawings bounded by inputs.

## Phase 4 — Regime + MTF Bias
- Kaufman efficiency ratio (prefix sums), ATR fast/slow expansion ratio, inside-value fraction, IB width vs N-day average, VWAP slope / structure / value migration fused into trendiness + direction scores; Trend Up / Trend Down / Balance / Volatile Chop with K-bar hysteresis; `regimeTrend` feature; background shading by regime.
- MTF: 1/5/15/60-minute bars aggregated internally from the primary chart (committed when a new bucket starts), EMA and fractal pivots per timeframe; cell = structure + VWAP side + EMA slope; weighted `mtfBias`. Cells whose timeframe is finer than the chart's bar period are marked unavailable.

## Phase 5 — Intermarket Engine
- Reference charts by number (YM/ES/RTY/TICK/6 mega caps), refreshed each call, time-aligned with `GetContainingIndexForSCDateTime` and shifted back when the reference bar was still forming at the primary bar's close.
- Relative strength z-scores vs each index (`rsIndex`), SMT divergence at freshly confirmed swings (any index failing to confirm), NYSE TICK session average, extremes count and TICK-vs-price trend divergence, mega-cap leadership (own RTH VWAP + EMA) breadth. Missing charts are reported in the HUD warnings and their features are NaN (dropped by the composite).

## Phase 6 — Composite DCS, bar painting, setups, alerts
- Weights file loader (`NQEdge_weights.txt`, hot reload by mtime, `w.*`, `gate.<regime>.<group>`, `thr.*`), compiled-in defaults.
- DCS = 100 · Σ w·gate·f / Σ|w·gate| over available (non-NaN) features; regime gates (trend damps reversal/location, balance inverts location, chop halves everything); EMA-smoothed line; bar states (strong/weak bull/bear, neutral); gradient-coloured histogram with dim forming bar; bar painting via the HUD study.
- Five setups on closed bars with bias + location + trigger: trend pullback (VWAP/±1σ/POC + absorption/delta flip/stacked imbalance), value-edge rejection in balance, failed breakout/trapped traders, break-and-acceptance (two closes beyond IB/VA/PD/ON level with flow support), SMT/CVD divergence at liquidity. Structural stop + ATR buffer, T1/T2 at the next liquidity levels, R:R gate, label `LONG · Trend Pullback · DCS +72 · R:R 2.4 · T1 VAH`.
- Signal drawings (arrow, entry/stop/T1/T2 segments, label), bounded; `sc.SetAlert` only for a signal on the newest closed bar in real time.
- HUD snapshot: nearest support/resistance from the as-of level list, plain-English state line, MTF availability, intermarket dots with mega-cap names, current-setup stats hook.

## Phase 7 — Validation + Feature Logger
- Validation: every signal replayed forward on closed bars with configurable slippage (entry and stop), stop-first on ambiguous bars, T1 win / stop loss / timeout (mark-to-market) resolution, T2 tracking, MFE/MAE in R, bars to resolution; per-setup count/win %/avg R/PF/T2 %; cumulative-R curve subgraph; results feed the HUD with a small-sample warning.
- Feature logger: one CSV row per closed bar written only once the +60 min window has closed (forward returns at +5/+15/+30/+60 in ATR, MFE/MAE), file per symbol in the Data folder, rewrite on full recalculation (input), RTH-only filter.

## Phase 8 — Python research loop
- `research/nqedge_research.py`: loads the feature logs, labels direction beyond an ATR threshold, walk-forward (rolling train/test by trading day, no shuffling) logistic regression + LightGBM (scikit-learn HistGradientBoosting fallback), hit rate / expectancy in R / profit factor / coverage per model, setup and regime, calibration tables, feature importances, and exports `NQEdge_weights.txt` (weights, regime gates from per-regime fits, hit-rate-derived threshold) with `--deploy` into Sierra's Data folder. `--synthetic N` smoke test.
- `research/README.md`, `research/requirements.txt`.
