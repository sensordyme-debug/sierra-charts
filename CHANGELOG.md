# Changelog

## Phase 1 — Scaffolding
- `docs/ARCHITECTURE.md`: structs, data flow, calculation model, complete input list.
- `docs/DECISIONS.md`: design decisions and unverified API calls.
- `scripts/check.ps1` / `check.sh`: syntax-only compile against `third_party/sierra` (clang via portable llvm-mingw, g++, or MSVC). `-Bootstrap` downloads the toolchain.
- `scripts/deploy.ps1`, `scripts/watch.ps1`.
- `src/sierra/NQEdgeSuite.cpp`: DLL skeleton with the per-chart registry, data stamp, params versioning, base pass (ATR, trading day, RTH sessions), drawing helpers, runtime warnings, GDI HUD, DCS bar painter, and all nine study functions with their full input sets. Engines 6–13 are stubs that size their arrays.
- `config/NQEdge_weights.txt`: default weights/gates/thresholds.

## Phase 2 — VWAP Engine + Auction/Structure Engine
- Auction engine: RTH volume profile from VAP (fallback: bar volume spread over H..L), developing POC/VAH/VAL, prior-day POC/VA/H/L, naked POCs (tested-through removal), TPO single prints (tails excluded), overnight H/L, initial balance + extensions, open-type classification, ATR-filtered N-bar swings confirmed N bars late, BOS/CHoCH, equal-high/low liquidity pools, location features (`vaPos`, `pocPos`, `ibPos`), value migration, level snapshot for targets.
- VWAP engine: session VWAP (RTH or trading-day anchor) with variance bands, overnight-anchored, RTH-anchored and swing-anchored VWAPs, ATR-normalized slope, acceptance/rejection state.
- Engine infrastructure: forming-bar fingerprint (`UpToDate/Stamp`) so repeated `Ensure*` calls in one update skip unchanged work; `dirtyFrom` so late-confirmed history (pivots, anchored VWAP back-fill) is mirrored to subgraphs.
- Auction study draws naked-POC rays, single-print and liquidity rectangles; swing/BOS/CHoCH markers as subgraphs.
