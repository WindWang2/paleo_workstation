# P5 井点数值表达与地图册交付闭环（2026-11-08）

任务：方向三「编图表达与成果交付」P5——把既有「井点因子结果 → 主图/结果
预览 → 地图册 → 用户可检查的导出文件」流程补齐为可核验交付。
分支：`codex/paleo-p5-mapbook-delivery-20261108`（base = origin/master 86454671）。

## 已实现（按工作包）

### W1 井点实际值表达契约
- 新增 `src/qgis/samplepointstyle.{h,cpp}`：
  - `formatFactorValue`：两位四舍五入（half away from zero）、整数去零、
    负零统一、NULL/非有限 →「无数据」、≥1e15 科学计数保真、locale 无关；
    比值不乘 100。
  - `formatWellIdentity`：well_name → well_id →「无数据」。
  - 自定义表达式函数 `paleo_format_sample_value`（与 C++ 同一实现——屏幕
    与导出文本同源可断言）。
  - `isWellFactorPoints`：语义身份核对（点 + factor_value + well_id/well_name
    之一），字段不齐不套用。
  - `applyWellFactorLabels`：井位点标记 + 双行标注（QGIS 标签引擎避让），
    幂等（= 恢复默认样点表达入口），渲染只读不改源 feature。

### W2 主图、预览、历史成果和导出共用
- `qgislayerservice.cpp` instantiate()：samples./wellfactor. 声明 + 语义核对
  通过时套样点表达；声明带显式 styleRef 时让位既有样式链。
- `mappingworkbench.cpp` styleLayer()：kind=well_factor_points（含
  product.<versionId> 历史成果、工程重开）走样点表达薄分支。
- `datapreviewtabmapproduct.cpp`：well_factor_points 专用分支，不再落
  name/NAME/id/fid/首字段通用回退。

### W3 冻结当次导出说明
- Request 携带 `FrozenInputs`（owner 线程在 layers/catalog 清空前捕获；
  同步 run() 由 prepareRun 同规则补齐）：图层序（qgisId/paleoId/name/type/
  crs/assetId/versionId/sha256/managed/source）、工程 CRS、QGIS 运行库版本。
- manifest.json 新增 `inputs` 段（layers/horizon/analysis/layout/runtime），
  未受管来源显式 `managed:false` + source，不伪造版本。

### W4 逐页与整册状态一致
- `PageRecord`（status ok/failed/cancelled + file/bytes/sha256/reason）与
  tiles 同序同量；manifest/Result/UI 摘要同源，计数守恒。
- 预演限额不引入第三状态：未出版 = cancelled（reason 区分「预演限额」与
  「用户取消」），旧字段口径不变。
- 装配段：`index.status` ok/failed/skipped 单列（没请求目录 = skipped）；
  登记失败路径同步降级页账本（不只清文件列表）。

### W5 完整目录分页和所有兄弟文件回传
- `buildIndexLayout` 按 A4 纸面/8pt 字号/4.6mm 行距/长名占行分页（多页
  版面，每页「第 i/N 页」），全部条目入册次序不变，不再 60 行截断；
  `indexPageCount` 为分页纯计算面（测试同源）。
- `RenderOutcome` 增 `files`/`totalBytes`：多页 PNG 的 _2.. 兄弟页全部
  回传并逐文件回读校验（头校验/可解码/尺寸），任一失败整次失败。

### W6 地图册局部 QGIS 边界收敛
- 新增 `src/qgis/mapbookrender.{h,cpp}` facade：renderTile/renderIndex
  （建版 → 渲染 → 校验 → 销毁不出封装层）+ layerStableId/
  projectLayerById/summarizeLayer/projectCrsAuthid。
- `mapbookqueue.cpp` 移除全部 qgs*.h include（QgsPrintLayout/QgsRectangle
  /QgsProject/QgsMapLayer 不再进功能层）；失败阶段 layout/export 语义保留。
- `tools/qgs-audit-baseline.txt` 删 `src/workflow/mapbookqueue.cpp` 行
  （在册条目 136→135，只缩不涨；此前记的 163/162 为文件总行数口径，订正）。

### W7 原生面板交付反馈
- mapbookpanel：交付摘要行（deliverySummary：进度 + 装配状态 + 警告数）+
  已产出文件入口 QListWidget（键盘 ↑↓/Enter；窄窗横向滚动 + tooltip 全路径；
  激活发 artifactActivated）。
- mapbookcontroller：打开产物前现校验存在性（导出后被移动/删除如实报错）；
  warning 逐条入日志；工程重置清交付区（旧工程摘要不残留给新工程）。

