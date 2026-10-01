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
All nine phases are implemented (see CHANGELOG.md). The suite has been syntax-checked against the
Sierra headers but not yet built or run inside Sierra Chart; `docs/DECISIONS.md` lists the API
details to confirm on the first Remote Build.

## Working on the source
`src/sierra/NQEdgeSuite.cpp` is organised by `// ==== N` banners. Engines live in `namespace nqe`
(`EnsureBase` … `EnsureLog`), study functions (`scsf_NQEdge_*`) at the end. When adding a feature:
add the enum entry in `FeatureId`, the name in `kFeatureNames`, its group in `kFeatureGroup`, the
default weight in `Weights::SetDefaults` and `config/NQEdge_weights.txt`, the assignment in
`EnsureDcs`, and the column in `research/nqedge_research.py` (`FEATURES`/`GROUPS`).
