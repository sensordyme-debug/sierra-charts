# NQ Edge Suite — Architecture

One DLL (`src/sierra/NQEdgeSuite.cpp`, `SCDLLName("NQ Edge Suite")`) that exports nine
ACSIL studies. Together they turn a Sierra Chart intraday chart into a directional decision
cockpit for NQ/MNQ (developed on MESZ26-CME as the trial proxy).

```
                 ┌──────────────────────────── one chart, one DLL ────────────────────────────┐
 Sierra data     │  Base pass ──► Auction/Structure ──► VWAP ──► Order Flow ──► Regime+MTF     │
 (bars, VAP,     │        │                                            │            │          │
  T&S, other     │        └──────────────► Intermarket ◄───────────────┘            │          │
  charts)        │                               │                                  ▼          │
                 │                               └──────────────► DCS composite ──► Setups     │
                 │                                                     │              │        │
                 │                     Validation ◄────────────────────┘              │        │
                 │                     Feature Logger ◄───────────────────────────────┘        │
                 │                     HUD + Bar Painter (GDI) ◄── reads everything            │
                 └─────────────────────────────────────────────────────────────────────────────┘
```

## 1. Studies (what you add to the chart)

| # | Study name (as shown in Sierra) | Function | Region | Precedence |
|---|---|---|---|---|
| 1 | NQ Edge: Auction/Structure Engine | `scsf_NQEdge_Auction` | 0 (price) | STD |
| 2 | NQ Edge: VWAP Engine | `scsf_NQEdge_VWAP` | 0 | STD |
| 3 | NQ Edge: Order Flow Engine | `scsf_NQEdge_OrderFlow` | own (CVD) | STD |
| 4 | NQ Edge: Regime + MTF Bias | `scsf_NQEdge_Regime` | 0 (background shade) | LOW |
| 5 | NQ Edge: Intermarket Engine | `scsf_NQEdge_Intermarket` | own | LOW |
| 6 | NQ Edge: Directional Conviction Score | `scsf_NQEdge_DCS` | own (histogram) | VERY LOW |
| 7 | NQ Edge: Signal Validation | `scsf_NQEdge_Validation` | own (cum. R) | VERY LOW |
| 8 | NQ Edge: Feature Logger | `scsf_NQEdge_FeatureLogger` | 0 (hidden) | VERY LOW |
| 9 | NQ Edge: HUD + Bar Painter | `scsf_NQEdge_HUD` | 0 | VERY LOW |

Add them in this order (the study collection in `sierra/studycollections/` already does).
Order is a *performance* preference, not a correctness requirement — see §3.

## 2. Shared state: the per-chart registry

All studies in the DLL share one `nqe::ChartState` per chart number, held in a DLL-global
registry (`std::map<int, ChartState*>` guarded by a recursive mutex). The registry is the
only way engines talk to each other; no study reads another study's subgraphs.

```cpp
struct ChartState {
    int chartNumber; int refCount;          // freed when the last NQ Edge study leaves the chart
    DataStamp stamp;                         // detects reloads / data changes (see §3)
    Params  params;                          // every engine's inputs, copied from sc.Input each call
    BaseState    base;     // ATR, true range, session/RTH flags, trading-day ids
    AuctionState auction;  // profiles, levels, swings, BOS/CHoCH, liquidity pools, open type
    VwapState    vwap;     // session + anchored VWAPs, bands, slope, acceptance state
    FlowState    flow;     // delta, CVD, absorption/imbalance zones, bubbles, trapped, divergence
    RegimeState  regime;   // regime enum + MTF cells
    InterState   inter;    // relative strength, SMT, TICK, mega-cap leadership
    DcsState     dcs;      // features, weights, DCS, signals
    ValState     val;      // signal outcomes and per-setup statistics
    LogState     log;      // pending CSV rows
    HudSnapshot  hud;      // what the GDI HUD draws
    Warnings     warn;     // runtime configuration warnings
};
```

Every engine state is **column-oriented**: `std::vector<float>` per feature, index = bar index,
sized to `sc.ArraySize`. Each engine keeps `computedThrough` (last *closed* bar whose values are
final). Values at `ArraySize-1` are provisional and recomputed on every call.

