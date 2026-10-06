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

## v3.1 — first live feedback
- HUD text anchored 2 bars right of the last bar (a negative `BeginDateTime` counts bars into the fill space; the old value pushed the text under the price scale).
- VWAP anchor input on the Terminal, default trading-day start, so the gold line and band show overnight.
- Time-zone warning now based on the chart's UTC offset (no false positive).

## v4 — PRO preset (the "power" look, still readable)
- Terminal: *Preset* input (PRO default, CLEAN) and tri-state *Layer:* inputs (Preset default / On / Off) for 15 layers.
- New layers: Fib Levels of the last completed leg (38.2/50/61.8/78.6, dotted gold, pills in the tag column, tags yield to level pills),
  Delta / Volume Per Bar (last N bars, hidden when bar spacing < 9 px), Regression Channel of the active leg (midline + dotted ±2σ,
  extended into the fill space). Swing-delta numbers, zones, bubbles, notes, projection, docked profile and tape are PRO defaults.
- Levels: 3 above / 3 below in PRO (2+2 in CLEAN); pills placed left of the docked profile; the profile now docks at the right edge
  of the fill space (bars grow leftward, POC pill on its left).
- HUD: 9 lines in PRO (adds order-flow, scoreboard and session-clock lines), placed in the half of the chart away from price so it
  never covers the level pills, and above the tape strip when at the bottom. Nearest R/S and the plan line now use structural
  levels only (VWAP bands / zones excluded). Plan lines shortened to one HUD width.
- Text: all `·` separators replaced by ASCII `|` (the middle dot rendered as a box in the chart font).
- VWAP band transparency is an input (88 % default, re-applied when changed).
- Performance: heavy per-bar layers are redrawn only when a bar closes, the layer set changes or the engines were rebuilt
  (`SlotKeep`); HUD, pills and signal boxes every update. Slot table grows to ~250 managed drawings.
- Docs: VISUAL_GUIDE (PRO screenshot description), SETUP (fill space 40–60, bar spacing, re-add after upgrade), DECISIONS 41–48.

## v4.1 — readability fixes from the first PRO screenshot + signal quality
- Pills are right-aligned against the docked profile (`DT_RIGHT`), a pill that would overlap the previous one moves one column
  left; pill height and HUD row pitch come from the real region height (captured by the GDI pass in `TermState::regionH`).
- HUD rewritten as 12 compact lines (≈ 48 chars) placed at the end of the chart with fewer level pills; the plan is two lines;
  new lines: day type + feature-group agreement + range vs ADR, last event, scoreboard, clock. CLEAN = 7 lines.
- Scattered notes replaced by a six-line event log at the opposite end (newest first, duplicates within 6 bars collapsed) with
  dash markers at the event price. Event texts shortened (`sell imb stack`, `exhaust top`, `absorb: buyers`, `CHoCH down`).
- VWAP band default = dotted ±1σ lines; *VWAP Band Style* input (dotted / filled / off); fill transparency 90 %.
- Delta Per Bar shows delta only (volume stays in the strip), needs bar spacing ≥ 12 px.
- Exhaustion events need an above-average volume bar (z ≥ 0.5), a close back ≥ 30 % of the bar range, and a 6-bar
  same-direction cooldown — the "thin top / thin bottom" spam is gone.
- Stop sanity in `BuildSignal`: the structural stop is widened to at least max(0.6 ATR, 6 ticks); stops wider than 2.5 ATR
  reject the candidate. Inputs *Setup: Min Stop / Max Stop (ATR)*, *Min R:R To T1*.
- Feature-group agreement: per-bar bitmask of the sign of each weighted group; the grade gains +0.5 when ≥ 75 % of the groups
  lean with the trade and loses 0.5 under 50 %; the HUD shows `k/n groups agree`.
- *Signals Outside RTH*: Off / Grade A only (default) / All.
- $-risk sizing: *Risk Per Trade ($)* → contracts = floor(risk / (stop ticks × currency value per tick)); shown on signal
  labels (`3x`), in the plan (`size 3x`) and in alerts.
- HUD range line falls back to the prior RTH range outside RTH (`prev RTH 118% ADR`) instead of `0%`.
- Ghost profile drawn brighter when there is no developing session profile (pre-open / after the close).
- Docs: VISUAL_GUIDE, SETUP, DECISIONS 49–58, CLAUDE.md.

