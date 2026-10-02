# Setup (Sierra Chart) — v4.4: one study, PRO preset, Flow series, cockpit

## 1. Settings the suite needs (the HUD's last line warns when they are wrong)

| Setting | Where | Value |
|---|---|---|
| Intraday Data Storage Time Unit | Global Settings >> Data/Trade Service Settings | **1 Tick** (then *Edit >> Delete All Data and Download* once per chart) |
| Time zone | Global Settings >> General Settings >> Time Zone | **New York** |
| Session times | Chart >> Chart Settings >> Session Times | Day 09:30–16:00, evening session on |
| **Fill Space** | Chart >> Chart Settings (or the Chart >> Fill Space control) | **40–60 bars** — HUD, event log, level pills, fibs and the docked profile live there; the HUD's last line says `Fill Space >= N` when it needs more at your zoom |
| Bar spacing | Chart >> Chart Settings or the zoom buttons | **≥ 12 px** for footprint heat and delta numbers, **≥ 20 px** for per-level delta, **≥ 36 px** for `bid x ask` text in every cell |
| Chart colours (optional) | Chart >> Graphics Settings | background `#0B0E14`, grid `#161B26`, candle outlines/wicks `#6E7686` |

## 2. Build

1. `scripts\deploy.ps1` (copies `NQEdgeSuite.cpp` into `C:\SierraChart\ACS_Source` and the weights file into `Data`).
   If PowerShell refuses to run scripts: `powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1`.
2. Sierra Chart: **Analysis >> Build Custom Studies DLL >> Remote Build – Release**, select `NQEdgeSuite.cpp`, **Build**.

## 3. Add ONE study

On the price chart: **Analysis >> Studies >> Add Custom Study >> NQ Edge Suite >> NQ Edge Terminal**.
Remove every other NQ Edge study from the chart. **After upgrading (v3 → v4 → v4.1) remove the old
Terminal and add it again** — inputs were inserted, so an old instance would read its saved values into
the wrong inputs.

Then set, in the Terminal's inputs:

- *Chart Number: YM*, *NYSE TICK*, *Mega Cap 1 (AAPL)*, *Mega Cap 2 (AMZN)* — the `#n` from the title
  bars of the other charts in the chartbook (open `YMZ26-CBOT`, `TICK-NYSE`, `AAPL`, `AMZN-NQTV` as 1-minute
  charts, tile them small or hide them). Leave the rest at 0. Until these are set the HUD shows `mkt 0/0`
  and the intermarket group is simply absent from the score and from the group count.
- *Session: Trading Day Start* — 18:00 for CME equity futures in New York time. The trading-day VWAP,
  the overnight range and the prior-day profile roll at this time (the suite no longer relies on the
  chart's session settings for it).
- *Risk Per Trade ($)* — the dollar risk behind the size suggestion (`3x` on signal labels, `size 3x` in
  the plan and the alert). It uses Sierra's currency value per tick for the symbol; if Sierra reports
  none, no size is shown.
- *Signals Outside RTH* — *Grade A only* by default; *Off* to trade RTH only, *All* for testing overnight.
- *Risk: Daily Loss Limit ($)* and *Max Trades Per Day* — the guard reads Sierra's trade position (Trade >>
  Trade Simulation Mode On while learning); when breached the HUD says `DAILY LIMIT HIT` and alerts stop.
- Optionally *Alert Sound Number*, *Signals Shown: Minimum Grade* (A and B by default), *Setup: Min /
  Max Stop (ATR)* and *Min R:R To T1*.

Everything else works at its defaults. File >> Save Chartbook.

## 4. What you see (PRO)

Footprint cells (bid × ask heat, POC, imbalances) inside bias-framed candles, delta numbers per bar,
swing-delta numbers, regression channel, gold VWAP with dotted ±1σ lines, fib retracements of the last leg, six nearest levels with right-edge pills, zones, bubbles, a
six-line event log with dash markers, A/B signal arrows with risk/reward boxes and contract size,
projection arrow with odds, a volume profile docked at the right edge, a calculated-values strip along
the bottom, and a twelve-line HUD. `docs/VISUAL_GUIDE.md` describes every element and the CLEAN preset.

## 5. Presets and layers

*Preset* = PRO or CLEAN. Each *Layer:* input is *Preset default / On / Off*. A clean way to work: start
in PRO, switch off what you do not read (typically *Delta Per Bar*, *Calculated-Values Strip* or
*Footprint Cells* on small timeframes), or
start in CLEAN and add *Volume Profile* and *Swing Delta Numbers*. *Profile Width* (% of the fill
space), *VWAP Band Style* and *VWAP Band Fill Transparency* are inputs.

## 6. Research loop

Feature logging is on by default (`C:\SierraChart\Data\NQEdge_features_<symbol>.csv`). After ~25 trading
days run `python research\nqedge_research.py --data "C:\SierraChart\Data" --deploy`; the Terminal
hot-reloads the learned `NQEdge_weights.txt`. This is where the score's accuracy comes from: the
compiled-in weights are a reasonable prior, the learned ones are fitted to your symbol and session.

## 7. Diagnostics (optional)

The engine studies (Auction/Structure, VWAP, Order Flow, Regime + MTF, Intermarket, DCS, Validation,
Feature Logger) can still be added for their subgraphs and extra inputs. While the Terminal is on the
chart they do not change parameters; the Terminal's inputs win.

## 8. Companion charts: the Flow series

On every other chart (another NQ timeframe, ES, YM, the mega caps) add **NQ Edge Flow Candles**
(price region) and, if you want the panels, **NQ Edge Flow CVD** and **NQ Edge Flow Delta** (they
open their own regions). Flow Candles carries the session, swing and order-flow inputs for that chart;
the panels read the same engines. Do not add the diagnostic *Order Flow Engine* study to a chart that
has Flow Candles (both set the flow parameters). On the main chart the Terminal owns the parameters and
Flow Candles simply draws alongside it.

## 9. The full cockpit

`docs/COCKPIT.md` is the complete multi-chart layout: the execution chart, the 5-minute and 15-minute
context charts with the Flow series, the ES / YM / RTY / TICK / mega-cap feed charts and the chart numbers
that connect them, the inputs that matter, how to read the HUD top to bottom, and the trade from the
signal bar to the exit.
