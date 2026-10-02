# Setup (Sierra Chart) — v4: one study, PRO preset

## 1. Settings the suite needs (the HUD's last line warns when they are wrong)

| Setting | Where | Value |
|---|---|---|
| Intraday Data Storage Time Unit | Global Settings >> Data/Trade Service Settings | **1 Tick** (then *Edit >> Delete All Data and Download* once per chart) |
| Time zone | Global Settings >> General Settings >> Time Zone | **New York** |
| Session times | Chart >> Chart Settings >> Session Times | Day 09:30–16:00, evening session on |
| **Fill Space** | Chart >> Chart Settings (or the Chart >> Fill Space control) | **40–60 bars** — HUD, level pills, fibs and the docked profile live there |
| Bar spacing | Chart >> Chart Settings or the zoom buttons | **≥ 9 px** so the per-bar delta/volume numbers show (they hide automatically when narrower) |
| Chart colours (optional) | Chart >> Graphics Settings | background `#0B0E14`, grid `#161B26`, candle outlines/wicks `#6E7686` |

## 2. Build

1. `scripts\deploy.ps1` (copies `NQEdgeSuite.cpp` into `C:\SierraChart\ACS_Source` and the weights file into `Data`).
   If PowerShell refuses to run scripts: `powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1`.
2. Sierra Chart: **Analysis >> Build Custom Studies DLL >> Remote Build – Release**, select `NQEdgeSuite.cpp`, **Build**.

## 3. Add ONE study

On the price chart: **Analysis >> Studies >> Add Custom Study >> NQ Edge Suite >> NQ Edge Terminal**.
Remove every other NQ Edge study from the chart. **After upgrading from v3 remove the old Terminal and add
it again** — v4 inserted inputs (Preset, tri-state layers, profile width, band transparency), so a v3
instance would read its old values into the wrong inputs.

Then set, in the Terminal's inputs:

- *Chart Number: YM*, *NYSE TICK*, *Mega Cap 1 (AAPL)*, *Mega Cap 2 (AMZN)* — the `#n` from the title
  bars of the other charts in the chartbook (open `YMZ26-CBOT`, `TICK-NYSE`, `AAPL`, `AMZN-NQTV` as 1-minute
  charts, tile them small or hide them). Leave the rest at 0. Until these are set the HUD shows `mkt 0/0`
  and the intermarket features are simply absent from the score.
- Optionally *Alert Sound Number*, *Signals Shown: Minimum Grade* (A and B by default).

Everything else works at its defaults. File >> Save Chartbook.

## 4. What you see (PRO)

Bias candles, delta/volume numbers per bar, swing-delta numbers, regression channel, gold VWAP + band,
fib retracements of the last leg, six nearest levels with right-edge pills, zones, bubbles, order-flow
notes, A/B signal arrows with risk/reward boxes, projection arrow with odds, a volume profile docked at
the right edge, a calculated-values strip along the bottom, and a nine-line HUD. `docs/VISUAL_GUIDE.md`
describes every element and the CLEAN preset.

## 5. Presets and layers

*Preset* = PRO or CLEAN. Each *Layer:* input is *Preset default / On / Off*. A clean way to work: start
in PRO, switch off what you do not read (typically *Delta / Volume Per Bar* or *Calculated-Values Strip*),
or start in CLEAN and add *Volume Profile* and *Swing Delta Numbers*. *Profile Width* (% of the fill
space) and *VWAP Band Transparency* are inputs.

## 6. Research loop

Feature logging is on by default (`C:\SierraChart\Data\NQEdge_features_<symbol>.csv`). After ~25 trading
days run `python research\nqedge_research.py --data "C:\SierraChart\Data" --deploy`; the Terminal
hot-reloads the learned `NQEdge_weights.txt`.

## 7. Diagnostics (optional)

The engine studies (Auction/Structure, VWAP, Order Flow, Regime + MTF, Intermarket, DCS, Validation,
Feature Logger) can still be added for their subgraphs and extra inputs. While the Terminal is on the
chart they do not change parameters; the Terminal's inputs win.