## v4.2 — footprint cells, back-fill-proof engines, pill-aware HUD
- **Footprint Cells (bid x ask)** layer (PRO default, GDI): one cell per traded price inside every visible bar, filled by delta/volume
  heat; `bid x ask` text plus delta above / volume below at ≥ 36 px bar spacing, the level delta at 20–35 px, heat only below;
  gold POC box, imbalance outlines (thicker when stacked), `u` for unfinished auctions, a 1 px frame in the bar's conviction colour.
  Nothing is drawn under 12 px spacing. Unsigned VAP volumes are converted to double before subtracting (the v2 renderer
  wrapped around when bid > ask).
- **Back-fill detection**: the data stamp now also compares a closed bar's volume and the mid bar's close, and the Terminal
  resets every engine on a full recalculation. The flat VWAP + jump seen on the delayed feed (bars re-filled with the same
  times) cannot recur.
- **Fill-space budget**: HUD text (48 chars), pills and the docked profile must fit side by side; the HUD font drops one point,
  then the profile narrows to 12 %, and the health line says `Fill Space >= N` with the computed number.
- **HUD placement** searches candidate positions (top, bottom, just below / above each level pill and fib tag) and takes the one
  covering the fewest pills; ties prefer the ends, then the half away from price.
- Per-bar delta numbers are staggered (odd bars one text height higher) under 26 px spacing and suppressed when the footprint
  prints them; neutral candles are shaded lighter for up bars and darker for down bars (two colour inputs).
- Docs: VISUAL_GUIDE, SETUP, DECISIONS 59–63.

## v4.3 — Flow series for the companion charts + trading-day boundary fix
- **NQ Edge Flow Candles** (`scsf_NQEdge_FlowCandles`, price region, any chart): candles coloured by a delta gradient (brighter on
  volume z ≥ 1.5; modes delta gradient / delta sign / up-down / off), absorption diamonds, exhaustion triangles, trapped-trader
  crosses, CVD-divergence plus marks, stacked-imbalance side dashes at the zone price, the bar's POC dash, buy/sell bubbles in
  three sizes (largest print in the lookback = L), delta number above and volume below the bar (`DRAWSTYLE_VALUE_ON_HIGH/LOW`),
  optional alert on absorption / trap / divergence. Sets the base, swing and flow parameters when the Terminal is absent.
- **NQ Edge Flow CVD** (own panel): cumulative delta coloured by direction with magenta divergence dots. Reader only.
- **NQ Edge Flow Delta** (own panel): delta histogram (gold on absorption bars, magenta on trapped bars), pressure line
  (EMA of delta), background shade on high-volume bars. Reader only.
- **Trading-day boundary** is now an input (*Session: Trading Day Start*, 18:00 default) on the Terminal and Flow Candles:
  the trading day rolls at that time instead of Sierra's `GetTradingDayDate`, which flipped at 20:12 on the user's chart and
  restarted the trading-day VWAP, the overnight range and the profiles mid-evening.
- Health line shows the region height the GDI pass captured (`reg 1203px`) to diagnose HUD row pitch.
- Docs: VISUAL_GUIDE (Flow series), SETUP §8, DECISIONS 64–67.

## v4.4 — the cockpit: action pill, internals, position / daily-risk guard, key times, prior-week levels
- **Action pill** (HUD line 2): `BUY NOW entry | stop | T1 | size`, `SELL NOW …`, `IN LONG +0.6R | stop | T1` (adds `trail to
  entry` at +1R), `WAIT | <plan>`, `NO TRADE | volatile chop / warming up / daily limit hit`. The live signal is now the newest
  unresolved one (validation decides), not "within five bars".
- **Internals line** (PRO): relative-strength signs for ES / YM / RTY, NYSE TICK value, mega-cap leadership, lead / lag; a
  prompt to set the chart numbers while none are configured.
