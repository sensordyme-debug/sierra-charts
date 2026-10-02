# NQ Edge Terminal — Visual guide (v4.2, PRO by default)

Palette: bull `#00C896` green, bear `#FF4D5E` red, neutral `#6E7686` gray, VWAP/fib gold `#FFC857`,
levels cyan `#3EC6FF`, text `#E6EAF2`, dim `#788091`. Every colour is an input.

Two presets (input *Preset*): **PRO** (default) shows every layer below; **CLEAN** shows only the
first five. Each *Layer:* input is tri-state (*Preset default / On / Off*), so you can thin PRO out or
add one layer to CLEAN without losing the preset.

## What PRO looks like (screenshot description)

A dark chart, candles in the left two thirds, the right third empty (the fill space, 40–60 bars).
Reading from the candles outward:

- **Candles** in three conviction colours: green when the Directional Conviction Score of that closed
  bar is at or above +25, red at or below −25, gray between (a lighter gray for up bars, darker for
  down bars, so a flat-score session still reads as price action). The forming bar is **hollow** in
  its provisional colour.
- **Footprint cells** inside every bar once bars are at least 12 px wide: one cell per traded price,
  filled by delta heat — green when more volume traded at the ask (aggressive buying), red at the bid,
  brighter for larger delta and volume. At 36 px or more each cell prints `bid x ask` and the bar gets
  its delta above and volume below; at 20–35 px each cell prints the level delta; narrower, heat only.
  The level with the most volume carries a gold box (the bar's POC), diagonal imbalances get a green
  or red outline (thicker when three or more stack), `u` marks an unfinished auction at an extreme, and
  a 1 px frame in the bar's conviction colour surrounds the column so the bias stays readable.
  (Layer *Footprint Cells (bid x ask)*.)
- Above each of the last 30 bars a small green or red number: the bar's **delta** (`+1.2k`, `-640`).
  Volume is not repeated on the chart (it is a row of the strip). The numbers appear only when the
  bar spacing is 12 px or more, so zooming out removes them. (Layer *Delta Per Bar*.)
- At each confirmed swing point a bold number, larger than the per-bar ones: the **cumulative delta of
  the leg** ending there, green or red, with `!` when the leg made a new extreme on weaker delta
  (divergence). No zigzag lines. (Layer *Swing Delta Numbers*.)
- A thin **regression channel** through the current leg: solid midline, dotted ±2σ rails, extended a
  few bars into the fill space, green/red by slope. (Layer *Regression Channel*.)
- A **2 px gold VWAP** with the ±1σ band as two thin **dotted gold lines** (*VWAP Band Style*: dotted
  lines / filled / off; the filled band of a trading-day VWAP covers most of a trend day, which is why
  dotted is the default).
- **Dotted gold lines** from the last swing pivot into the fill space: the 38.2 / 50 / 61.8 / 78.6 %
  retracements of the last completed leg, each ending in a small gold pill (`61.8% 7731.75`) in the
  pill column. A fib tag is skipped where a level pill or the HUD already sits. (Layer *Fib Levels*.)
- **Six thin cyan lines**: the three nearest levels above price and the three below (POC, VAH/VAL,
  prior-day POC/VA/H/L/close, IB high/low, overnight H/L, naked POC, swings, liquidity, open). Each
  runs from where it was born to its pill. The **pills** are opaque cyan, right-aligned against the
  docked profile; a pill that would sit on top of another moves one column to the left. A level being
  tested turns solid with a white, bold pill. Equal prices share a pill.
- Faint **rectangles** extending right from where they were born, lettered in the corner: **A**
  absorption, **I** stacked imbalance, **L** liquidity pool, **F** failed-auction supply/demand; green
  support-type, red resistance-type, ≥ 82 % transparent, only within 2 ATR of price. (Layer *Zones*.)
- **Dots** at large aggressive prints in the last 90 bars, green buyer / red seller, sized by volume
  percentile. (Layer *Order-Flow Bubbles*.)
- An **event log** of six lines at the end of the chart opposite the HUD, newest first and bold:
  `17:10 sell imb stack @ 7774.63 (1b)`, `16:35 absorb: buyers @ 7771.25 (8b)`, `15:05 CHoCH down
  @ 7788.50 (27b)`, `trapped longs`, `IB break up`, `acceptance above value`. Repeats of the same event
  within six bars collapse into the newest, and each listed event gets a short **dash marker** at its
  price on the bar it happened, so nothing is written across the candles. (Layer *Event Log + Markers*.)
- When a grade A or B setup fires on a closed bar, a **green arrow** under the bar (red above). From
  that bar a thin red box (entry → stop) and a thin green box (entry → T2) extend right, ≥ 80 %
  transparent, with a dotted T1 line and one label above the box at its start:
  `LONG B | Trend Pullback | R:R 2.3 | stop 7718.00 | 3x` — `3x` is the number of contracts that
  risks the *Risk Per Trade ($)* input between entry and stop. After resolution the box ends at the
  resolution bar and the label reads `WIN +2.3R | Trend Pullback B` or `LOSS -1.0R | …`.
- From the last bar two **dashed segments** into the fill space: the expected pullback target, then
  the continuation target (in balance: rotation to POC, then the far edge), labelled with the empirical
  odds `pullback+go P(T1) 61% n=88` from the validation history of this setup × regime
  (`(low conf.)` under 30 samples, `no history yet` before any). (Layer *Projection Arrow*.)
- Docked at the **right edge of the fill space**, a horizontal **volume profile** of the developing RTH
  session growing leftward: red part = volume on the bid, green = on the ask, value-area rows brighter,
  gold POC line with a price pill, a faint gray ghost of the prior session behind it (brighter before
  the open and after the close, when it is the only profile). (Layer *Volume Profile (docked right)*.)
- Along the **bottom 11 %** of the price region a colour-scaled table, one column per visible bar:
  Delta, Volume, Delta %, CVD change, Imbalances, DCS (the Sierra "calculated values" strip).
  (Layer *Calculated-Values Strip*.)
- In the fill space, wherever it covers the fewest pills (top, bottom, or between two pills), twelve
  compact lines (≈ 48 characters, row height sized from the real region height), the **HUD**. When the
  fill space is too narrow for HUD + pills + profile, the HUD font drops one point, the profile
  narrows, and the last line says `Fill Space >= N` with the number of bars that would fit everything:
  1. A coloured **pill** `LONG` / `SHORT` / `NEUTRAL` + `DCS +62 ^` (score trend over five bars).
  2. `TREND UP | Open-Drive up | value higher`, coloured by regime.
  3. `Normal day | 5/6 groups agree | range 64% ADR` — day type, how many feature groups (trend,
     flow, reversal, location, intermarket, context) lean the same way as the score, and today's range
     against the average daily range (`prev RTH …` outside RTH).
  4. The **plan** in gold: `Buy pullback to VWAP 7725.25` (or `LONG Trend Pullback B: entry 7726.00`).
  5. Its second line: `trigger absorb/delta flip | invalid < 7718.00` (or `stop … | T1 … | R:R 2.3 | size 3x`).
  6. `MTF 1m. 5m+ 15m- 60m+ | leg R2 0.71 | SMT-` (+ bullish, − bearish, = flat, . n/a).
  7. `CVD ^ z+1.4 | delta +820 (+18%) | YM leads` — order flow and lead/lag.
  8. `R VAH 7741.25 +12t | S VWAP 7725.25 -8t` — nearest structural levels (bands and zones are
     drawn, not listed).
  9. `last: 17:10 sell imb stack @ 7774.63 (1b)` — the newest event.
  10. `Trend Pullback: 58% win | +0.42R | n=88` — scoreboard of the current setup.
  11. `1h12m to close | bar 0:37 | MESZ26-CME` — session clock, bar countdown, symbol.
  12. `VAP on | depth off | mkt 4/4 | 0.8 ms` — data health, or the first warning in gold.

## What CLEAN looks like

Bias candles, VWAP + dotted band, two nearest levels above and below with pills, A/B arrows + boxes,
and a seven-line HUD (lines 1, 2, 4, 5, 6, 8 and 12 above). Nothing else.

## Hierarchy rules the renderer enforces

- Three line colours: gold (VWAP, fibs), cyan (levels), green/red (signals, channel, swing numbers).
- Every line is 1 px except the VWAP (2 px). Every fill is at least 80 % transparent.
- Labels sit in the fill space or offset above/below the bar, never on a candle; per-bar numbers are
  staggered on narrow bars and dropped under 12 px; fib tags yield to level pills and to the HUD; the
  HUD takes the position covering the fewest pills, the event log the opposite end, and both stay
  above the strip.
- Nothing is drawn more than 2 ATR from price except levels, fibs and the channel.
- Heavy layers (numbers, swing numbers, zones, bubbles, event log, fibs, channel) are redrawn when a
  bar closes; the HUD, pills and signal boxes every update. All drawings are managed by line number:
  adjusted in place, deleted when stale, all removed when the study is removed. Decisions, statistics
  and colours use closed bars only; the forming bar is hollow.
