# [med] qgs-audit include 级闸全仓零接线：PALEO_QGIS_PLAN §2 的唯一 include 级执行者没进 ctest/CI

## 问题

`check_layering.py --qgs-audit` 是 docs/PALEO_QGIS_PLAN.md §2「QGIS 迁移成本只许落在 src/qgis」在 include 级的唯一机械执行者，但它在全仓零接线——没有 ctest 项、ci.yml 没有该步；而它当前是红的（2 个本批新增文件直连外部 qgs* 头且不在 baseline）。

## 证据

- `grep -rn "qgs-audit\|qgs_audit" CMakeLists.txt .github/workflows/*.yml tools/check_ci_scripts.py` → 零命中。CMakeLists.txt:975-1000 有 layering / layering_strict / layering_selftest / layering_symbol_audit / layering_transitive / qgis_spread* 共 9 项，独缺 qgs-audit；ci.yml Source gates 步（118-122 行）亦无。
- 当前 HEAD（a0cd3a88）复跑 `python tools/check_layering.py --qgs-audit --strict` → 退出码 1，FAIL `src/ui/dialogs/geologicalexportdialog.cpp` 与 `src/workflow/geologicalexport.cpp`；汇总「直连 118，baseline 116，新增 2」。
- 区间末端（8db58138）同命令为 3 个文件（另含 paleomainwindow_ribbon.cpp，#527 已将其改走 QgisCanvasController::hasLayers() 后转绿）。
- tools/qgs-audit-baseline.txt 本批只补了 #473 平移的 _commands/_wells 两行；qgs_audit_whitelist 为 []，即无显式裁决。

## 影响

include 级直连可以无限增长而无任何自动信号——本次 2 个新增直连全部靠人工复跑才发现；与 #474「有工具未接线」、#501「有约束无强制」同族。qgs-audit 与 qgis-spread 是两道互补闸（前者 include 级、后者符号级），只接后者意味着 include 级失控不可见。

## 建议

- 新增 `layering_qgs_audit` ctest 项（`check_layering.py --qgs-audit --strict`）+ ci.yml Source gates 一步，与 qgis_spread 同批接线。
- 接线前先按配套 issue 把 2 个文件的 QGIS 调用收进 src/qgis，避免一上线就红。

## 验证

`ctest --test-dir build -R layering_qgs_audit` 存在且 Passed；ci.yml lint 车道 Source gates 含 `python3 tools/check_layering.py --qgs-audit --strict` 且退出码 0。

## 关联

- 同族：#474（symbol-audit / transitive 未接线）、#501；依赖配套 issue「#523 导出链仍直连外部 qgs* 头」的封装收敛。
