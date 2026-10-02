# NQ Edge Suite — working notes for Claude / contributors

Sierra Chart ACSIL (C++) directional-intelligence suite for NQ/MNQ. Developed on MESZ26-CME.

## Layout
```
src/sierra/NQEdgeSuite.cpp   the whole DLL (one file; Remote Build compiles the selected .cpp)
third_party/sierra/          Sierra's real headers + example studies (git-ignored; copy from C:\SierraChart\ACS_Source)
scripts/check.ps1|.sh        syntax-only compile against the headers (clang/g++/MSVC; -Bootstrap downloads portable clang to tools/)
scripts/deploy.ps1           copy .cpp/chartbook/study collection/weights into C:\SierraChart, print next clicks
scripts/watch.ps1            re-run deploy on save
config/NQEdge_weights.txt    default composite weights (the research loop overwrites the copy in Sierra's Data folder)
research/                    Python pipeline (pandas / scikit-learn / LightGBM, walk-forward only)
sierra/chartbooks, sierra/studycollections   saved .Cht / study collection
docs/ARCHITECTURE.md         structs, data flow, full input list   docs/DECISIONS.md   ambiguities + unverified API calls
docs/SETUP.md                first-time Sierra setup
```

## Rules that shape the code
* Exact Sierra API only (`third_party/sierra/sierrachart.h`, `scstructures.h`, `VAPContainer.h`). No guessed signatures.
* No repainting: decision values final only for `i <= ArraySize-2`. Forming bar = provisional, drawn dim, never in stats/logs.
* Symbol-agnostic: thresholds in ATR / ticks / z-scores / percentiles. Use `sc.TickSize`, never hard-coded specs.
* Every tunable and every colour is an `sc.Input[]`.
* Manual looping (`sc.AutoLoop = 0`), incremental from `computedThrough+1`, persistent state in the per-chart registry.
* Run `scripts/check.ps1` (or `check.sh`) after every change. Nothing is done until it passes.
* Do not use `std::min/max` or `std::numeric_limits<T>::max()`: windows.h `min`/`max` macros are active (Sierra does not define NOMINMAX). Use `nqe::Min/Max`.
* Avoid identifiers that collide with Windows macros (`REG_NONE`, `ERROR`, `DELETE`, `IN`, `OUT`, ...).

## Architecture in one paragraph
All nine studies share one `nqe::ChartState` per chart (DLL-global registry, recursive mutex). Engines are lazy and
idempotent: `EnsureBase → EnsureAuction → EnsureVwap → EnsureFlow → EnsureRegime → EnsureInter → EnsureDcs → EnsureVal → EnsureLog`.
Each study copies its inputs into `ChartState::params` (byte-compared; a change resets that engine and its dependents),
checks the data stamp (reload/back-fill detection), calls its `Ensure*`, then mirrors the engine's per-bar arrays into its
subgraphs and manages its drawings. The HUD study draws with GDI from `HudSnapshot` and paints bars from `dcs.barState`.

## How each feature is computed
See docs/ARCHITECTURE.md §4 (engines) and the section banners (`// ==== N`) in the source.

## Build / test
```
.\scripts\check.ps1            # syntax check (first time: .\scripts\check.ps1 -Bootstrap)
.\scripts\deploy.ps1           # copy into C:\SierraChart and print the Remote Build clicks
```
Then in Sierra Chart: Analysis >> Build Custom Studies DLL >> Remote Build - Release >> NQEdgeSuite.cpp.

## Phase status
v1 (nine engine phases) and v2 (eleven terminal phases: GDI renderer, backdrop/overlay/tape studies,
presets, levels/pills, docked profile, zones/bubbles/swings/channel, HUD, cards/grades/notes,
projection, footprint/depth, signal-power features) are implemented — see CHANGELOG.md. v1 was
built and run in Sierra by the user; v2 is syntax-checked only. `docs/DECISIONS.md` 26–34 list the
v2 API details to confirm on the first Remote Build. `docs/VISUAL_GUIDE.md` explains every visual.

## v4.4 (current): one study, PRO preset, Flow series, cockpit
`scsf_NQEdge_Terminal` is the only study the trader adds. It sets every engine's parameters from its
own inputs (`S.terminalPresent` makes the engine studies passive) and draws with Sierra drawing
objects managed by line number (`term::Slot*` helpers, `TermState` slots, `SlotFlush` each update,
`SlotKeep` for heavy layers between bar closes; the GDI pass stores the region pixel size in
`TermState::regionH` so HUD row pitch and pill gaps are pixel-aware). Pills are right-aligned at the
docked profile's edge; notes are an event log (`S.events`, deduped) at the end opposite the HUD. Presets PRO (default: everything) and CLEAN (five
layers); `kTermPreset` rows + tri-state `Layer:` inputs decide `on[TL_*]`. GDI is used for the
footprint cells, the docked profile and the tape strip (`DrawTerminalGDI`, drawn over the candles).
The data stamp compares bar content (a closed bar's volume, the mid close) as well as times, and the
Terminal rebuilds all engines on a full recalculation. The trading day is derived from
`BaseParams::dayStartSec` (input, 18:00), not from Sierra's trading-day date. Section `// --- 10.`
holds the Flow series (`scsf_NQEdge_FlowCandles`, `FlowCVD`, `FlowDelta`): subgraph-only studies for
companion charts that share the engines; Flow Candles sets parameters only when the Terminal is absent. v4.4: `HudSnapshot::action/actionKind`
(derived from the newest unresolved signal or the plan), internals line (ES/YM/RTY/TICK/mega caps), position
and daily-risk guard from `sc.GetTradePosition` (`limitHit` suppresses alerts), `TL_KEYTIMES` vertical lines,
`LVL_PWH/LVL_PWL`. `docs/COCKPIT.md` is the multi-chart layout recipe. The v2 GDI overlay/backdrop/tape studies were
removed; `render::Frame`, `DrawProfile` and `DrawTape` remain. New Terminal layer: add a `TL_*` entry,
its name in `kTermLayerNames`, both `kTermPreset` rows, slots in `TermState`, and a draw block that
either redraws (when `heavy`) or `SlotKeep`s.

## v2 rendering rules (apply to the remaining GDI layers)
* Draw only `firstVis..lastVis`; anything beyond the last bar is positioned by `Frame::XOf` extrapolation.
* Opacity helper `Frame::Fill(..., alpha)` takes opacity %; never call the raw transparent fill directly.
* New layer: add to `enum Layer`, `kLayerNames`, the three `kPresetLayers` rows, a `render::Draw*`
  function, and a call in `DrawOverlay` (above candles) or `DrawBackdrop` (below).

## Working on the source
`src/sierra/NQEdgeSuite.cpp` is organised by `// ==== N` banners. Engines live in `namespace nqe`
(`EnsureBase` … `EnsureLog`), study functions (`scsf_NQEdge_*`) at the end. When adding a feature:
add the enum entry in `FeatureId`, the name in `kFeatureNames`, its group in `kFeatureGroup`, the
default weight in `Weights::SetDefaults` and `config/NQEdge_weights.txt`, the assignment in
`EnsureDcs`, and the column in `research/nqedge_research.py` (`FEATURES`/`GROUPS`).