## 3. Calculation model (no repainting, order-independent, incremental)

* **Lazy, idempotent engines.** `EnsureBase/EnsureAuction/EnsureVWAP/...` compute bars
  `computedThrough+1 .. ArraySize-1`, then set `computedThrough = ArraySize-2`. Any study may
  call any `Ensure*`; dependents call their upstream `Ensure*` first. Whichever NQ Edge study
  runs first in an update cycle does the work; the rest find it done. This makes the suite
  immune to study-list ordering.
* **Closed bars only.** Everything that influences a decision (features, DCS, signals, zones,
  stats, log rows) is finalized only for bars `i <= ArraySize-2`
  (`sc.GetBarHasClosedStatus(i) == BHCS_BAR_HAS_CLOSED`). The forming bar gets a provisional
  value drawn dimmed/hollow and is never used in statistics or the logger.
* **Committed + forming accumulators.** Running sums (CVD, VWAP sums, session profile) are kept
  in "committed" form through `computedThrough`; the forming bar's contribution is computed on
  the side so re-evaluating the last bar never double-counts.
* **Data stamp.** `DataStamp` stores `ArraySize` and the bar times at index 0, mid and
  `computedThrough`. If any differ (reload, back-fill, more days loaded) the whole registry
  state for the chart is reset. `sc.IsFullRecalculation`/`UpdateStartIndex==0` are treated as
  hints; a matching stamp means the existing state is still valid and is reused (so nine studies
  doing a full recalc do not cause nine recomputations).
* **Params versioning.** Each study copies its inputs into `ChartState::params` on every call.
  If a parameter block changed, that engine and its dependents reset and recompute.
* **Performance.** Manual looping (`sc.AutoLoop = 0`), work bounded by new bars, no per-tick
  O(n²). Session profiles are tick-indexed vectors; POC/VA are recomputed only when the profile
  changes (once per closed bar, plus the forming bar).

## 4. Engines and their features

Every engine publishes normalized features in **[-1, +1]** per closed bar (NaN = unavailable;
unavailable features are dropped and the composite re-weights). Thresholds are in ATR units,
ticks, percentiles or z-scores only.

### Base pass
`atr[i]` (Wilder ATR, length input), `tr[i]`, `tradingDay[i]` (`sc.GetTradingDayDate`),
`isRTH[i]`, `rthSessionId[i]`, `secondsIntoRTH[i]`.

### A. Auction / Structure (`AuctionState`)
* Developing RTH volume profile from `sc.VolumeAtPriceForBars` (falls back to spreading bar
  volume over H..L when VAP is unavailable). POC, VAH, VAL (value-area % input).
* Prior-day POC/VAH/VAL, naked POCs (prior-session POCs untouched since), TPO single prints
  (30-min TPO periods, levels with exactly one TPO inside the profile body), overnight H/L,
  initial balance (first N minutes) with extension multiples, prior-day H/L.
* Value migration: today's VA vs yesterday's: `valueMig` ∈ {-1, 0, +1} (overlap ≥ 50 % = 0).
* Open type after the first 30 min: Open-Drive, Open-Test-Drive, Open-Rejection-Reverse,
  Open-Auction, each with a direction → `openType` ∈ [-1, +1].
* Swings: N-bar fractal pivots confirmed N bars later (confirmation index stored, never
  repainted), ATR distance filter. HH/HL vs LH/LL → `structTrend` ∈ {-1,0,+1}.
  BOS / CHoCH events decayed over K bars → `bos` ∈ [-1,+1].
* Liquidity pools: equal highs/lows (tolerance ticks), PDH/PDL, ON H/L.
* Location features: `vaPos` (above VAH +1 … below VAL -1, inside = scaled by VA width),
  `pocPos` = clamp((close-POC)/ATR), `ibPos`.

### B. VWAP (`VwapState`)
Session VWAP (anchor: RTH open or trading-day start) with variance bands ±1/2/3σ, anchored
VWAPs from overnight open, RTH open, last confirmed swing high and swing low.
`vwapPos` = clamp((close-VWAP)/(2σ)), `vwapSlope` = clamp((VWAP[i]-VWAP[i-k])/ATR / 0.5),
`vwapAccept` ∈ {-1,0,+1} (N closes beyond, or rejection wick).

