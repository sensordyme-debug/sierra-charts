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
