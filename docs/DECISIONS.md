# Decisions log

Ambiguities resolved while building, and anything not verifiable without a Sierra build.

## Design

1. **Cross-study data sharing via a DLL-global registry** (per-chart `ChartState`) instead of
   reading other studies' subgraphs. Reason: engines need rich state (zones, levels, signals,
   stats) that subgraph arrays cannot carry, and lazily computed engines make the suite
   independent of study-list order. Key per-bar features are still mirrored to subgraphs so
   they can be plotted and inspected.
2. **Lazy `Ensure*` engines with a data stamp** rather than relying on
   `sc.IsFullRecalculation`. Nine studies each receiving a full recalc would otherwise
   recompute the shared state nine times. The stamp (ArraySize + bar times at 0 / mid /
   computedThrough) detects reloads and back-fills; params are versioned per engine.
3. **Session / ATR settings live on the Auction/Structure study** (RTH start/end, ATR length).
   One place to edit; every other engine reads them from the registry.
4. **Session VWAP default anchor = RTH open.** The overnight-anchored VWAP covers the
   trading-day-start anchor, so both are available and distinct.
5. **Diagonal imbalance definition** follows Sierra's Numbers Bars: ask volume at price P
   compared with bid volume at P − 1 tick (buy imbalance) and bid at P vs ask at P + 1 tick
   (sell imbalance), ratio in percent with a minimum volume on both legs.
6. **Large trades:** Time & Sales only covers the recent buffer, so historical bubbles come
   from VAP (volume / number-of-trades per price level ≥ rolling percentile), flagged with a
   hollow marker; live bubbles come from T&S records ≥ rolling percentile of trade size.
7. **MTF bias is computed internally** by aggregating the primary chart's bars into 5/15/60 min
   buckets. Linked-chart MTF was not implemented (the spec allowed either). If the primary
   chart's bar period is larger than a cell's timeframe, that cell shows gray/unavailable.
8. **Pivot confirmation is delayed by the swing-strength N**; swing-based signals (CVD
   divergence, SMT, divergence setup) fire on the confirmation bar's close, never earlier.
9. **Validation assumes stop-first** when a bar touches both stop and target. 1-tick slippage
   on entry and stop (input).
10. **Bar painting is done by the HUD study** (it must be in region 0 for
    `DRAWSTYLE_COLOR_BAR`) using the DCS state from the registry; regime shading is a
    `DRAWSTYLE_BACKGROUND` subgraph on the Regime study.
11. **Local syntax check uses a portable llvm-mingw clang** downloaded into `tools/`
    (git-ignored) because no compiler was installed. It targets x86_64-w64-windows-gnu with
    `-fsyntax-only -std=c++17`, which compiles Sierra's own `Studies*.cpp` cleanly, so it is a
    faithful proxy for Remote Build's MSVC. MSVC `cl` is used automatically if found.
12. **File I/O** uses the C runtime (`fopen`) for the logger and weights file rather than
    `sc.OpenFile`; both are valid in ACSIL and the CRT gives `fseek`/`fgets`.
13. **Market depth** is never required. Depth-derived features are absent in this version
    (trial has no depth); the HUD reports "no depth" as informational only.

## Unverified without a Sierra build (check on first Remote Build)

* `sc.Graphics.FillRectangleWithColorTransparent` / `DrawRectangleTransparent` exist in the
  header (added at version 2910; installed Sierra is 2957). The HUD checks the pointer for
  null before use and falls back to opaque rectangles.
* `s_UseTool` with `DrawingType = DRAWING_RECTANGLEHIGHLIGHT` uses `BeginIndex/EndIndex` +
  `BeginValue/EndValue`, `TransparencyLevel` and `LineWidth = 1` for an outline (matches
  Sierra's `scsf_DrawingExample`).
* `DRAWSTYLE_BACKGROUND` with per-bar `DataColor` for regime shading (Sierra's
  "Out of Order Timestamps Detector" uses the style; per-bar DataColor is standard for
  all draw styles).
* `sc.GetChartTimeZone(ChartNumber)` returns a string; the New York check looks for
  "New_York" / "America/New_York" / "US/Eastern" case-insensitively.
* `sc.IntradayDataStorageTimeUnit`: the check treats values other than 0 (= 1 tick) as a
  warning. If your build reports the warning with the correct setting, the enum mapping
  differs — change `kStorageUnitTick` in the source.
* `sc.VolumeAtPriceForBars->GetNumberOfBars() < sc.ArraySize` is used (as Sierra's own
  VWAP study does) to detect VAP not yet loaded.
