# NQ Edge Terminal — Visual guide (v3, CLEAN by default)

Palette: bull `#00C896` green, bear `#FF4D5E` red, neutral `#6E7686` gray, VWAP gold `#FFC857`,
levels cyan `#3EC6FF`, text `#E6EAF2`, dim `#788091`.

## What CLEAN looks like (screenshot description)

Picture a dark chart, candles in the middle, about a third of the width empty on the right
(the fill space). Reading left to right:

- **Candles** are painted in exactly three colours: green when the Directional Conviction Score of
  that closed bar is at or above the candle-bias threshold (+25 by default), red at or below −25,
  gray in between. A run of green candles means the model has leaned long for that stretch; a
  gray patch means no conviction. The bar that is still forming is **hollow** in its provisional
  colour, so you can tell at a glance it is not final.
- A **2 px gold line** winds through the candles: the session VWAP. Around it a faint gold haze,
  the ±1σ band, lets you see when price is stretched away from fair value. Nothing else is filled.
- **Four thin cyan lines** cross the recent bars: the two nearest levels above price and the two
  below (POC, VAH/VAL, prior-day levels, IB, ON high/low, naked POC, swings, liquidity, open).
  Each starts where the level was born and runs a couple of bars into the fill space, ending in a
  short cyan tag such as `VAH 7741.25`. When price is testing a level, its line turns solid and
  its tag white and bold; otherwise lines are dotted. Equal prices share a tag (`PDH/ONH`).
- When a grade A or B setup fires on a closed bar, a **green arrow** sits under the bar (or a red
  one above). From that bar a thin **red box** (entry → stop) and a thin **green box**
  (entry → T2) extend to the right, both ≥ 80 % transparent, with a dotted green T1 line. At the
  right end of the boxes one line of text: `LONG B · Trend Pullback · DCS +62 · R:R 2.3 ·
  stop 7718.00 T1 7741.25`. After the trade resolves the box ends at the resolution bar and the
  label reads `WIN +2.3R · Trend Pullback B` (or `LOSS -1.0R`). Only the three most recent
  signals near price are kept.
- In the empty space to the right of the last bar, top-aligned, six lines of text, the **HUD**:
  1. A coloured **pill** — green `LONG`, red `SHORT` or gray `NEUTRAL` — followed by `DCS +62 ^`
     (the glyph shows whether the score rose, fell or held over the last five bars).
  2. `TREND UP · Open-Drive Up · value higher · Trend day` (regime, open type, value migration,
     day type), coloured by regime.
  3. The **plan line** in gold: `Trend up (bias long). Wait for pullback to VWAP 7725.25 with
     absorption or delta flip. Invalid below 7718.00.` — location, trigger and invalidation.
  4. `MTF  1m+ 5m+ 15m- 60m+   leg R2 0.71` (+ bullish, − bearish, = flat, . unavailable) and an
     `SMT+/-` flag when the index failed to confirm the last swing.
  5. `R VAH 7741.25 +12t · S VWAP 7725.25 -8t · range 64% ADR` — nearest resistance and
     support with tick distances and today's range as a share of the average daily range.
  6. `VAP on · depth off · mkt 4/4 · 0.8 ms` — data health (or, in gold, the first configuration
     warning such as "Storage unit must be 1 TICK").

That is all. No zigzags, ribbons, backdrops, profiles or notes unless you switch a layer on.

## Layers (Terminal inputs, all off by default)

| Layer | What appears |
|---|---|
| **Volume Profile (docked)** | The developing RTH profile drawn horizontally in the fill space: red part = bid (aggressive selling), green part = ask, value-area rows brighter, gold POC line and pill; a faint gray ghost of the prior session behind it. |
| **Zones** | Up to 8 active zones within 2 ATR of price as faint rectangles extending right, letter in the corner: **A** absorption, **I** stacked imbalance, **L** liquidity pool, **F** failed-auction supply/demand. Green = support-type, red = resistance-type. |
| **Order-Flow Bubbles** | Dots at large aggressive prints (last 90 bars, within 2 ATR): green buyer, red seller, size = volume percentile. |
| **Swing Delta** | A small bold number at each recent swing point: cumulative delta of the leg ending there (green/red); `!` marks a divergence leg (new extreme with weaker delta). No connecting lines. |
| **Annotations** | Up to 12 short timestamped notes near recent events within 2 ATR (`09:47 absorption: passive buyers @ 7726.50`, `10:12 IB break up`, `10:31 SMT: MES HH, index no HH`, `trapped longs`, `acceptance above value`), placed above/below the bar and stacked so they never sit on a candle. Also adds a scoreboard line and a last-event line to the HUD. |
| **Projection** | Two dashed segments from the last closed bar into the fill space: expected pullback target then continuation target (or rotation to POC then the opposite edge in balance), labelled with the empirical `P(T1) 61% n=88` from the validation history of this setup × regime; `(low conf.)` under 30 samples. |
| **Tape Strip** | A colour-scaled table in the bottom 12 % of the price region: Delta, Volume, Delta %, CVD change, Imbalances, DCS per visible bar. |
| **VWAP + 1 Sigma Band / 4 Nearest Levels / Signal Arrows + Boxes / HUD / Bias Candles** | The CLEAN layers; each can be switched off individually. |

## Hierarchy rules the renderer enforces

- At most three line colours on screen: gold (VWAP), cyan (levels), green/red (signals).
- Every line is 1 px except the VWAP (2 px). Every fill is at least 80 % transparent.
- Labels sit in the fill space or offset above/below the bar, never on a candle.
- Nothing is drawn more than 2 ATR from price except the four nearest levels.
- All drawings are managed by line number: redrawn in place each update, deleted when stale, all
  removed when the study is removed. Decisions, statistics and colours use closed bars only; the
  forming bar is hollow.
