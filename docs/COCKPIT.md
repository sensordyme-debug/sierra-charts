# The NQ Edge cockpit — the full multi-chart setup

This is the layout that puts everything the suite computes in front of you: one execution chart that
tells you what to do, context charts that tell you why, and the feeds that make the score better than
price alone. Chart numbers below are suggestions; Sierra assigns its own (`#n` in each title bar).

What it does and does not do: the Terminal prints **BUY NOW / SELL NOW / IN TRADE / WAIT / NO TRADE**
from a scored, back-validated setup engine, sizes the trade to your dollar risk, and stands you down at
your daily limit. It does not know the news, it cannot see the order book without a depth feed, and its
score is only as good as the weights it runs on. The first 25 trading days are for logging; after that
the research loop fits the weights to your symbol and the statistics in the HUD start to mean something.

## 1. Screens

### Screen A — execution (the only screen you trade from)

| Chart | Symbol / bars | Studies | What you read there |
|---|---|---|---|
| #1 | NQ (or MNQ / MES) **1-minute**, Fill Space 50–60, bar spacing ≥ 12 px | **NQ Edge Terminal** (PRO) | The action pill, the plan, footprint cells, levels, signal boxes, the profile, the event log. Alerts fire here. |
| #2 | same symbol **5-minute** | **NQ Edge Flow Candles**, **Flow CVD**, **Flow Delta** | Structure of the session: where absorption and exhaustion happened, the swing deltas, whether CVD confirms price. |
| #3 | same symbol **15-minute** or **30-minute**, 10 days loaded | **Flow Candles** (markers only; set *Candle Colour* = Up / down) + Sierra's own *Volume by Price* study docked right | The bigger auction: prior days' value areas, the week's range, where the big absorption prints sit. The Terminal already shows PWH/PWL/PDH/PDL pills, this chart shows the context around them. |

Arrange A as #1 taking two thirds of the width, #2 and #3 stacked on the right third.

### Screen B — feeds and internals (can be a second monitor or hidden tabs)

| Chart | Symbol / bars | Studies | Why it exists |
|---|---|---|---|
| #4 | **ES** 1-minute | Flow Candles | Relative strength NQ vs ES, lead / lag, SMT divergence. |
| #5 | **YM** 1-minute | Flow Candles | Same, against the Dow. |
| #6 | **RTY** 1-minute | Flow Candles | Risk appetite: small caps leading or lagging. |
| #7 | **NYSE TICK** (`TICK-NYSE` or your feed's symbol) 1-minute | none needed | Breadth: cumulative TICK, extremes, divergence against price. |
| #8–#11 | **AAPL, MSFT, NVDA, AMZN** 1-minute | none needed | Mega-cap leadership: are the names that move NQ confirming? |

Set the Terminal's *Chart Number: ES / YM / NYSE TICK / Mega Cap 1–3* inputs to these numbers. The HUD
internals line then reads, for example, `ES+ YM= RTY- | TICK +0.62 | AAPL+ NVDA+ MSFT- | YM leads`, and the
`mkt 8/8` on the health line confirms every feed is connected. Charts #4–#11 can be minimised; they only
need to exist and update.

### Optional

- **Market depth**: if your data feed includes depth, enable it in the chart; the engine uses it only
  for the health line today, the depth heatmap layer returns when the feed is there.
- **Sierra Trade DOM** for the execution symbol next to chart #1 for order entry.
- **Trade Simulation Mode** (Trade >> Trade Simulation Mode On) while you learn the system: the HUD's
  position line and the daily-risk guard read Sierra's own position and P&L, simulated or live.

## 2. Terminal inputs that matter

| Input | Set to |
|---|---|
| Session: RTH Start / End | 09:30 / 16:00 New York |
| Session: Trading Day Start | 18:00 |
| Risk Per Trade ($) | what one trade may lose. The size suggestion (`3x`) follows from the stop. |
| Risk: Daily Loss Limit ($) | your prop-firm daily limit minus a cushion (e.g. $1,000 limit → $700). |
| Risk: Max Trades Per Day | 4–6. Over-trading is the common failure mode, not bad setups. |
| Signals Outside RTH | Grade A only (default). Overnight MES/MNQ flow is thin. |
| Signals Shown: Minimum Grade | A and B. Grade C is logged for research, never traded. |
| Chart Numbers | the feeds above. |
| Alerts Enabled + Sound | yes; the alert text carries entry, stop, T1, T2 and size. |

