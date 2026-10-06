# Going live — the Day-1 protocol

Read this once tonight and once before the open. It is short on purpose.

## The truth about tomorrow

- The system has not been validated on your account, your symbol and your feed. Its weights are
  the compiled defaults; the research loop that fits them needs about 25 logged trading days. The
  scoreboard numbers you will see tomorrow are single digits and mean nothing yet.
- A prop-firm account is lost by over-trading and by trading without a plan, almost never by one
  bad signal. Everything below is designed to make that impossible.
- Sierra Chart's trial feed is **delayed ten minutes**. A delayed `BUY NOW` is a signal that was
  true ten minutes ago. Never act on it. Use one of the two live paths below.

## Live data: two paths

**Path A — Sierra with the prop firm's feed (best).** If Lucid gives you Rithmic, CQG or a Teton
login, set it in *Global Settings >> Data/Trade Service Settings* (service: the one named in your
welcome email), reconnect, and the chart's title bar stops saying `Delayed`. Everything in this suite
then runs in real time, and the Terminal's position line reads the live account.

**Path B — TradingView for the live chart, Sierra for research** (`docs/TRADINGVIEW.md` has the install
steps, the chart settings and how to read the dashboard and the performance panel). Add `tradingview/NQEdgeLite.pine`
as a TradingView indicator (Pine Editor >> paste >> Add to chart) on `CME_MINI:NQ1!` or `MNQ1!`,
2- or 3-minute bars (1-minute delta needs a timeframe above one minute; on a 1-minute chart the
bar's own tick rule is used). TradingView needs a CME real-time data subscription for live futures
prices; without it TradingView is delayed as well. Create two alerts on the indicator, condition
"Any alert() function call", frequency "Once Per Bar Close". Execute on the prop firm's platform.

## Day-1 rules (the Terminal enforces the first four)

| Rule | Where it is set | Why |
|---|---|---|
| Trade window 10:00–15:30 ET | Terminal *Live: Trade Window*; Lite *Trade window* inputs | The open is where untested systems die; the last half hour is for runners only. |
| Grade A only | Terminal *Signals Shown: Minimum Grade* = A only; Lite *Show grade B* off | B signals exist to be studied, not traded on day one. |
| Max 3 signals, $-risk sized | *Risk: Max Trades Per Day* = 3, *Risk Per Trade ($)* = what one loss may cost | Three losses is a bad day, not a blown account. |
| Daily loss limit = firm limit minus 30 % | *Risk: Daily Loss Limit ($)* | The guard turns the action pill red and stops alerts before the firm does it for you. |
| One contract (micro if available) | your order ticket | Size is earned by the scoreboard, not by confidence. |
| Stop and T1 exactly as printed | the signal box | The validation engine books what the box shows; follow it and the statistics describe your trading. |
| No trade without the pill | your discipline | If the pill does not say `BUY NOW` / `SELL NOW`, there is nothing to do. |

## Pre-flight (15 minutes before the open)

1. Sierra: Remote Build done, Terminal re-added (inputs changed), health line reads `VAP on`,
   `mkt n/n` with your feed charts connected, `reg` non-zero, no gold warning.
2. Trade >> Trade Simulation Mode **On** if you are not yet connected to the live account, so the
   position line and the guard have data; switch it off only when you mean to trade live.
3. Chart Settings: Fill Space as the health line asks, bar spacing ≥ 12 px.
4. TradingView (Path B): indicator loaded, HUD shows `delta: 1-min intrabars`, `feeds: ES on, TICK
   on`, alerts armed.
5. Write the three numbers on paper: risk per trade, daily limit, max trades.

## During the session

- 09:30–10:00: watch. The HUD classifies the open type; the pill says `NO TRADE | outside window`.
- 10:00 onward: act only on the pill. `WAIT` lines tell you what has to happen first, so you are
  never surprised by the signal.
- In a trade: the pill shows the live R. At `+1R` it says `trail to entry`; do it. T1 is where you
  take half.
- `NO TRADE | daily limit hit`: close the platform. Not "one more".

## After the close

- Nothing to do: the Terminal logged the day's features. In Sierra, the *Signal Validation*
  diagnostic study on a spare chart shows every signal's outcome.
- Write one line per trade: pill text, what you did, what you felt. That log is worth more than
  any indicator for the first month.