### C. Order Flow (`FlowState`)
Per-bar delta (Ask−Bid volume), delta %, session CVD, CVD slope z-score; swing-based CVD
divergence; absorption (volume z ≥ X, range/ATR ≤ Y, close back inside) with zones; VAP
exhaustion (thin volume at the extreme after a run); stacked diagonal imbalances (ask@P vs
bid@P−1tick ratio ≥ 300 %, ≥ N consecutive) as zones active until traded through; large
trades from Time & Sales (live, rolling percentile) and from VAP (history, avg trade size
percentile) drawn as bubbles; trapped traders (breakout with strong delta fully reversed
within K bars). Features: `delta`, `cvdZ`, `cvdDiv`, `absorb`, `exhaust`, `imbalance`,
`trapped`, `largeTrade`.

### D. Regime + MTF (`RegimeState`)
Kaufman ER, ATR(fast)/ATR(slow) expansion, VWAP slope, value migration, IB range vs 20-day
average, fraction of last N bars inside value → **Trend Up / Trend Down / Balance / Volatile
Chop** with hysteresis (K bars). `regimeTrend` ∈ [-1,+1]. MTF: 1m/5m/15m/60m bars aggregated
internally from the primary chart; each cell = structure (HH/HL) + VWAP side + EMA slope →
{-1,0,+1}; `mtfBias` = weighted mean.

### E. Intermarket (`InterState`)
Charts by number (0 = off): YM, ES, RTY, TICK, up to 6 mega caps. Time-aligned with
`sc.GetContainingIndexForSCDateTime`. Relative strength z-score vs each index (`rsIndex`),
SMT divergence at confirmed swings (`smt`), TICK cumulative/extremes/trend divergence
(`tickCum`, `tickExt`, `tickDiv`), mega caps vs their own VWAP and EMA → `megaCap` breadth.

### F. DCS composite (`DcsState`)
`DCS = 100 · Σ w_f·g_r(f)·f / Σ |w_f·g_r(f)|` over available features, where `g_r` is the
regime gate for the feature's group (trend / flow / reversal / location / intermarket /
context). In Balance the *location* group is sign-inverted (value edges become fades) and
trend features damped; in Trend, reversal features are damped. Weights and gates come from
`NQEdge_weights.txt` (hot-reloaded on mtime change); defaults are compiled in.
Bar state: strong bull ≥ 60, weak bull ≥ 25, neutral, weak bear, strong bear (inputs).

Setups (bias + location + trigger, closed bars only): Trend Pullback, Value-Edge Rejection,
Failed Breakout / Trapped, Break-and-Acceptance, SMT/CVD Divergence at liquidity. Each signal
has entry, structural stop (+ATR buffer), T1/T2 at next liquidity, R:R, label and alert.

### G. Validation (`ValState`)
Replays each signal forward on closed bars with 1-tick slippage on entry and stop,
stop-first on ambiguous bars. Per setup: count, win %, avg R, profit factor, T2 %, MFE/MAE,
bars to resolution. Sample-size warning below 30.

### H. Feature logger (`LogState`)
One CSV row per closed bar, written once forward returns at +5/+15/+30/+60 min and MFE/MAE
are known. File: `<Data folder>\NQEdge_features_<symbol>.csv`.

## 5. HUD
GDI panel (`sc.p_GDIFunction`) drawn by the HUD study from `HudSnapshot`: bias + DCS + 5-bar
arrow, regime/open type/value migration, MTF strip, intermarket dots + SMT flag, order-flow
line, nearest S/R distances, plain-English state line, live stats for the current setup,
warnings. Bar painting by DCS state via a `DRAWSTYLE_COLOR_BAR` subgraph.

## 6. Inputs (complete list)

All colors are inputs. Defaults in parentheses.

**Auction/Structure Engine** — RTH Start (09:30), RTH End (16:00), ATR Length (14),
IB Minutes (60), Open-Type Minutes (30), Value Area % (70), Profile Ticks/Level (1),
Naked POCs Tracked (10), TPO Minutes (30), Swing Strength Bars (5), Swing Min ATR (0.5),
Equal H/L Tolerance Ticks (2), BOS/CHoCH Decay Bars (10), IB Ext Mult A/B/C (0.5/1.0/2.0),
Draw: Dev POC/VA, Prior-Day VA, Naked POCs, Single Prints, ON H/L, IB+Ext, PDH/PDL, Swings,
BOS/CHoCH, Liquidity (all Yes), Max Zone Drawings (40); colors ×13.

