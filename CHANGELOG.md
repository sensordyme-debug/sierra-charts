# Changelog

## Phase 1 — Scaffolding
- `docs/ARCHITECTURE.md`: structs, data flow, calculation model, complete input list.
- `docs/DECISIONS.md`: design decisions and unverified API calls.
- `scripts/check.ps1` / `check.sh`: syntax-only compile against `third_party/sierra` (clang via portable llvm-mingw, g++, or MSVC). `-Bootstrap` downloads the toolchain.
- `scripts/deploy.ps1`, `scripts/watch.ps1`.
- `src/sierra/NQEdgeSuite.cpp`: DLL skeleton with the per-chart registry, data stamp, params versioning, base pass (ATR, trading day, RTH sessions), drawing helpers, runtime warnings, GDI HUD, DCS bar painter, and all nine study functions with their full input sets. Engines 6–13 are stubs that size their arrays.
- `config/NQEdge_weights.txt`: default weights/gates/thresholds.
