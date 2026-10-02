# First-time setup (Sierra Chart) — v2 terminal

## 1. Sierra settings the suite needs (the HUD warns when they are wrong)

| Setting | Where | Value |
|---|---|---|
| Intraday Data Storage Time Unit | Global Settings >> Data/Trade Service Settings | **1 Tick** (then *Edit >> Delete All Data and Download* on each chart once) |
| Time zone | Global Settings >> General Settings >> Time Zone (per chart: Chart >> Chart Settings >> Advanced) | **New York (US Eastern)** |
| Session times | Chart >> Chart Settings >> Session Times | Day 09:30–16:00, evening session on (18:00–09:30) |
| **Fill Space** | Chart >> Chart Settings >> Chart Data (or the *Chart >> Fill Space* control) | **40 bars or more** — the HUD, docked profile, level pills and projection arrow live in this space |
| Volume at Price | set by the studies; if the HUD still warns, Chart >> Reload and Recalculate | on |
| Market depth | optional; the trial has none. Depth layers hide automatically | — |

## 2. Graphics Settings (terminal theme)

A study cannot change everything, so set these once in **Global Settings >> Graphics Settings**
(or per chart: Chart >> Graphics Settings, untick *Use Global Graphics Settings*). Alternatively set
*Apply Terminal Theme To Chart Colors (once)* = Yes on the Terminal Overlay study, which applies the
chart-level items below through the API.

| Item | Value |
|---|---|
| Chart Background | `#0B0E14` (11,14,20) |
| Chart Grid / Grid Secondary | `#161B26` (22,27,38) |
| Chart Text | `#E6EAF2` (230,234,242) |
| Candlestick Up Fill / Down Fill | `#00C896` (0,200,150) / `#FF4D5E` (255,77,94) — overridden per bar by conviction candles |
| Candlestick Up Outline / Down Outline, Bar High-Low | `#8A93A6` (138,147,166) so wicks stay neutral gray |
| Value scale / time scale text | `#8A93A6` |

Tip: Chart >> Chart Settings >> *Use Global Graphics Settings* must be unticked for per-chart colours.

## 3. Build the DLL

1. `scripts\deploy.ps1` copies `NQEdgeSuite.cpp` into `C:\SierraChart\ACS_Source` and the weights file into `Data`.
2. Sierra Chart: **Analysis >> Build Custom Studies DLL >> Remote Build – Release**, select `NQEdgeSuite.cpp`, **Build**, wait for *Build successful*.

## 4. Two-chart chartbook (COMMAND 5-min + FOOTPRINT 1-min)

Open a new chartbook and open these charts (title bar shows each chart's number `#n`):

| Chart | Symbol (trial → paid) | Bars | Role |
|---|---|---|---|
| #1 | `MESZ26-CME` → `NQZ26-CME` / `MNQZ26-CME` | **5 min**, 25+ days | COMMAND chart (all 11 studies) |
| #2 | same symbol | **1 min**, 10+ days | FOOTPRINT chart (same studies, preset FOOTPRINT) |
| #3 | `YMZ26-CBOT` → `ESZ26-CME`, `RTYZ26-CME` | 1 min | index confirmation |
| #4 | `TICK-NYSE` → `TICK-NASD`, `ADD` | 1 min | breadth |
| #5 | `AAPL` | 1 min | mega cap 1 |
| #6 | `AMZN-NQTV` → `NVDA`, `MSFT`, `META`, `GOOGL` | 1 min | mega cap 2 |

Charts #3–#6 only need to exist in the chartbook (tile them small or hide them with Window >> Hide).
**Chart linking:** on #1 and #2 set Chart >> Chart Settings >> *Chart Link Number* to the same value (e.g. 1)
so symbol changes and crosshair/scroll follow each other (Global Settings >> Chart Linking for the options).

## 5. Add the studies (both price charts, in this order)

**Analysis >> Studies >> Add Custom Study >> NQ Edge Suite**:

1. NQ Edge: Terminal Backdrop (drawn under the candles; add first so it sits at the bottom of the study list)
2. NQ Edge: Auction/Structure Engine — RTH 09:30/16:00, ATR 14, Composite Days 5, ADR Days 10
3. NQ Edge: VWAP Engine
4. NQ Edge: Order Flow Engine
5. NQ Edge: Regime + MTF Bias
6. NQ Edge: Intermarket Engine — **Chart Number: YM = 3, NYSE TICK = 4, Mega Cap 1 = 5, Mega Cap 2 = 6** (your numbers may differ); leave unused at 0
7. NQ Edge: Directional Conviction Score
8. NQ Edge: Signal Validation
9. NQ Edge: Feature Logger (chart #1 only is enough)
10. NQ Edge: Order-Flow Tape (creates the bottom strip; *Strip Height* 13 %)
11. NQ Edge: Terminal Overlay — **Preset = COMMAND** on #1, **FOOTPRINT** on #2

The engines are shared and lazily computed, so the order only affects draw order (backdrop below,
overlay above). If a study collection exists in `sierra/studycollections/`, apply it instead
(Analysis >> Studies >> Study Collections).

Then **File >> Save Chartbook As** `NQEdge.Cht` and **Analysis >> Studies >> Save All Studies to
Study Collection** `NQ Edge Suite` (copy both into `sierra/` as described there).

## 6. Presets and layers

* *Preset* on the Terminal Overlay: **COMMAND** (candles + overlays + tape), **FOOTPRINT** (cells +
  profile + tape; needs bar spacing ≥ 34 px for `bid x ask` text, otherwise heat blocks), **CLEAN**.
* Every layer has a "Preset default / On / Off" input on the Overlay; colours are the *Theme:* inputs.
* *Show Diagnostic Regions* = Yes adds the CVD, intermarket, DCS histogram and cumulative-R regions back.
* *Signals Shown: Minimum Grade* = A only / A and B (default) / All. Grade C stays in the feature log.

## 7. Daily use

* Glance order: BIAS badge → plan line → projection arrow → nearest pills → tape.
* Alerts play the sound set in the DCS study for A/B signals on the newest closed bar.
* Feature logs land in `C:\SierraChart\Data\NQEdge_features_<symbol>.csv`; run the research loop
  (`research/README.md`) after ~25 trading days and deploy the learned weights (hot-reloaded).

## 8. Switching to NQ/MNQ

Change the symbol on the linked charts. Nothing is instrument-specific: thresholds are ATR/tick/z-score based.
