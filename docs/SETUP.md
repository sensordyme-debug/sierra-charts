# Setup (Sierra Chart) — v3: one study

## 1. Settings the suite needs (the HUD's last line warns when they are wrong)

| Setting | Where | Value |
|---|---|---|
| Intraday Data Storage Time Unit | Global Settings >> Data/Trade Service Settings | **1 Tick** (then *Edit >> Delete All Data and Download* once per chart) |
| Time zone | Global Settings >> General Settings >> Time Zone | **New York** |
| Session times | Chart >> Chart Settings >> Session Times | Day 09:30–16:00, evening session on |
| **Fill Space** | Chart >> Chart Settings (or the Chart >> Fill Space control) | **30–40 bars** — the HUD and the level tags live there |
| Chart colours (optional) | Chart >> Graphics Settings | background `#0B0E14`, grid `#161B26`, candle outlines/wicks `#6E7686` |

## 2. Build

1. `scripts\deploy.ps1` (copies `NQEdgeSuite.cpp` into `C:\SierraChart\ACS_Source` and the weights file into `Data`).
2. Sierra Chart: **Analysis >> Build Custom Studies DLL >> Remote Build – Release**, select `NQEdgeSuite.cpp`, **Build**.

## 3. Add ONE study

On the price chart: **Analysis >> Studies >> Add Custom Study >> NQ Edge Suite >> NQ Edge Terminal**.
Remove every other NQ Edge study from the chart (the old Overlay/Backdrop/Tape studies no longer exist;
the engine studies are optional diagnostics and are not needed).

Then set, in the Terminal's inputs:

- *Chart Number: YM*, *NYSE TICK*, *Mega Cap 1 (AAPL)*, *Mega Cap 2 (AMZN)* — the `#n` from the title
  bars of the other charts in the chartbook (open `YMZ26-CBOT`, `TICK-NYSE`, `AAPL`, `AMZN-NQTV` as 1-minute
  charts, tile them small or hide them). Leave the rest at 0.
- Optionally *Alert Sound Number*, *Signals Shown: Minimum Grade* (A and B by default).

Everything else works at its defaults. File >> Save Chartbook.

## 4. What you see (CLEAN)

Bias-coloured candles, gold VWAP with one faint band, the four nearest levels with right-edge tags,
A/B signal arrows with compact risk/reward boxes, and a six-line HUD in the space right of the last bar.
`docs/VISUAL_GUIDE.md` describes every element.

## 5. Layers

Switch on one at a time in the *Layer:* inputs: Volume Profile (docked), Zones, Order-Flow Bubbles,
Swing Delta, Annotations, Projection, Tape Strip. The CLEAN layers (Bias Candles, VWAP + 1 Sigma Band,
4 Nearest Levels, Signal Arrows + Boxes, HUD) can be switched off individually too.

## 6. Research loop

Feature logging is on by default (`C:\SierraChart\Data\NQEdge_features_<symbol>.csv`). After ~25 trading
days run `python research\nqedge_research.py --data "C:\SierraChart\Data" --deploy`; the Terminal
hot-reloads the learned `NQEdge_weights.txt`.

## 7. Diagnostics (optional)

The engine studies (Auction/Structure, VWAP, Order Flow, Regime + MTF, Intermarket, DCS, Validation,
Feature Logger) can still be added for their subgraphs and extra inputs. While the Terminal is on the
chart they do not change parameters; the Terminal's inputs win.