**VWAP Engine** — Session Anchor (RTH Open | Trading Day Start), Band Mult 1/2/3 (1/2/3),
Slope Lookback (10), Acceptance Closes (3), Draw Session VWAP+Bands (Y), Draw ON-anchored (Y),
Draw RTH-anchored (N), Draw Swing-anchored (Y); colors ×8.

**Order Flow Engine** — CVD Reset (RTH Open | Trading Day | Never), CVD Slope Bars (10),
Volume Z Length (50), Absorption Vol Z (2.0), Absorption Max Range ATR (0.6),
Absorption Zone Fraction (0.25), Exhaustion Run Bars (3), Exhaustion Extreme Vol % (15),
Imbalance Ratio % (300), Imbalance Min Volume (10), Stacked Levels (3),
Large Trade Percentile (99), Large Trade Min Size (20), Large Trade Lookback (2000),
Max Bubbles (100), Trapped Lookback (20), Trapped Reversal Bars (3), Trapped Min Delta % (20),
Divergence Min ATR (0.5), Event Decay Bars (8), Max Active Zones (30), Draw toggles ×7;
colors ×11.

**Regime + MTF** — ER Length (20), ER Trend (0.35), ER Balance (0.20), ATR Fast (7),
ATR Slow (50), Chop Expansion Ratio (1.3), IB Average Days (20), Inside-Value Lookback (30),
Hysteresis Bars (3), MTF EMA Length (20), MTF Swing Strength (3), Shade Background (Y);
colors ×4.

**Intermarket** — Chart numbers: YM, ES, RTY, TICK, MegaCap 1-6 (all 0), RS Lookback (20),
RS Z Length (100), TICK Extreme (800), TICK Strong (1000), TICK Lookback (10), TICK EMA (10),
MegaCap EMA (20); colors ×2.

**DCS** — Weights File (NQEdge_weights.txt), Hot Reload Seconds (5), Signal Threshold (40),
Fade Threshold (20), Smoothing (5), Strong/Weak Thresholds (60/25), Setup toggles ×5,
Require MTF Alignment (Y), Level Tolerance ATR (0.3), Stop Buffer ATR (0.5), Min R:R (1.0),
Min Target Distance ATR (0.5), Max Signals Drawn (30), Target Line Bars (20),
Alerts (Y), Alert Sound (1); colors ×11.

**Validation** — Slippage Ticks (1), Max Bars To Resolution (120), Stop First On Same Bar (Y),
Min Sample (30); colors ×2.

**Feature Logger** — Enabled (Y), File Prefix (NQEdge_features), Rewrite On Full Recalc (Y),
RTH Only (N).

**HUD** — Position (TL/TR/BL/BR), Preset (Minimal | Full), Font Size (11), Panel Width (340),
Panel Opacity % (80), Paint Bars (Y), Show Warnings/Stats/Intermarket/Order Flow/Levels (Y);
colors ×14.

## 7. Weights file (`NQEdge_weights.txt` in Sierra's Data folder)

```
# key = value, '#' comments. Unknown keys ignored. Missing keys keep defaults.
version = 1
w.vwapPos = 0.6        # feature weights (any real number)
...
gate.balance.location = -0.8   # regime gates: gate.<trend|balance|chop>.<group>
thr.signal = 40
thr.fade = 20
```
The Python research pipeline writes this file; the DCS study hot-reloads it.

## 8. Drawing management
ACSIL drawings use `sc.UseTool` with `UTAM_ADD_OR_ADJUST` and line numbers stored in the
registry per object. Line numbers are cleared on full recalculation and validated with
`sc.ChartDrawingExists` before reuse. Counts are bounded by inputs. Zones are
`DRAWING_RECTANGLEHIGHLIGHT` extended each bar while active; levels are subgraph lines or
horizontal rays; signals are markers + short horizontal segments + text.
