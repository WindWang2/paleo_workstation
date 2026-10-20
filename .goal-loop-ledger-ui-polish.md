# UI polish ledger (goal/ui-polish-20261020, base origin/master e6837e14)
- [x] worktree + build started (uroot, PJ=8)
- [x] before screenshots (tst_ui snapshot harness, 1440x900)
- [x] diagnosis: chips elided, centered collapsible headers, redundant caption, predict dock overflow (4-button row + long combos)
- [x] fixes committed
  - `9ddc0887` chip bar scroll (no … elide); collapsible headers left-align; drop redundant 数据属性 caption
  - `2d01cc0a` predict run buttons → 2-row grid (narrow dock)
  - `085f79dc` mapping dock combos no longer size to longest option
  - size ratchet: mappingworkbenchpage kept ≤1637 via pageshared helper
- [x] after screenshots (67 pngs + tst_ui.log PASS exit=0 under /workspace/ui-polish-shots/after/)
- [x] compare montages (67/67 side-by-side 2900×930 under /workspace/ui-polish-shots/compare/)
- [x] static gates: layering/i18n/ui_invariants green; ui_size/ui_tokens/file_budget/qgis_spread/qgs_audit/docs_drift red set == master (verified on origin/master worktree); our deltas fixed (mapping ≤1637, buildCenterArea 208)
- [x] full rebuild (adaptive -j via /workspace/ujobs.sh) + ctest -LE perf: 397/411 pass; 14 red = 10 script gates (master-same) + tst_ui D61 legend (master-same, documented in prior ledgers) + tst_assetpaging/tst_pythonenv/tst_horizontracksvc (outside diff: data tree / python env / GDAL env)
- [x] test fix: tst_mappingpages finds collapsibleSectionToggle (was QToolButton → segfault)
- [x] chip scroll setWidgetResizable(false) so chips really scroll
- [x] push + PR

Shots root: `/workspace/ui-polish-shots/{before,after,compare}/`
