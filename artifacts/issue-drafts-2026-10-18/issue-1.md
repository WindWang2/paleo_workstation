# [med] #523 导出链仍直连外部 qgs* 头：qgs-audit 判红，qgis_spread 靠 #527 补登基线转绿而非收进封装

## 问题

本批新增的源数据/地质图导出链（#523）在功能层与视图层直接 include 外部 qgs* 头，越过 src/qgis 封装层，违反 docs/PALEO_QGIS_PLAN.md §2「QGIS 迁移成本只许落在 src/qgis」。区间末端（8db58138）qgis_spread_strict 与 qgs-audit 双 strict 闸红；区间后 #527 通过向 tools/qgis-spread-baseline.txt 补登 5 行把 qgis_spread_strict 转绿，但封装收敛并未发生——当前 HEAD（a0cd3a88）qgs-audit 仍判红。

## 证据

- 当前 HEAD（a0cd3a88）复跑 `python tools/check_layering.py --qgs-audit --strict` → 退出码 1，FAIL 2 个文件：`src/ui/dialogs/geologicalexportdialog.cpp`、`src/workflow/geologicalexport.cpp`；汇总「直连 118，baseline 116，新增 2」。
- 区间末端（8db58138）复跑：`python tools/check_qgis_spread.py --strict` → 退出码 1（6 个新增散布文件：geologicalexportdialog.{cpp,h}、vertexpartitionvalidation.cpp、paleomainwindow_ribbon.cpp、geologicalexport.{cpp,h}）；`--qgs-audit --strict` → 退出码 1（3 个文件，另含 paleomainwindow_ribbon.cpp）。
- #527（60ba3b28）对 tools/qgis-spread-baseline.txt 的改动为 +5 行（vertexpartitionvalidation.cpp、geologicalexport.{h,cpp}、geologicalexportdialog.{h,cpp}），注释自述「待收进 src/qgis 封装后删行」；同时把 ribbon 改为 `QgisCanvasController::hasLayers()`（正确的封装修法）——ribbon 因此从 qgs-audit FAIL 名单消失，剩余 2 个文件未照此办理。
- 接线事实：`CMakeLists.txt:994-1000` 注册 qgis_spread / qgis_spread_strict 两个 ctest 项；ci.yml lint 车道 Source gates 步跑 `--strict`；qgs-audit 则全仓零接线（见配套 issue）。
- 豁免面为空：tools/layering_vocab.json 的 qgs_audit_whitelist 为 []，无显式裁决。

## 影响

1. 闸门转绿靠基线增长而非代码收敛，「只缩不涨」纪律被倒置执行；与 #501（绿灯通胀）同族。
2. QGIS 升级/迁移成本继续落在封装层之外，PALEO_QGIS_PLAN §2 的分工被实质掏空。
3. qgs-audit 一旦按配套 issue 接线即红——「红闸 + 未接线」组合让违规长期无自动信号。

## 建议

- 把 geologicalexport.cpp 与 geologicalexportdialog.cpp 的 QGIS 调用（QgsPrintLayout / QgsLayoutItemMap / QgsProject / QgsReadWriteContext / QgsLayoutView）下沉到 src/qgis 薄壳——#527 的 ribbon 改动与既有的 geologicalmaplayout / dataexportwriter 封装即为范例。
- 修绿后从 tools/qgis-spread-baseline.txt 删掉 #527 补登的 5 行（只缩不涨）。
- 若短期必须直连，走 tools/layering_vocab.json 的 qgs_audit_whitelist 显式裁决，并在 PR 登记理由与 owner。

## 验证

`python tools/check_layering.py --qgs-audit --strict` 退出码 0；`python tools/check_qgis_spread.py --strict` 退出码 0 且 tools/qgis-spread-baseline.txt 行数不增（#527 补登的 5 行已删）；CI lint 车道连续两次 success。

## 关联

- 配套：qgs-audit include 级闸全仓零接线；QGIS 闸基线机制漂移（按文件计数放行已册文件无限追加）。
- 同族：#474（layering 防增长闸）、#501（绿灯通胀）；区间后修复 PR #527（补登基线转绿，未收封装）。
