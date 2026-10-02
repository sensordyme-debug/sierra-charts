# NQ Edge Terminal — Visual guide (v4, PRO by default)

Palette: bull `#00C896` green, bear `#FF4D5E` red, neutral `#6E7686` gray, VWAP/fib gold `#FFC857`,
levels cyan `#3EC6FF`, text `#E6EAF2`, dim `#788091`. Every colour is an input.

Two presets (input *Preset*): **PRO** (default) shows every layer below; **CLEAN** shows only the
first five. Each *Layer:* input is tri-state (*Preset default / On / Off*), so you can thin PRO out or
add one layer to CLEAN without losing the preset.

## What PRO looks like (screenshot description)

A dark chart, candles in the left two thirds, the right third empty (the fill space, 40–60 bars).
Reading from the candles outward:

- **Candles** in exactly three colours: green when the Directional Conviction Score of that closed bar
  is at or above +25, red at or below −25, gray between. The forming bar is **hollow** in its
  provisional colour.
- Above every one of the last 30 bars a small green or red number: the bar's **delta** (`+1.2k`,
  `-640`). Below it, dim, the bar's **volume**. These only appear when the bar spacing is 9 px or
  more, so zooming out removes them automatically. (Layer *Delta / Volume Per Bar*.)
- At each confirmed swing point a bold number, larger than the per-bar ones: the **cumulative delta of
  the leg** ending there, green or red, with `!` when the leg made a new extreme on weaker delta
  (divergence). No zigzag lines. (Layer *Swing Delta Numbers*.)
- A thin **regression channel** through the current leg: solid midline, dotted ±2σ rails, extended a
  few bars into the fill space, green/red by slope. (Layer *Regression Channel*.)
- A **2 px gold VWAP** with one faint ±1σ haze (transparency input, 88 % by default).
- **Dotted gold lines** from the last swing pivot into the fill space: the 38.2 / 50 / 61.8 / 78.6 %
  retracements of the last completed leg, each ending in a small gold pill (`61.8% 7731.75`). A fib tag
  is skipped where a level pill already sits. (Layer *Fib Levels (last leg)*.)
- **Six thin cyan lines**: the three nearest levels above price and the three below (POC, VAH/VAL,
  prior-day POC/VA/H/L/close, IB high/low, overnight H/L, naked POC, swings, liquidity, open). Each
  runs from where it was born to the pill column in the fill space and ends in an opaque cyan **pill**
  (`VAH 7741.25`). A level being tested turns solid with a white, bold pill. Equal prices share a pill.
- Faint **rectangles** extending right from where they were born, lettered in the corner: **A**
  absorption, **I** stacked imbalance, **L** liquidity pool, **F** failed-auction supply/demand; green
  support-type, red resistance-type, ≥ 82 % transparent, only within 2 ATR of price. (Layer *Zones*.)
- **Dots** at large aggressive prints in the last 90 bars, green buyer / red seller, sized by volume
  percentile. (Layer *Order-Flow Bubbles*.)
- Short **timestamped notes** next to the bar they belong to (`09:47 absorption: passive buyers`,
  `10:12 IB break up`, `10:31 SMT: MES HH, index no HH`, `trapped longs`, `acceptance above value`),
  stacked so they never sit on a candle. (Layer *Order-Flow Notes*.)
- When a grade A or B setup fires on a closed bar, a **green arrow** under the bar (red above). From
  that bar a thin red box (entry → stop) and a thin green box (entry → T2) extend right, ≥ 80 %
  transparent, with a dotted T1 line and one label at the box end:
  `LONG B | Trend Pullback | DCS +62 | R:R 2.3 | stop 7718.00 T1 7741.25`. After resolution the box
  ends at the resolution bar and the label reads `WIN +2.3R | Trend Pullback B` or `LOSS -1.0R | …`.
- From the last bar two **dashed segments** into the fill space: the expected pullback target, then
  the continuation target (in balance: rotation to POC, then the far edge), labelled with the empirical
  odds `pullback+go P(T1) 61% n=88` from the validation history of this setup × regime
  (`(low conf.)` under 30 samples, `no history yet` before any). (Layer *Projection Arrow*.)
- Docked at the **right edge of the fill space**, a horizontal **volume profile** of the developing RTH
  session growing leftward: red part = volume on the bid, green = on the ask, value-area rows brighter,
  gold POC line with a price pill, a faint gray ghost of the prior session behind it.
  (Layer *Volume Profile (docked right)*.)
- Along the **bottom 11 %** of the price region a colour-scaled table, one column per visible bar:
  Delta, Volume, Delta %, CVD change, Imbalances, DCS (the Sierra "calculated values" strip).
  (Layer *Calculated-Values Strip*.)
- In the fill space, in whichever half of the chart price is **not** in (so it never covers the level
  pills), nine lines of text, the **HUD**:
  1. A coloured **pill** `LONG` / `SHORT` / `NEUTRAL` + `DCS +62 ^` (score trend over five bars).
  2. `TREND UP | Open-Drive Up | value higher | Trend day`, coloured by regime.
  3. The **plan line** in gold: `Buy pullback to VWAP 7725.25 on absorption/delta flip | invalid < 7718.00`
     or, with a live signal, `LONG Trend Pullback B: entry 7726.00 | invalid 7718.00 | T1 7741.25`.
  4. `MTF 1m+ 5m+ 15m- 60m+ | leg R2 0.71 | SMT- | YM leads` (+ bullish, − bearish, = flat, . n/a).
  5. `CVD ^ z+1.4 | bar delta +820 (+18%) | Absorption (buyers) @ 7726.50 3b ago` — order flow.
  6. `R VAH 7741.25 +12t | S VWAP 7725.25 -8t | range 64% ADR` — structural levels only
     (VWAP bands and zones are drawn, not listed).
  7. `Trend Pullback: hist 58% | avg +0.42R | n=88 | 212 signals total` — scoreboard.
  8. `1h12m to close | bar 0:37 | MESZ26-CME` — session clock, bar countdown, symbol.
  9. `VAP on | depth off | mkt 4/4 | 0.8 ms` — data health, or the first warning in gold.

## What CLEAN looks like

Bias candles, VWAP + band, two nearest levels above and below with pills, A/B arrows + boxes, and a
six-line HUD (lines 1–4, 6 and 9 above). Nothing else.

## Hierarchy rules the renderer enforces

- Three line colours: gold (VWAP, fibs), cyan (levels), green/red (signals, channel, swing numbers).
- Every line is 1 px except the VWAP (2 px). Every fill is at least 80 % transparent.
- Labels sit in the fill space or offset above/below the bar, never on a candle; per-bar numbers are
  dropped when bars are narrower than 9 px; fib tags yield to level pills; the HUD moves to the half of
  the chart away from price and stays above the tape strip.
- Nothing is drawn more than 2 ATR from price except levels, fibs and the channel.
- Heavy layers (numbers, swing numbers, zones, bubbles, notes, fibs, channel) are redrawn when a bar
  closes; the HUD, pills and signal boxes every update. All drawings are managed by line number:
  adjusted in place, deleted when stale, all removed when the study is removed. Decisions, statistics
  and colours use closed bars only; the forming bar is hollow.
