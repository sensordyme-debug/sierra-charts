# NQ Edge Lite 2.0 on TradingView

## Install

1. TradingView → Pine Editor → open `NQ Edge Lite` (or a new indicator) → select all → paste
   `tradingview/NQEdgeLite.pine` → **Save** → **Add to chart**. Remove the old copy from the chart
   first, so you do not run two.
2. Chart: `CME_MINI:NQ1!` or `MNQ1!`, **5-minute** (3-minute also works). Live prices need TradingView's
   CME real-time data add-on; without it the chart is delayed like Sierra's trial.
3. Inputs to set once: *Risk per trade ($)*, *Point value* (NQ 20, MNQ 2), *Max signals per day*.
4. Alerts: Alert → Condition *NQ Edge Lite* → *Any alert() function call* → *Once Per Bar Close*.

## With TradingView Plus

**First check the data.** Plus does not include real-time CME prices; that is a separate add-on
(Profile → Account and billing → Market data subscriptions → CME Group / CME_MINI). If the symbol in the
legend shows a "D" / *delayed* badge, the chart is 10 minutes behind and the signals must not be traded.
Plus also does not unlock second-based timeframes or the volume footprint (Premium), so delta stays the
1-minute tick-rule approximation.

What Plus does unlock, and how this system uses it:

- **10K bars of history.** The performance panel and the expectancy gate learn from the loaded history,
  so more bars means more tracked trades per setup. Scroll the chart back to the start once after loading
  so TradingView loads the full 10K bars, then let the script recalculate.
- **Companion pane `tradingview/NQEdgeFlow.pine`.** Add it (Pine Editor → paste → Save → Add to chart)
  up to three times, each with a different *Show* setting:
  - *CVD*: session cumulative delta as a glowing line with a gradient fill and magenta divergence dots.
  - *Delta*: per-bar delta columns, brighter on stronger delta, gold on absorption bars, a cyan pressure
    line, and violet shading on high-volume bars.
  - *DCS (linked)*: the conviction score as an oscillator with the ±40 signal and ±25 bias lines. In its
    settings set *DCS source* to **NQ Edge Lite: DCS**.
- **4 charts per tab.** The layout below.
- **Bar Replay.** Rehearse before going live: Replay → pick yesterday 09:30 → play at 1–3× speed and act
  only on the dashboard's ACTION row. Alerts do not fire in replay; the markers and panel do.
- **Alerts to your phone.** One alert per chart (*Any alert() function call*, *Once Per Bar Close*) with
  *Notify in app* on; the mobile app delivers it.

### The 4-chart layout (layout button → 4 charts, then link *Crosshair* and *Time*, not *Symbol*)

| Position | Chart | Indicators | Purpose |
|---|---|---|---|
| Top left (largest) | NQ1! or MNQ1! **5-minute** | NQ Edge Lite, NQ Flow (CVD), NQ Flow (DCS linked), Volume | Decisions: ACTION row, signals, live position |
| Top right | same symbol **1-minute** | NQ Edge Lite with *Session volume profile*, *Performance panel* and *Dashboard* off | Entry timing at the level the plan names |
| Bottom left | same symbol **15-minute**, 10 days visible | NQ Edge Lite with signals off, TradingView *Session Volume Profile* (built in) | Context: prior days' value, the week's range |
| Bottom right | **ES1! 5-minute** | NQ Flow (CVD), NQ Flow (Delta) | Does ES confirm? NQ making a high while ES CVD diverges is a warning |

## Chart settings for the clean look (gear icon → Settings)

| Tab | Setting | Value |
|---|---|---|
| Symbol | Body / Borders / Wick | leave on; the script colours the bars. Wick colour `#56638A` for both directions |
| Status line | Indicator arguments | off (the script already hides its inputs) |
| Scales and lines | Indicators and financials value labels | on; the script only puts the VWAP on the scale |
| Canvas | Background | gradient `#0A0E17` → `#111827` |
| Canvas | Grid lines | none |
| Canvas | Margins → Right | **45 bars** (room for the level tags and the volume profile) |
| Trading | Buy/Sell buttons | off |
| Symbol → Session | Extended hours background | off, or a very dark `#0D1220` |

The built-in **Volume** indicator: either remove it or set its colours to 80 % transparency so it sits
quietly under the candles. Zoom so that about 150–250 bars are visible.

## What you see

- **Candles** coloured by the Directional Conviction Score: bright mint or pink at strong conviction,
  fading toward neutral, slate gray when the score is inside ±25.
- **VWAP**: a glowing cyan line, reset at 18:00 ET without a vertical jump, with a soft violet ±1σ cloud.
- **EMA ribbon** (9 / 21): a thin mint or pink cloud. Its colour is the short-term trend that with-trend
  setups require.
- **Signal markers**: a small `A` (or `B`) tag under a long bar or above a short bar. **Hover it** to see
  the full plan: entry, stop, T1 with R:R, T2, size.
- **Live position**: while a shown trade is open, a red risk box and a green reward box run from the
  signal bar to the last bar, with ENTRY (live R), STOP, T1 and T2 tags at the right edge.
- **Outcomes**: when a trade resolves, a small `+1.4R` or `-1.0R` tag at the exit price.
- **Key levels**: the three nearest levels above and below price as dotted lines with coloured tags
  (amber prior day, lavender overnight, cyan initial balance, pink prior week, gold value area / POC,
  gray swings). A level price is testing turns solid with a white tag.
- **Volume profile** of the RTH session docked in the right margin: violet rows, brighter inside the
  value area, gold POC row. Before the open it shows the last session.
- **Absorption bubbles** at the absorbed price, a glow ring around a solid core, bigger on heavier volume.
- **Context marks** (subtle): an X where breakout traders got trapped, a magenta diamond on a CVD
  divergence. Exhaustion triangles only with *Context marks = All*.
- **Trend lines** through the last valid swing highs and lows, solid until broken, then dotted.
- A very faint cyan tint marks the trade window.

## Dashboard (top right)

| Row | Meaning |
|---|---|
| NQ EDGE | bias and score: `LONG ▲ DCS +62 ↑` |
| ACTION | `BUY NOW 31229.25`, `SELL NOW …`, `IN LONG +0.6R`, `WAIT`, or `NO TRADE` |
| PLAN | for a signal: stop, T1, R:R, size. In a trade: stop, T1, when to trail. Waiting: what has to happen first |
| REGIME | trend up / down, balance or chop, IB state, ribbon direction |
| MTF | chart, 5m, 15m and 60m bias, swing structure |
| FLOW | CVD direction and z-score, bar delta, volume z |
| FEEDS | NQ against ES, NYSE TICK average |
| LEVELS | nearest resistance and support with tick distance |
| VALUE | POC and value area, and whether price is above, inside or below value |
| STATUS | trade window, signals used today, delta source |

## Performance panel (bottom right) and the expectancy gate

Every A and B signal is tracked forward on the chart's own history: stop first, then T1, timeout after
120 bars. The panel shows each setup's count, win rate and average R:

- **LEARN** — fewer than 15 resolved trades; shown normally.
- **ON** — at least 15 trades and average R ≥ 0; shown.
- **MUTED** — at least 15 trades and average R < 0; no markers, no alerts. It keeps being tracked in the
  background and turns back on if its record recovers.

The **SHOWN** row is the record of the signals you would actually have seen. That number, not any single
trade, is what tells you whether the system is working on your symbol and timeframe. Check it before
the open every day. If SHOWN is negative over 30+ trades, do not trade the signals; they are not an edge
on that chart.