- **Position + daily risk** (PRO): Sierra's trade position (live or Trade Simulation Mode) — `LONG 2 @ 7776.25 | open $+45 |
  day $-120 | limit $-500` or `FLAT | day … | trades n`. Inputs *Risk: Daily Loss Limit ($)* and *Max Trades Per Day*; when
  breached the line and the action pill turn into `DAILY LIMIT HIT | stand down` and signal alerts are suppressed.
- **Key Session Times** layer: dotted vertical lines with `OPEN`, `IB end`, `CLOSE` labels for the last two RTH sessions (on in
  both presets).
- **Prior-week high / low** (`PWH` / `PWL`) join the level set: pills, HUD R/S, setup reference levels and grading.
- HUD is 15 lines in PRO, 8 in CLEAN. `docs/COCKPIT.md` describes the full multi-chart layout, inputs, how to read the HUD, the
  trade from entry to exit, and the routine.

## v4.5 — auto trend lines + absorption bubbles
- **Trend lines from confirmed swings** (`FindTrendLine`): resistance through the newest swing high and the most recent
  earlier swing high whose connecting line no bar high cuts by more than 0.1 ATR; support the same way through swing lows.
  Solid while intact, dotted once a close breaks the line. Terminal layer *Trend Lines (auto, from swings)* (on in both
  presets, drawing objects extended into the fill space); Flow Candles draws them as line subgraphs up to the last bar.
  Input *Trend Lines: Swings Scanned* (8).
- **Absorption bubbles**: a point at the absorbed price on every absorption bar, sized by the bar's volume z-score (Terminal:
  8–24 px within 3 ATR of price, last 150 bars, in the Bubbles layer; Flow Candles: three size classes at z ≥ 2 / 3 / 4,
  default marker style, *Absorption Marker* input keeps the diamonds as an option).
- Docs: VISUAL_GUIDE, DECISIONS 73–74.

## v4.6 — live protocol: trade window + TradingView port
- **Trade window** (`DcsParams::tradeStartSec/EndSec`, Terminal inputs *Live: Trade Window Start / End*, default 10:00–15:30 ET):
  no signals outside it; the action pill says `NO TRADE | outside window 10:00-15:30` while nothing is open.
- **`tradingview/NQEdgeLite.pine`** — the system for TradingView's live data (Pine v5): trading-day VWAP ± 1σ, prior-day /
  overnight / IB / prior-week levels with tags, tick-rule delta from intrabars, CVD and its z-score, absorption / exhaustion /
  trapped / CVD-divergence detection, swing structure, regime with hysteresis, MTF bias (chart / 5 / 15 / 60), ES relative strength
  and NYSE TICK feeds, the gated-weight DCS, the five setups with stop sanity, liquidity targets, A/B grading, $-risk sizing,
  in-script forward validation (scoreboard), signal boxes + labels, auto trend lines, absorption bubbles, a HUD with the
  action line, trade window, max signals per day, and alerts (`alert()` once per bar close). Not compiled here: paste it in the
  Pine Editor and report any error line.
- `docs/LIVE_DAY1.md`: the day-one protocol (rules the Terminal enforces, the two live-data paths, pre-flight, session, after).

## TradingView 2.0 — NQ Edge Lite redesign (clean neon look + expectancy gate)
- Status line cleaned: every input `display=display.none`, every plot pane-only (the VWAP keeps its price-scale label).
- Signals: compact `A` / `B` tags with the full plan in the hover tooltip; one live position tool (risk and reward boxes,
  ENTRY with live R, STOP, T1, T2 tags) instead of a 30-bar box per signal; `+R` / `-R` outcome tags at the exit price.
- **Expectancy gate**: every A/B signal is tracked forward (shown or muted); a setup with ≥ 15 resolved trades and a negative
  running average R is muted (no marker, no alert) until its record recovers. Performance panel: per-setup N, win %, avg R,
  state (LEARN / ON / MUTED) and the SHOWN total.
- **Session volume profile** (RTH): incremental POC and value area; POC / VAH / VAL and prior-session pdPOC / pdVAH / pdVAL join
  the level set and the setups (value-edge fades at VAH / VAL, failed breakouts and break-and-accept through them); the profile
  is drawn in the right margin with a gold POC row.
- EMA 9/21 ribbon (visual and a with-trend filter: pullbacks and break-and-accept need it aligned).
- Look: conviction-gradient candles, glowing VWAP without reset jumps, violet ±1σ gradient cloud, glow-ring absorption bubbles at
  the absorbed price, subtle context marks (exhaustion only on *All*), glowing trend lines, nearest 3+3 levels with colour-coded
  monospace tags, faint trade-window tint, monospace dashboard (bias, action, plan, regime, MTF, flow, feeds, levels, value, status).
- Defaults follow the day-one protocol: grade B hidden, max 3 signals per day.
- `docs/TRADINGVIEW.md`: install, chart settings for the look, how to read the dashboard and the gate.
- **NQ Edge Flow** (`tradingview/NQEdgeFlow.pine`): companion pane with three modes (CVD with glow, gradient fill and divergence dots; Delta columns with absorption in gold, pressure line and high-volume shading; DCS oscillator linked from NQ Edge Lite via indicator-on-indicator). NQ Edge Lite exports `DCS` as a data-window plot for it. TRADINGVIEW.md: Plus section (data check, 10K bars, 4-chart layout, Bar Replay, mobile alerts).