### W8 测试与图件检查
- 新增 `tests/tst_samplepoint_presentation.cpp`（数值/身份/身份核对/
  表达式同源/只读性/真实 PNG 导出）与 `tests/tst_mapbook_delivery.cpp`
  （逐页状态守恒/取消语义/页失败/冻结输入/125 条分页/兄弟文件损坏检测/
  长中文分页 + PDF 页数核验/索引失败显式化）。
- 构建接线：`cmake/extra-p5-mapbook-delivery.cmake` + 根清单一行登记。

## 验证证据

- 构建：`cmake --build build --target tst_samplepoint_presentation tst_mapbook_delivery tst_mapbook tst_layoutexport tst_layerproperties paleo -j4` 全部通过（RelWithDebInfo，vendored QGIS prefix）。
  改动前基线：同两测试目标在 origin/master@86454671 干净态构建通过（exit 0）。
- 新测试（确定性断言，非截图）：
  - `tst_samplepoint_presentation` 9/9：数值矩阵（0.23→0.23、12.00→12、12.126→12.13、
    NULL/NaN/±Inf→无数据、-0.001→0、-12.126→-12.13、0.005→0.01、1e-9→0、
    2e15→2e+15、德语 locale 不变）；身份回退；语义身份核对（缺 factor_value
    /非点层拒绝）；**表达式同源**（QgsExpression 逐 feature 求值与 C++ 契约
    一致，含 w2 只剩 well_id、w4/w5 NULL 值→无数据）；只读性（重读 feature
    数值不变）+ 幂等；真实 A4 PNG 导出（1122×793 = 297×210mm@96dpi 精确）。
  - `tst_mapbook_delivery` 11/11：预演 3/9 → 3 ok + 6 cancelled（manifest 页
    reason=预演、sha256 与落盘文件重算一致）；开跑前取消 → 9 cancelled 不谎报；
    退化范围页失败 → 8 成 1 败 reason 入账、其余保留；冻结输入（受管层
    asset/version/sha 与 catalog 事实一致、未受管层 managed=false+source、
    projectCrs/horizon/layout/runtime 入册）；125 条目录 ≥3 页全部 PNG 头
    校验过、manifest index.files 列全；截断/删除 _2 兄弟文件 →
    validateExportedFile 拒绝；61 条长中文 → 2 页（版面页数=纯计算，
    QPdfDocument 页数=2 一致）；索引路径被占 → index.status=failed、页账本
    不受装配失败影响、deliverySummary 呈「目录失败」。
- 回归：`tst_mapbook`（既有快照/取消/装配失败/异步登记 12.4s 全量）✓、
  `tst_layoutexport` ✓、`tst_layerproperties`（instantiate 改动面）✓。
- Source gates：`ctest -R "layering|qgis_spread|file_budget|ui_size|
  ui_invariants|i18n|ui_tokens|docs_drift|cpp_mainline|qgis_versions"` 29/29
  绿（--strict 口径）；基数按实际条目数订正见下（mapbookqueue.cpp 删行）；
  file_budget 冻结值 372→355（真拆分后下调，无新增超标）。
- 图件检查记录（渲染级）：
  - 井点标注真实渲染像素证据：4 井位（按版面几何推导 x=201/365/529/693）
    各自 ±(25,120)px 窗口墨迹 259/247/254/254 深色像素——标记+双行标注
    均衡落墨，非空白地图。
  - 61 条长中文目录 PDF：2 页，页数与分页纯计算和版面页数三方一致。
  - pdftotext 对导出 PDF 无文本（textAsPaths 既有口径，预期内）；数值语义
    由表达式同源断言代偿（见上）。
  - **视觉关口如实记录**：本机模型无图像输入，visual-judge 子代理亦无法
    目检（三页 Unverified）；视觉验收需有图像输入能力的审查者复核
    /tmp 样张（已随测试生成，可复现）。

## 限制与未做

- `mapbookqueue.h` 仍前向声明 QgsMapLayer/QgsProject（Request.layers 的
  既有公开口径未改）——qgs-spread 基线 mapbookqueue 两行保持冻结，
  未在本任务收敛（属 API 形状重构，超出「仅收敛调用链」范围）。
- 预演限额未单列 skipped 状态（语义 = cancelled + reason，见 W4）。
- PDF 数值标签文本不做字节级抽取断言（textAsPaths 口径下无文本层；
  以表达式同源断言 + 文件有效性代偿，见测试注释）。

## 与并行任务/开放 PR 的关系

- #912（P2）：共享 `CMakeLists.txt` manifest 行与
  `tools/file-budget-baseline.txt`（各自一行/条目，无业务交集）。
- #906/#910/#907：未触碰其占用区（I/O、发布恢复、算法、依赖钉版）。
- 同批 P1/P3：文件集无交集。
