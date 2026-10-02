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

## Phase 9 — Polish
- HUD: "Add study: …" warnings for missing engine studies (after the first full update), time-zone check against Sierra's POSIX string, performance line (update ms, max, last full recalculation, bar count) in the Full preset.
- Registry freed at DLL unload; validation no longer blocks on the first pending signal; logger append mode never truncates and skips rows already written; DCS threshold inputs default to 0 = taken from the weights file (an explicit value overrides).
- Docs: `docs/SETUP.md`, `sierra/chartbooks/README.md`, `sierra/studycollections/README.md`, decisions 14–25, CLAUDE.md contributor notes.

## v2 Phase 1–2 — Terminal layout, renderer, theme, candles, cloud, backdrop
- New GDI renderer (`nqe::render::Frame`): visible-bar geometry, fill-space detection, transparent fills (`FillRectangleWithColorTransparent`, opaque blend fallback), fonts, pills, arrows; per-paint frame time.
- Three terminal studies: **Terminal Overlay** (above the candles: DCS ribbon, HUD glass panel in the future space, conviction candles; owns preset/theme/layer switches), **Terminal Backdrop** (under the candles: regime tint, RTH/ETH shade, RTH open / IB end / close separators, translucent VWAP cloud tinted by slope), **Order-Flow Tape** (bottom strip table: Delta, Volume, Delta %, CVD change, Imbalances, DCS; sets its own region height).
- Presets COMMAND / FOOTPRINT / CLEAN with per-layer "Preset default / On / Off" overrides; "Show Diagnostic Regions" (default off) moves the CVD, intermarket, DCS histogram and cumulative-R plots into their own regions; by default every engine lives in the price region.
- Conviction candles: body fill by DCS state (`DRAWSTYLE_COLOR_BAR_CANDLE_FILL`), hollow forming bar; optional one-time application of the terminal palette to the chart's Graphics Settings.
- HUD moved into the fill space (auto-sizes, compact when the space is small): bias badge, gauge, 30-bar sparkline, regime/open/value row, MTF strip, intermarket dots with arrows, order-flow row with event age, levels row, plan line (wrapped), stats, health row, warnings.
- Removed all `sc.UseTool` drawings (naked POC rays, zones, bubbles, markers, signal lines); these become renderer layers.

## v2 Phase 3 — Level system
- Level rays from the bar where each level was born to a compact right-edge price pill, colour-coded by family (gold POC/VWAP/IB, cyan VA/liquidity, magenta naked POC, white PDH/PDL/PDC, dim gold extensions). Levels > 4 ATR away fade and become dotted; levels within the location tolerance pulse (solid bold pill). Identical prices merge (`VAH/PDH`), overlapping pills stack with a connector. Added PDC and session OPEN levels, prior-day close, RTH range history (ADR) and session profile history.

## v2 Phase 4 — Docked volume profile
- Developing RTH profile drawn horizontally in the fill space, each row split bid (red) / ask (green), value-area rows brighter, gold POC line + pill, cyan dotted VAH/VAL; faint ghost of the prior session; optional N-day composite (Auction input *Composite Days*).

## v2 Phase 5 — Zones, bubbles, swings, channel
- Zones as translucent rectangles extending right until mitigated (then dashed outline fading over ~40 bars) with a corner letter: A absorption, I stacked imbalance, L liquidity, P single prints, F failed-auction supply/demand (new; created at trapped-trader events).
- Large-trade bubbles sized by percentile with cluster counts; hollow while forming.
- Swing legs: zigzag with bold leg-delta numbers, leg volume + ticks, divergence `!`; active leg dashed with provisional delta.
- Regression channel (fit ±2σ, R²) on the active leg, colour by slope, extended into the future space.

## v2 Phase 6 — HUD glass panel
- Full panel in the fill space: bias badge, −100…+100 gauge with needle and threshold ticks, 30-bar sparkline, open type / value migration / day type guess, MTF strip + leg R², intermarket dots with arrows and SMT or lead/lag note, CVD row with event age, nearest levels with ticks/ATR, session clock (to RTH open / IB end / close, bar countdown) and range vs ADR, plan line, per-setup scoreboard, health row (VAP, depth, TZ, charts connected, draw/calc ms, data delay).

