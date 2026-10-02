# NQ Edge Suite v2 — Visual guide

What every colour, shape and label on the terminal means. Palette: bull `#00C896` green,
bear `#FF4D5E` red, neutral `#8A93A6` gray, VWAP gold `#FFC857`, levels cyan `#3EC6FF`,
naked POC magenta `#D65DFF`, text `#E6EAF2`, dim `#5C6577`, background `#0B0E14`.

## Price region (85 % of the height)

| Element | Meaning |
|---|---|
| **Candle body colour** | Directional Conviction Score state of that closed bar: strong bull (bright green), weak bull (dark green), neutral (gray), weak bear (dark red), strong bear (bright red). Wicks are neutral gray. The forming bar is hollow (provisional). |
| **DCS ribbon** (6 px strip at the bottom edge) | DCS per bar on a deep red → gray → deep green gradient; the last cell is dimmed (forming bar). |
| **Background tint** | Regime of each bar: green = Trend Up, red = Trend Down, cyan = Balance, gold = Volatile Chop. Darker shade = ETH (outside 09:30–16:00). |
| **Vertical separators** | Gold solid = RTH open; cyan dotted = IB end; dim dotted = RTH close. |
| **VWAP cloud** | Gold line = session VWAP. Filled bands: ±1σ (brighter) and ±2σ (lighter), tinted green/red when the VWAP slope is up/down. Dotted thin lines = ±3σ. |
| **Thin horizontal rays with right-edge pills** | Levels. Pill text = name + price. Gold = POC/VWAP/IB; cyan = VAH/VAL/liquidity; magenta = naked POC; white = PDH/PDL/PDC; gray-cyan = prior-day value; dim gold = IB extensions / VWAP bands. A level **fades** when it is more than 4 ATR away; it **pulses** (solid pill, bold) when price is within 0.3 ATR. Identical prices merge into one pill (`VAH/PDH`); overlapping pills stack and a short connector points to the real price. |
| **Translucent rectangles with a corner letter** | Zones extending right until mitigated: **A** absorption (green = buyers absorbed selling, red = sellers absorbed buying), **I** stacked imbalance (green buy / red sell), **L** liquidity pool (equal highs/lows, cyan), **P** single prints (gold-gray), **F** failed auction supply/demand (left behind by trapped traders). Mitigated zones become a dashed outline and fade over ~40 bars. |
| **Circles** | Large aggressive trades: green = buyer, red = seller, size = volume percentile, number inside = prints clustered at that price. Hollow = still forming. |
| **Zigzag + big numbers** | Swing legs. The bold number at each swing is the cumulative delta of the leg (green positive, red negative); the small line is leg volume and length in ticks. `!` in gold = divergence leg (new extreme with weaker delta). The dashed leg is the active leg with a provisional delta. |
| **Three thin parallel lines** | Regression channel on the active leg (middle = fit, outer = ±2σ), green/red by slope, extended dashed into the future space. Leg R² is in the HUD. |
| **Green/red boxes with an arrow** | Signal: green box = entry → T2 reward, red box = entry → structural stop risk, dotted line inside = T1, white line = entry. Live boxes extend right; resolved boxes end at the resolution bar. Card text: `LONG · Trend Pullback · DCS +72 · R:R 2.4 · T1 VAH · B · hist 58% n=41`; after resolution `WIN √ +2.4R · B` or `LOSS x -1.0R`. Only grades A and B are drawn by default. |
| **Small timestamped notes** | Event annotations: `09:47 absorption: passive buyers @ 7726.50`, `10:12 IB break up`, `10:31 SMT: MES HH, index no HH`, `trapped longs`, `acceptance above value`, `CHoCH up`. Newest are bright, older ones fade; they stack to avoid overlap. |
| **Dashed arrow into the future space** | Projection: the most likely path (pullback target, then continuation target) with a label `P(T1 first) 61% · n=88` from the validation history of this setup × regime (× grade when a signal is live). `low confidence` and a dimmed arrow when n < 30. The thin red dotted line is the invalidation level. |
| **Docked histogram in the future space** | Developing RTH volume profile: left part of each row = bid (aggressive selling, red), right part = ask (aggressive buying, green); value-area rows are brighter; gold line + pill = POC; cyan dotted = VAH/VAL. The faint gray silhouette behind it is the prior session (ghost); the faint cyan one is the N-day composite (off by default). |
| **Footprint cells** (FOOTPRINT preset) | Each cell = `bid x ask` at that price; cell shade = delta heat (green = ask-dominant, red = bid-dominant). Green/red border = diagonal imbalance ≥ 300 %; thick bright border = stacked (≥ 3). Gold box = bar POC. `u` = unfinished auction at that extreme. Number above the bar = delta, below = volume. When bars are too narrow, cells become heat-coloured blocks without text; narrower still, standard candles. |
| **Depth heatmap** (only with market-depth data) | Resting liquidity behind price from historical depth: green = bid side, red = ask side, intensity = size percentile. Hidden automatically without depth; HUD shows `depth off`. |

## Order-Flow Tape (bottom strip)

One column per visible bar, rows **Dlt** (delta), **Vol** (volume), **Dl%** (delta %), **CVD**
(CVD change over the slope window), **Imb** (stacked imbalance count, + buy / − sell), **DCS**.
Cell colour intensity = magnitude within the visible window (green/red diverging, cyan for volume).
Values are abbreviated (`1.2k`). Rows that do not fit are dropped from the bottom.

## HUD glass panel (top-right, future space)

1. **BIAS badge** LONG/SHORT/NEUTRAL + `DCS ±nn` and trend glyph (`^` rising, `v` falling, `=` flat over 5 bars); regime chip on the right.
2. **Gauge** −100…+100 with the needle at the current DCS and tick marks at ±signal threshold.
3. **Sparkline** of the last 30 closed-bar DCS values.
4. **Open type · value migration · day type** (IB forming / Normal / Normal var. / Trend day / Balance day).
5. **MTF strip** 1m/5m/15m/60m (green/red/gray, dark = unavailable for this bar period) and **leg R²**.
6. **MKT row** YM, TICK, mega caps as dots with `^`/`v`; `SMT+/-` pill or `YM leads 2b ^` lead/lag note.
7. **CVD row** CVD trend, z-score, last order-flow event with price and age.
8. **Levels row** nearest resistance/support with distance in ticks and ATR; **clock row** time to RTH open / IB end / close, bar countdown, today's range as % of ADR.
9. **Plan line** (gold, bold): location, trigger and invalidation in plain English.
10. **Scoreboard** per setup: win %, average R, profit factor, n (`*` = fewer than 30 samples).
11. **Health row** VAP, depth, time zone, intermarket charts connected, draw/calc milliseconds, `DELAY!` when data lags, and a reminder when the fill space is too small.
12. **Warnings** (gold) for wrong settings or missing studies.

## Presets

* **COMMAND** (default): everything above except footprint cells, depth heatmap and composite profile.
* **FOOTPRINT**: footprint cells + depth heatmap (when available) + profile, levels, zones, tape; bubbles, swing labels, channel and cloud off to keep the cells readable.
* **CLEAN**: conviction candles, DCS ribbon, levels + pills, signal cards, projection, HUD only.

Every layer can be forced On/Off on the Terminal Overlay study regardless of the preset.
