# [low] 本批中低危清单（13 条）：功能层 pump 事件队列/WKB 进版本契约/tr() 缺失/预览无防抖/字阶与画布色值/triage 红/baseline 注释/appimage 钉版/暂存与 CSV 健壮性

## 问题

本批 13 条已验证低危发现，按域分四组（均不单独立 issue，汇总跟踪）：

**架构/生命周期**

1. 功能层同步 pump Qt 事件队列：`src/workflow/mappingworkbench_edit.cpp:87-89` `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`（调用点含 :520 起 removeProduct 内多处），事件派发职责渗漏进功能层，且依赖一条无类型保障的跨层生命周期契约。
2. GEOS/WKB 序列化几何（base64）钉进 catalog 版本 extra：`src/workflow/mappingworkbench_edit.cpp:215` `{"partition_footprint_wkb", partitionFootprint}`，数据层契约耦合 QGIS/GEOS 序列化格式，与「QGIS 管 GIS 管线、Paleo 管地质语义」相反。

**UI/设计系统**

3. 新 UI 未走 tr()：`src/ui/dialogs/dataexportdialog.cpp:32` 与 `src/ui/paleomainwindow_attach_compose.cpp:405-408` 的格式名裸字面量。
4. 地质图导出对话框预览无防抖：`src/ui/dialogs/geologicalexportdialog.cpp:85-89` 共 13 条连接全部直连 rebuild()（同轮 dataexportdialog.cpp:53-55 反而有 200ms 防抖）。
5. 次级状态标签未按 label 字阶设字体：`src/ui/pages/mappingworkbenchpage.cpp:378-380` 只设 objectName/wordWrap（对照同文件 :100 走 kLabelPt）。
6. wellcomposite 画布钉注旗硬编码色值：`src/ui/wellcomposite/compositebody.cpp:158` `QColor(27, 115, 208, 215)` 即交互主色 #1B73D0 作装饰底，违反 DESIGN.md「禁止用于装饰性背景」。

**构建/CI**

7. triage job 从第一次运行起就会红：`.github/workflows/ci.yml:470-486` `needs: [lint, …]`，lint 的 Source gates 步本批新加的 qgis_spread_strict 已由 #527 补登基线转绿，但 `--transitive`（带 QGIS_PREFIX）仍红（见同批 [med]）；`docs/ci/known-reds.json` 仅 3 条 test-scope 条目、无 lint 的 job-scope 登记，分诊器按「gating job 级红不可分诊」exit 1。
8. `tools/transitive-baseline.txt` 注释与自身数值矛盾：:20 注释称「heavy_tus 83→86（出闸边 52 条不变）」而 :26 写 87，且把 ui_qtwidgets_notes 涨幅归因 #516，实际已被本批 faciespartitiontool 链推到 8（当前 HEAD 带 QGIS_PREFIX 复跑小计仍为「8 处 / 87 TU」）。
9. `tools/make_appimage.sh:36-44` 的 runtime sha256 钉版只在「文件不存在或不可执行」时校验；build/appimage/runtime-x86_64 已存在且带 x 位时复用路径永不与新 pin 对账。

**IO 健壮性**

10. `src/services/dataexport.cpp:246` `QTemporaryDir input(staging.filePath(".input-XXXXXX"));` 不校验 isValid()（外层 staging 在 :229 校验）。
11. `src/services/jsonsidecar.h:69` `f.write( doc.toJson( format ) );` 丢弃返回值，与同批 dataexport.cpp:310 / geologicalexport.cpp:195 / horizonbatchexport.cpp:440 的字节数比对不一致。
12. `src/services/dataexport.cpp:267-269` CSV 快路径用 safeName 取名却用原名去暂存目录取源（:254-255 落盘名是 QFileInfo(part).fileName()），段名不合法时必然假失败。
13. 4 个隐藏暂存根无清扫器：`src/workflow/geologicalexport.cpp:57-58`（.map-products-XXXXXX / .paleo-map-XXXXXX）、`src/workflow/horizonbatchexport.cpp:368/374`（.horizon-export-XXXXXX / .paleo-batch-XXXXXX）、`src/services/dataexport.cpp:228`（.paleo-export-XXXXXX），清理全押 QTemporaryDir 析构。

## 证据

见「问题」各条 path:line——逐条读过上述代码行；`python tools/check_i18n.py` 复跑退出码 0（第 3 条盲区）；`QGIS_PREFIX=… python tools/check_layering.py --transitive` 复跑退出码 1 且小计「ui_qtwidgets_notes 8 / heavy 87」（第 8 条口径）；`grep -rn` 确认 5 处隐藏暂存根字面量且无清扫代码（第 13 条）。第 9 条 pin 值正确性由复核方下载实测（944632 字节，sha256 = 2fca8b44…260d），未复跑下载。

## 影响

单条均为低危（局部不一致/窗口期失败/复用车况），合计构成「设计系统与闸门口径的持续漏损」；第 7 条会连带让 triage 这个新 context 恒红，第 10/12 条会让导出批次部分成功、半成品交付。

## 建议

按域分批：架构两条收敛为数据层/壳侧显式辅助；UI 四条与 DESIGN.md 口径对齐并扩展 check_i18n.py 扫描面（addItems/addItem）；CI 三条随红闸修复后一并对账；IO 四条补齐 isValid/返回值/键一致性与超龄清扫。

## 验证

`python tools/check_i18n.py` 扩展后对 addItems/addItem 命中裸字面量即红；`grep -rn "sendPostedEvents" src/workflow` 零命中；make_appimage.sh 复用路径下改 pin 值能触发校验失败；`grep -rn "\.input-XXXXXX\|\.paleo-.*-XXXXXX" src/` 每处均有 isValid 或清扫对账。

## 关联

- 红闸修复见同批 [med] 条目（qgs-audit / transitive / file_budget / 导出链封装）；#497、#500、#502、#503。