## v2 Phase 7 — Signal cards, grading, annotations
- Position boxes: green entry→T2, red entry→stop, dotted T1, white entry; live boxes extend right, resolved boxes end at the resolution bar with WIN/LOSS/TIMEOUT and R. Card label includes grade and historical hit rate for the setup.
- A/B/C grading from confluence (bias strength, location quality, trigger strength, intermarket agreement, regime fit); only A and B drawn/alerted by default (inputs).
- Event registry with timestamped notes (absorption, imbalance, trapped, exhaustion, CVD divergence, BOS/CHoCH, IB break, SMT, acceptance/rejection, signals), stacked to avoid overlap, fading with age, max N visible.
- Validation keeps statistics by setup × regime × grade.

## v2 Phase 8 — Projection arrow
- Dashed forward path from the last closed bar to the next liquidity (signal targets, or pullback→continuation in trends, rotation to POC→edge in balance) with the empirical `P(T1 first)` and n from the matching setup × regime (× grade) bucket; dimmed with `low confidence` under 30 samples; invalidation line; plan line now names location, trigger and invalidation.

## v2 Phase 9 — Footprint Pro + depth heatmap
- Footprint cells (`bid x ask`, delta heat background, diagonal imbalance borders, stacked highlight, gold POC box, unfinished-auction `u`, delta above / volume below) with automatic fallback to heat blocks and then to plain candles as bar spacing shrinks.
- Historical market-depth heatmap in the backdrop (bid/ask dominant side, size percentile intensity), only when depth data exists; requests historical depth storage when the layer is on.

## v2 Phase 10 — Signal power
- New features: `legEff` (delta per tick moved vs typical), `legVol` (leg volume vs prior leg), `pullback` (depth of the counter-leg as a share of the prior leg), `absorbQ` (volume z × (1 − range/ATR) × close position × repeat touches), `auction` (balance → initiative breakout → acceptance/rejection state machine per session, also feeds regime trendiness and the break-and-acceptance trigger), `leadLag` (rolling lagged correlation with YM / mega caps; flags a leader that moved while MES has not). 32 features total; logger writes them plus the signal grade; research pipeline and default weights updated.

## v2 Phase 11 — Performance + docs
- Renderer draws visible bars only, merges backdrop runs, caches fonts/text sizes per frame, bounds every list; frame times in the HUD health row.
- `docs/SETUP.md` rewritten (Graphics Settings palette, fill space, two-chart chartbook with linking, chart numbers, presets), new `docs/VISUAL_GUIDE.md`, decisions 26–33.

## v3 — One study, CLEAN by default
- New master study **NQ Edge Terminal** (`scsf_NQEdge_Terminal`): runs every engine itself (hidden `h.*` helper subgraphs), draws only in the price region, exposes the essential inputs (session, value area, swings, imbalance, intermarket chart numbers, model thresholds, setups, alerts, layers, colours). Adding this one study gives the whole system; the engine studies remain as optional diagnostics and stop fighting over parameters when the Terminal is present.
- CLEAN default: 3-shade bias candles (hollow forming bar), gold VWAP (2 px) + one translucent ±1σ band (`TRANSPARENT_FILL_TOP/BOTTOM`, study transparency 82 %), the 4 nearest levels (2 above / 2 below, equal prices merged) as 1 px lines with short right-edge tags that go solid/bold when price is within tolerance, A/B signal arrows with compact risk/reward boxes (≥ 80 % transparent) and a one-line label at the box end, text HUD (6 lines) in the space right of the last bar.
- Every drawing is a Sierra drawing object managed by line number: adjusted in place each update, deleted when no longer used, all deleted when the study is removed. No GDI for the default view.
- Removed: zigzag connecting lines, the DCS ribbon, regime/session backdrop, cloud, docked pills, the Overlay/Backdrop/Tape studies and all extra regions. Swing delta is now a small label at the swing point only.
- Layer inputs (all off by default): Volume Profile (docked, GDI), Zones (A/I/L/F rectangles within 2 ATR), Order-Flow Bubbles (markers within 2 ATR, last 90 bars), Swing Delta (last 24 legs), Annotations (last 12 events within 2 ATR, stacked), Projection (dashed path + empirical odds), Tape Strip (GDI, bottom 12 % of the price region).
- Hierarchy rules: lines 1 px except VWAP 2 px; line colours limited to VWAP gold, level cyan and signal green/red; nothing except the 4 nearest levels is drawn more than 2 ATR from price; fills ≥ 80 % transparent.
