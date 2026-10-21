# [med] QGIS 闸基线机制漂移：按文件计数放行已册文件无限追加，qgis-spread 出生即带 148 条冻结基线

## 问题

qgs-audit / qgis-spread 按「文件」计数而非按 include/符号增量计数，已册文件成为永久豁免区——本轮在功能层新增整套 URI→QgsMapLayer 重建（含 CRS WKT 往返）、在视图层新增 3 个外部 qgs 头，全部落在 baseline 已册文件上，两个 strict 闸对本维度全绿。叠加：本范围新建的 qgis-spread 闸出生即带 148 条冻结基线且合入时已红，偏离 AGENTS.md 为 layering-baseline 立下的「残留归零 + --strict 非空即红」纪律；区间后 #527 又向基线补登 5 行转绿，是「补登而非收敛」的又一实例。

## 证据

- 已册文件新增直连（逐行复核过）：`src/workflow/predictionworkflow.cpp:398/402` `std::make_unique<QgsRasterLayer>(job->inputUri,…)` / `QgsVectorLayer`、:408 `QgsCoordinateReferenceSystem::fromWkt`、:272 `input->crs().toWkt()`，新增 include `<qgscoordinatereferencesystem.h>` / `<qgsrasterlayer.h>` / `<qgsvectorlayer.h>`（tools/qgs-audit-baseline.txt:125 已册）；`src/ui/paleomainwindow_attach_mapping.cpp:26-30` 新增 `<qgslayoutitemlabel.h>` / `<qgsmaplayer.h>` / `<qgsproject.h>`（:78 已册）；`src/workflow/mappinghierarchy.cpp:12` 新增 `<qgsfield.h>` 并 :99-113 直连 layer->beginEditCommand / addAttribute / changeAttributeValue / endEditCommand。
- tools/qgis-spread-baseline.txt 头部自述「148 = 146（issue-sweep 初冻）+ 2（#516 references/panel 新文件）」并声明「只缩不涨」，而 AGENTS.md 对 layering-baseline 的口径是「已归零——--strict 闸门下非空即红（防回升）」。
- #527（60ba3b28）向该 baseline 补登 5 行（vertexpartitionvalidation.cpp、geologicalexport.{h,cpp}、geologicalexportdialog.{h,cpp}），注释自述「待收进 src/qgis 封装后删行」——判红后补登基线转绿，而非收敛代码。
- 区间末端复跑 qgis_spread --strict 退出码 1（6 个新增散布文件），即该闸带着红合入后才被 #527 补登转绿。

## 影响

已册 148+ 个文件的既有 QGIS 散布被一次性永久豁免，闸门只剩「新文件」一个信号面；QGIS 迁移成本可以继续在封装层外无声积累，与 PALEO_QGIS_PLAN §2 相悖。

## 建议

- 把 qgs-audit / qgis-spread 口径从「文件计数」改为「新增 include/符号增量」计数（或至少对已册文件的增量另立黄牌）。
- 为这两个新闸补收敛计划（按目录/批次把基线条目缩到零），或在 AGENTS.md 明写「qgis-spread/qgs-audit 采用初冻基线、与 layering-baseline 归零纪律不同」的裁决，避免两条基线语义相互矛盾。

## 验证

在任一已册文件新增一个 qgs* include，`check_layering.py --qgs-audit --strict` 或 `check_qgis_spread.py --strict` 出现信号（增量计数生效）；baseline 行数在后续两个迭代内下降。

## 关联

- 同族：#474、#501、#502；AGENTS.md layering-baseline 归零纪律；docs/PALEO_QGIS_PLAN.md §2；配套 issue「#523 导出链仍直连外部 qgs* 头」。