## 3. How to read the HUD, top to bottom

1. **Bias pill** — `LONG DCS +62 ^`: the score, its sign is the bias, the arrow its five-bar trend.
2. **Action pill** — the one thing to do now:
   - `BUY NOW 7776.25 | stop 7772.00 | T1 7784.25 | 3x` — an A/B setup closed on the last bar. Enter at
     or near the close, stop and first target as printed, size as printed.
   - `IN LONG +0.6R | stop 7772.00 | T1 7784.25` — the trade is open; the R multiple is live. At `+1.0R`
     the pill adds `trail to entry`.
   - `WAIT | Buy pullback to VWAP 7770.25` — bias exists, location does not; the plan line says what
     has to happen first.
   - `NO TRADE | volatile chop` or `NO TRADE | daily limit hit` — stand aside.
3. **Regime** line and **day type / group agreement / range** line: context for the bias. Five of six
   groups agreeing is a strong score; two of six is a coin toss dressed up as a number.
4. **Plan** (two gold lines): location, trigger, invalidation, or the live trade's stop / T1 / R:R / size.
5. **MTF** strip and **CVD** line: do the higher timeframes and the order flow agree with the bias?
6. **Internals**: do the other indices, breadth and the mega caps agree?
7. **R / S** line: the nearest structural levels with tick distances.
8. **Last event**, **scoreboard** (the current setup's history), **position / daily P&L**, **clock**.
9. **Health**: `VAP on | depth off | mkt 8/8 | 0.8 ms | reg 1203px` — if it says `Fill Space >= N`, do it.

## 4. The trade, start to finish

1. **Pre-market (09:00–09:30)**: chart #3 for the overnight range and yesterday's value; the Terminal's
   pills show PWH/PWL, PDH/PDL, ONH/ONL, pdPOC. Note the open type the HUD assigns after 10:00.
2. **Open to IB end (09:30–10:30)**: the key-time lines mark it. The engine learns the day type here;
   most A-grade signals come after the IB closes, when value and structure exist.
3. **A signal fires**: the arrow and the box appear on the bar close, the action pill turns green or red,
   the alert sounds. The box is the trade: red part is the risk, green part the reward, dotted line T1.
4. **Enter** at the close of the signal bar or on the first pullback into the entry level (the plan says
   which). Never chase more than half a stop distance from the printed entry.
5. **Manage**: stop where the box says. At T1, take half and trail the rest to entry (the pill reminds
   you). T2 is the second liquidity level. The validation engine books the trade as the box shows it, so
   the scoreboard describes what you actually traded if you follow the box.
6. **Stand down** when the pill says `NO TRADE`, after the daily limit, after the max trade count, and in
   the last 15 minutes before the close unless you are managing a runner.

## 5. Daily routine

- Morning: `scripts\check.ps1` is not needed; just confirm `VAP on`, `mkt n/n`, `reg` non-zero.
- After the close: nothing; the feature logger writes `NQEdge_features_<symbol>.csv` automatically.
- Every 25 trading days: `python research\nqedge_research.py --data "C:\SierraChart\Data" --deploy`.
  The Terminal hot-reloads the weights. Watch the scoreboard's `n` grow; below 30 the odds are noise.
- Weekly: look at the cumulative-R subgraph of the *Signal Validation* diagnostic study on a spare chart.

## 6. What the companion charts add

- **Flow Candles** on the 5-minute: a swing-delta number that shrinks on each new high while price
  keeps climbing is the divergence the Terminal will later trade against; you see it forming.
- **Flow CVD**: CVD making a lower high while price makes a higher high during RTH is the strongest
  single warning the suite produces. The magenta dot marks the bar that confirmed it.
- **Flow Delta**: gold bars (absorption) at a level the Terminal lists as support or resistance are the
  trigger the plan line is asking for.
