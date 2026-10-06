# NQ Edge Suite

Advanced Sierra Chart (ACSIL C++) directional-intelligence system for intraday NQ/MNQ trading:
order-flow + auction-market + intermarket + regime engines fused into one Directional Conviction
Score, with non-repainting setups, structural stops, liquidity targets, on-chart signal statistics,
a heads-up display, and a Python research loop that learns the weights.

* Build: `scripts\check.ps1` (local syntax check) then Sierra Chart **Analysis >> Build Custom Studies DLL >> Remote Build**.
* Install: `scripts\deploy.ps1`.
* Going live: `docs/LIVE_DAY1.md`. TradingView version (live data): `tradingview/NQEdgeLite.pine`.
* Docs: `docs/SETUP.md` (first-time setup), `docs/COCKPIT.md` (the full multi-chart layout and how to trade it),
  `docs/VISUAL_GUIDE.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md`, `CLAUDE.md`.
* Research: `research/README.md`.
