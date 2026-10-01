# First-time setup (Sierra Chart)

## 1. Sierra settings the suite needs (the HUD warns when they are wrong)

| Setting | Where | Value |
|---|---|---|
| Intraday Data Storage Time Unit | Global Settings >> Data/Trade Service Settings >> Intraday Data Storage Time Unit | **1 Tick** (then *Edit >> Delete All Data and Download* on each chart once) |
| Time zone | Global Settings >> General Settings >> Time Zone (and Chart >> Chart Settings >> Advanced Settings if overridden per chart) | **New York (US Eastern)** |
| Session times | Chart >> Chart Settings >> Session Times | Day session 09:30–16:00, evening session on (18:00–09:30) so overnight bars exist |
| Volume at Price | handled by the studies (`sc.MaintainVolumeAtPriceData`); if the HUD still warns, Chart >> Reload and Recalculate | on |
| Market depth | optional; the trial has none. Depth features degrade gracefully | — |

## 2. Build the DLL

1. Copy the suite into Sierra: `scripts\deploy.ps1` (uses `C:\SierraChart`; pass `-SierraPath` otherwise).
2. In Sierra Chart: **Analysis >> Build Custom Studies DLL >> Remote Build – Release**, select
   `NQEdgeSuite.cpp`, click **Build**. Wait for *Build successful* in the Build Custom Studies DLL log.
   (If you edit the source later: save, run `scripts\deploy.ps1` or keep `scripts\watch.ps1`
   running, then Remote Build again. Sierra releases and reloads the DLL automatically.)

## 3. Build the chartbook (trial symbols)

Open a new chartbook (File >> New Chartbook) and open these charts (File >> Find Symbol or
File >> New/Open Intraday Chart). Use **1-minute bars**, **Days to Load = 25** or more:

| Chart | Symbol (trial) | Paid upgrade |
|---|---|---|
| Primary (traded proxy) | `MESZ26-CME` | `NQZ26-CME` / `MNQZ26-CME` |
| Index confirmation | `YMZ26-CBOT` | `ESZ26-CME`, `RTYZ26-CME` |
| Breadth | `TICK-NYSE` | `TICK-NASD`, `ADD` |
| Mega-cap leadership | `AAPL`, `AMZN-NQTV` | `NVDA`, `MSFT`, `META`, `GOOGL` |

Each chart shows its **chart number** in its title bar (`#1`, `#2`, …). Write them down.

## 4. Add the studies to the primary chart (in this order)

**Analysis >> Studies >> Add Custom Study >> NQ Edge Suite**:

1. NQ Edge: Auction/Structure Engine — set *RTH Start/End* (09:30 / 16:00) and *ATR Length*
2. NQ Edge: VWAP Engine
3. NQ Edge: Order Flow Engine
4. NQ Edge: Regime + MTF Bias
5. NQ Edge: Intermarket Engine — set the chart numbers: YM, TICK, Mega Cap 1 (AAPL), Mega Cap 2 (AMZN); leave unused ones at 0
6. NQ Edge: Directional Conviction Score
7. NQ Edge: Signal Validation
8. NQ Edge: Feature Logger
9. NQ Edge: HUD + Bar Painter

Order is a convenience, not a requirement: engines are shared and lazily computed, so any order
works; listing them this way just avoids one extra pass on the first update.

If a study collection file exists in `sierra/studycollections/`, use **Analysis >> Studies >>
Study Collections** to apply it instead of adding the nine studies by hand.

## 5. Save back into the repo

* **File >> Save Chartbook As** `NQEdge.Cht` (into `C:\SierraChart\Data`), then copy it to
  `sierra/chartbooks/`.
* **Analysis >> Studies >> Save All Studies to Study Collection** `NQ Edge Suite`, then copy
  `C:\SierraChart\Data\NQ Edge Suite.StdyCollct` to `sierra/studycollections/`.

`scripts\deploy.ps1` copies both back into Sierra on every run.

## 6. Daily use

* The HUD (top-left by default) shows bias, DCS, regime, MTF strip, intermarket dots, order-flow
  line, nearest support/resistance, the plain-English state line, live stats and warnings.
* Signals draw an arrow, entry/stop/T1/T2 lines and a label; Sierra plays the alert sound set in
  the DCS study inputs (*Global Settings >> General Settings >> Alerts* to pick the sound file).
* `NQ Edge: Signal Validation` shows the cumulative R curve; the HUD shows per-setup stats and a
  small-sample warning under 30 signals.
* Feature logs land in `C:\SierraChart\Data\NQEdge_features_<symbol>.csv`; run the research
  loop (`research/README.md`) after ~25 trading days and deploy the learned weights.

## 7. Switching to NQ/MNQ later

Change the primary chart's symbol (Chart >> Chart Settings >> Symbol). Nothing in the suite is
instrument-specific: thresholds are ATR/tick/z-score based and `sc.TickSize` is read at runtime.
