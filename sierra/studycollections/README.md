# Study collections

After adding the nine NQ Edge studies to the primary chart (see `docs/SETUP.md` §4) and setting
the intermarket chart numbers, save them with **Analysis >> Studies >> Save All Studies to Study
Collection**, name it `NQ Edge Suite`, and copy
`C:\SierraChart\Data\NQ Edge Suite.StdyCollct` into this folder. `scripts\deploy.ps1` copies
every file here back into Sierra's Data folder, and the collection can then be applied to any
chart with **Analysis >> Studies >> Study Collections**.

Study order inside the collection (matches the recommended add order):

1. NQ Edge: Auction/Structure Engine
2. NQ Edge: VWAP Engine
3. NQ Edge: Order Flow Engine
4. NQ Edge: Regime + MTF Bias
5. NQ Edge: Intermarket Engine
6. NQ Edge: Directional Conviction Score
7. NQ Edge: Signal Validation
8. NQ Edge: Feature Logger
9. NQ Edge: HUD + Bar Painter
