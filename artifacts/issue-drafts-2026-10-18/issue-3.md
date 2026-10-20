# [med] ci.yml --transitive 带 QGIS_PREFIX 必红：本批 faciespartitiontool.h 新增 2 条视图层 QtWidgets 边，本地 ctest 假绿

## 问题

本批把 `check_layering.py --transitive` 接进 ci.yml lint 车道，而 CI 实际配置（Source gates 步注入 QGIS_PREFIX）下该闸必红：ui_qtwidgets_notes 实测 8 > baseline 6，超出的 2 条边全部来自本批新增的 src/qgis/faciespartitiontool.h；本地 ctest 项因不传 QGIS_PREFIX 只闸 heavy_* 指标而假绿。当前 HEAD（a0cd3a88）复跑仍红。

## 证据

- 当前 HEAD 复跑（模拟 CI 环境）：`QGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix python tools/check_layering.py --transitive` → 退出码 1，末行 `FAIL ui_qtwidgets_notes: 8 > baseline 6——传递债只缩不涨`；小计行「视图层 QtWidgets 边记录 8 处；重头出闸黄牌 52 条出闸边（41 个重头 × 87 个受染 TU）」。
- `.github/workflows/ci.yml:113` 给 Source gates 注入 `QGIS_PREFIX: ${{ github.workspace }}/vendor/prefix/usr`，:121 跑 `--transitive`；CMakeLists.txt:984-988 注释明写「本测试不显式传参……CI lint 车道显式给 QGIS_PREFIX 全闸」——本地/CI 口径差异是已知设计。
- 2 条新边：`ui/edittools/editingtoolbar.cpp:43 → qgis/faciespartitiontool.h -> … -> <QGestureEvent>` 与 `ui/edittools/editingtoolbar_partition.cpp:4 → …`；faciespartitiontool.h 由本批 2a007073（#521）新增（`git show 366aef97:src/ui/edittools/editingtoolbar.cpp | grep faciespartitiontool` 无命中）。
- 同一命令在无 QGIS_PREFIX 的环境退出码 0（ctest 项 layering_transitive 因此假绿）。

## 影响

required lint 车道在 CI 上每个 run 都红；本地/CI 口径不一致还会让「本地绿」给出虚假安全感，与 #497 / #500 同族。

## 建议

- 让编辑工具栏不再直接 include faciespartitiontool.h（改经 src/qgis 门面转发 QtWidgets 边），把 ui_qtwidgets_notes 降回 6。
- 给 layering_transitive ctest 项注入 QGIS_PREFIX（或文档化「本地不闸该指标」），使本地与 CI 同口径。

## 验证

带 QGIS_PREFIX 复跑 `python tools/check_layering.py --transitive` 退出码 0；`ctest --test-dir build -R layering_transitive` 在注入了 QGIS_PREFIX 的环境同样 Passed；CI lint job 连续两次 success。

## 关联

- 同族：#474（transitive 防增长闸）、#497、#500；tools/transitive-baseline.txt 注释与数值漂移见同批低危清单。
