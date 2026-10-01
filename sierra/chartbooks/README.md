# Chartbooks

Sierra Chart chartbooks (`.Cht`) are binary files that only Sierra Chart can write. Build the
chartbook once as described in `docs/SETUP.md` §3–§5, save it as `NQEdge.Cht`, and copy it
here. `scripts\deploy.ps1` copies every `*.Cht` in this folder into Sierra's Data folder.

Recommended layout for the chartbook:

| # | Chart | Bars | Notes |
|---|---|---|---|
| 1 | MESZ26-CME (later NQZ26-CME / MNQZ26-CME) | 1 min, 25+ days | all nine NQ Edge studies |
| 2 | YMZ26-CBOT | 1 min | Intermarket input "Chart Number: YM" |
| 3 | TICK-NYSE | 1 min | Intermarket input "Chart Number: NYSE TICK" |
| 4 | AAPL | 1 min | Intermarket input "Chart Number: Mega Cap 1" |
| 5 | AMZN-NQTV | 1 min | Intermarket input "Chart Number: Mega Cap 2" |

Charts 2–5 can be hidden (Window >> Hide) or tiled small; they only need to exist in the same
chartbook so `sc.GetChartBaseData` can read them.
