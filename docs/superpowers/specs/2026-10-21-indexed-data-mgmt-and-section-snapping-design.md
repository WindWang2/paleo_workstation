# 设计：索引式数据管理与连井剖面画线吸附

- 日期：2026-10-21
- 状态：已获用户确认（设计评审通过），待写实现计划
- 需求来源：用户反馈第 7、8、9 条（导入索引与对象命名 / 连井剖面地图绘制 / 删除数据对象索引）

## 1. 需求原文与决策记录

**第 7 条**：导入系统后的数据应该以索引的方式做数据文件关联；导入系统内的数据对象层应当支持用户自定义该数据对象的名称。

**第 8 条**：连井剖面的地图绘制剖面功能，激活后可以在左边画布中做任意剖面的绘制，支持自动吸附到井位点上，支持用户根据井之间的空间位置关系选择判断连井的方式，可快速形成过井剖面。

**第 9 条**：数据管理建议增加删除数据对象索引的功能，只删除数据对象，不删除本地实际的数据文件。

经逐条澄清，四项产品决策（用户已拍板）：

| # | 决策点 | 结论 |
|---|--------|------|
| D1 | 导入存储默认行为 | **默认只建索引**（外链，`managed=false` + 绝对路径 + SHA-256），「复制进工程」降级为导入选项。SEG-Y 原有外链行为不变。 |
| D2 | 画线吸附后的井序 | **吸附顺序即井序**；另给手动勾选的井列表加「按空间关系重排」辅助（最近邻链 / 主轴投影）。 |
| D3 | 本次适用范围 | **仅限有活动地震体的井震剖面**。无地震体的纯测井连井仍由既有 WellSectionPanel 承担，不重复造。 |
| D4 | 删索引时的文件口径 | **文件一律不动**：外链源文件不动，受管副本也保留为孤立文件，由存储治理面板发现/回收。 |

## 2. 现状摘要（调查结论，含关键位置）

### 2.1 导入与命名（第 7 条）

- 数据层已具备完整索引式能力：`CatalogVersion` 有 `managed` / `path`（外链为绝对路径）/ `sourceUri` / `sha256` 字段（`src/catalog/datacatalog.h:76-92`）；外链读取分流在 `src/io/dataimportservice.cpp:203-215`；源文件重定位机制已实现（`src/io/dataimportservice.h:224-233`）。
- 但 UI 导入默认**物理复制**到 `artifacts/raw/<assetId>/<versionId>/<文件名>`（`src/io/dataimport_file.cpp:354-372`，复制实现 `src/io/dataimport_session.cpp:88-183`）。只有 `options.linkExternal || cls.type == "seismic"` 时走外链（`dataimport_file.cpp:345-353`）。这是 `docs/PROJECT_AREA_PLAN.md:57-77` 写明的既定设计，本次按 D1 改口径。
- `linkExternal` 后端默认 `false`（`src/io/dataimportservice.h:36-42`），普通 UI 路径均未设置：单文件向导 `src/ui/paleomainwindow_attach_data.cpp:346-438` → `src/workflow/folderimport.cpp:552-593`；批量 `src/ui/paleomainwindow_project.cpp:58-175` → `src/io/dataimport_folder.cpp:608-613`；`IngestPlan` 无存储策略字段（`src/io/ingestplan.h:31-47`）。
- 命名：资产有 `displayName` 字段（`datacatalog.h:68-74`）且与版本物理文件名 `CatalogVersion::fileName` 分离，但**无更新 API、无 UI 入口**；右键菜单矩阵只给实体 `renameEntity`（`src/ui/pages/dataopspanelops.h:452-455`、`src/ui/pages/datalist_batch.cpp:524-549`），F2 只处理实体（`src/ui/pages/datalist.cpp:931-936`）。实体重命名走 sidecar（`.paleo/entity_overrides.json`），与资产命名无关。

### 2.2 连井剖面（第 8 条）

- 对话框：`src/ui/seismicsection/sectionsetupdialog.cpp:19-162`（勾选 + 拖动排序 + 上移/下移，`按顺序生成连井剖面` 收集已勾选井 id）。
- 「地图绘制任意折线」**已实现**：`SeismicSectionTool`（`src/qgis/seismicsectiontool.{h,cpp}`）左键加点、移动虚线预览、右键完成、Backspace 撤点、Esc 取消；编排链路 `src/ui/paleomainwindow_sections.cpp:208-256` → `src/linkage/seismicmaplink.cpp:127-174`（节点取整到 IL/XL）→ 异步提取。
- **吸附未实现**：工具直接读 `e->mapPoint()`（`seismicsectiontool.cpp:45-51,72-79`），未调吸附 API。主画布虽有全局 snapping 配置（`src/qgis/qgiscanvascontroller.cpp:49-58`），但该工具未使用。
- 井坐标来源：`SectionWorkbench` → `ProjectDataFacade::wells()` → catalog 井实体（`surfaceX/surfaceY/coordinateStatus`，`src/services/projectdata.cpp:85-103`）；画布井位图层 `wells`（GeoJSON，同源不同读取路径）。
- 空间排序可复用算法：PCA 主轴投影排序 `src/domain/wellsection.cpp:474-538`（数据层，纯算法无 Qt 依赖问题）。
- 现状限制：画线依赖活动地震体（按 D3 保持）；成功后对话框不重新显示（`paleomainwindow_sections.cpp:140-163`）。

### 2.3 删除索引（第 9 条）

- 数据层能力**现成**：`DataCatalog::removeAsset/removeAssets` 只删索引（资产行 + 版本行 + 链接行，单事务），头注释明确「磁盘字节不在此职责内」（`src/catalog/datacatalog.h:268-275`、`src/catalog/datacatalog_assets.cpp:81-161`）；血缘保护（被派生引用或 `retained_by_map_results` 非空拒绝删除）与 staging 保护在内。
- UI 只有两端：软删「删除…」（`BatchRemoveCmd` 写 `.paleo/recycle_bin.json`，索引文件全保留，`src/ui/pages/datalist_batch.cpp:141-178`）和「物理删除选中…」（`StorageGovernanceController::purgeAssets` → `paleo::assetops::purgeAssets`，索引 + 受管文件双删，`src/workflow/assetops.cpp:317`）。中间档「删索引保文件」缺 UI 入口与 workflow 封装。
- 先例：编图工作台对未登记图件的 declaration_only 删除（`src/ui/paleomainwindow_workbench_commands.cpp:366-368`）。
- 孤儿治理基础：回收站对话框已有按资产统计受管字节能力（`src/ui/pages/datalist_undo.cpp:376-395`），`StorageGovernanceController` 有体检/扫描面，但无「孤立受管文件」专项。

## 3. 设计

### 3.1 功能一（第 7 条）：索引式导入 + 数据对象命名

**技术路线（已定）**：默认翻转做在 **workflow/UI 编排层**，后端 `ImportOptions::linkExternal=false` 默认值不动（API 语义与脚本调用方稳定，`src/app/mkproject.cpp` 等不受影响）。

**A. 导入默认只建索引**

1. workflow 层新增统一入口（如 `uiDefaultImportOptions()`，放 `src/workflow/folderimport` 或新小文件），返回 `linkExternal=true` 的 `ImportOptions`；所有 UI 发起的导入路径经它构造：
   - 单文件向导链：`paleomainwindow_attach_data.cpp:346-438` → `folderimport.cpp:552-593`；
   - 文件夹/批量链：`paleomainwindow_project.cpp:58-175` → `dataimport_folder.cpp:608-613`；
   - 拖入链：`src/ui/pages/datalist_dnd.cpp:79-139`（`externalImportRequested` 的接收方）。
2. 单文件导入向导新增勾选「将文件复制进工程目录（受管副本）」，默认不勾；勾选后该次导入 `linkExternal=false`。
3. 批量确认表加全局「存储方式」切换（默认「只建索引」），`IngestPlan`（`src/io/ingestplan.h:31-47`）增加存储策略字段贯通批量执行。
4. 导入登记逻辑无需大改：`dataimport_file.cpp:345-353` 外链分支直接复用；外链读取、SHA-256 校验、源文件重定位链均已存在。
5. 文档同步：`docs/PROJECT_AREA_PLAN.md:57-77` 默认口径改为「默认索引外链，显式选择受管复制」；`docs/dataops/IMPORT.md` 补充存储方式说明。
6. 存量工程与已入库资产零影响（只改新导入的默认）。

**B. 数据对象自定义命名**

1. 数据层新增 `DataCatalog::renameAsset(assetId, newName)`（`src/catalog/datacatalog_assets.cpp`）：事务更新 `assets.displayName`；trim 后为空则拒绝；允许重名（与实体命名口径一致，树内靠类型/归属区分）。
2. UI 入口：
   - 数据树右键菜单新增「重命名数据对象…」（单选资产时出现，动作矩阵 `dataopspanelops.h:419-451` 扩展）；
   - F2 对资产选择生效（`datalist.cpp:931-936` 扩展）；
   - 对话框预填当前 `displayName`。
3. 撤销：新增 `RenameAssetCmd` 走既有 dataops 命令栈（`src/ui/pages/dataops/dataopscommands.h` 模式），redo/undo 调 `renameAsset`。
4. 导入时命名：单文件向导确认步与批量确认表允许编辑显示名（默认原文件名）；导入登记时 `asset.displayName` 取用户值而非 `fi.fileName()`（`dataimport_file.cpp:309-334`）。
5. 视图口径：数据树/主表/预览标签本就以 `asset.displayName` 渲染，无需改；井附件面板文件列继续显示物理 `fileName`（`src/ui/pages/wellattachmentpanel.cpp:285-290`），不受影响。

**测试**：`renameAsset` 单元测试（含空名拒绝、重开工程持久化）；菜单矩阵测试更新（资产出现重命名项）；导入默认索引的 UI 层测试（UI 入口构造的 options `linkExternal=true`）；`tests/tst_import.cpp` 中钉住旧 UI 默认的用例按新口径调整（后端默认不变，受影响面应限于 UI 编排层用例）。

### 3.2 功能二（第 8 条）：画线吸附 + 吸附即井序 + 空间重排

**技术路线（已定）**：**自定义井点最近邻吸附**，不用 QGIS `snappingUtils`。理由：吸附必须带回「吸到哪口井」的身份以收集井序；井坐标在 `SectionWorkbench` 现成（catalog 同源），不依赖全局吸附配置，行为确定。备选（snapToMap + 回匹配井层要素）多绕一层且受全局配置影响，不采用。

**A. 井位吸附（`SeismicSectionTool` 扩展）**

1. 工具构造时接收有效坐标井集合（id / 名称 / XY，来自 `SectionWorkbench`，与对话框井列表同口径：`coordinateStatus == ok/untransformed` 且坐标有限）。
2. `canvasMoveEvent`：容差取画布 QGIS 吸附设置的像素容差，读取不到时回退 12 像素；换算成地图单位后找最近井；命中则：
   - 橡皮筋预览终点吸到井口坐标；
   - 井口处显示吸附标记（`QgsRubberBand` 点符号或 `QgsVertexMarker`）+ 井名提示。
3. `canvasPressEvent` 左键：若当前处于吸附状态，顶点取**井口精确坐标**，并把 well id 追加进有序井序列表（连续同井去重）；未吸附则为自由顶点——任意线绘制能力完整保留。
4. Backspace 撤点：若撤销的是吸附顶点，同步从井序弹出对应 well id。
5. Esc 取消行为不变。

**B. 吸附顺序即井序，完成即生成**

1. 右键完成时，工具信号由「仅折线」扩展为「折线 + 有序 well id 列表」（`seismicmaplink.cpp:253-267` 链路相应扩展）。
2. 主窗口（`paleomainwindow_sections.cpp:178-256`）：吸附井数 ≥ 2 → 以该井序生成过井剖面（走现有 IL/XL 取整 + 异步提取链），并将设置对话框井列表同步为该顺序与勾选状态（`SectionSetupDialog` 需提供同步方法）；< 2 → 维持现有纯任意线提取行为。
3. 生成成功后重新显示设置对话框（改现状），便于继续做时深对齐与保存版本。

**C. 空间重排辅助（对话框）**

1. `SectionSetupDialog` 新增「按空间关系重排」按钮（对当前勾选井生效，少于 2 口勾选时禁用），下拉两种算法：
   - **最近邻链**：从最西端井（surfaceX 最小；并列时取 surfaceY 最小）起贪心最近邻（算法放 `src/domain` 或复用 `wellsection.cpp` 风格，纯函数可测）；
   - **主轴投影排序**：直接复用 `src/domain/wellsection.cpp:474-538` 的 PCA 投影排序。
2. 重排只改列表顺序，不改勾选集合，排序后可继续手动拖动微调。

**边界（按 D3）**：无活动地震体时「地图绘制任意折线」按钮行为不变（提示先导入带有效坐标的地震体）；纯测井连井剖面仍由既有 WellSectionPanel 承担，本次不打通。

**测试**：吸附最近邻/容差/连续同井去重的单元测试（井序收集逻辑抽成可测纯函数）；井序同步对话框的 UI 测试；空间重排两种算法的单元测试；既有任意线无吸附行为回归。

### 3.3 功能三（第 9 条）：删除数据对象索引（文件一律不动）

1. **数据层零改动**：直接复用 `DataCatalog::removeAssets`（血缘保护、staging 保护、单事务现成）。
2. workflow 新增封装 `removeAssetsKeepFiles(assetIds)`（`src/workflow/assetops.{h,cpp}`）：只调 `removeAssets`，返回结果；**不做任何文件操作**（区别于 `purgeAssets` = removeAssets + 文件清理）。
3. UI：数据树右键新增「从目录移除（保留文件）…」（资产单选/多选均可，动作矩阵扩展），确认弹窗明示三点：
   - catalog 索引记录（含实体挂接关系）将被删除，**不可撤销**（如需可恢复请用软删「删除…」）；
   - **所有本地文件一律不动**（外链源文件与工程内受管副本均保留）；
   - 受管副本将成为孤立文件，可在存储治理中查看与回收。
4. 链接行随资产删除（`removeAssets` 现状语义），在确认文案中说明。
5. 孤儿治理落地（D4 的「留待治理」）：存储治理面板新增「孤立受管文件」扫描项——遍历 `artifacts/raw/`（及其他 stage 目录）下无 catalog 资产对应的目录，列出路径与占用字节，支持「删除选中」与「在文件管理器中显示」。最小实现，不做自动回收。
6. 分层：catalog API（数据层）/ `assetops` 封装（功能层）/ 菜单与确认框（视图层）各归其层。

**测试**：`tests/tst_assetops.cpp` 新增 keep-files 用例（索引删除成功；受管副本文件仍在；外链源文件不动；血缘保护拒绝时文件与索引都不动）；右键菜单矩阵测试更新；孤儿扫描的识别正确性测试（种一个无对应资产的目录能被列出，有对应资产的不列出）。

## 4. 横切约束

- 分层纪律：每文件头三行 `// 层：...` 标注；`src/domain`/`src/catalog`/`src/io`/`src/workflow` 禁 QtWidgets 与 `ui/` include；新顶层模块先 `scripts/new_module.sh` 登记（本次预计不需要新顶层模块）。
- 闸门：`file_budget` / `ui_size` 函数体规模闸——`SeismicSectionTool` 与 `SectionSetupDialog` 扩展时注意拆分，超预算即拆文件而非补登基线；`qgis_spread` / `qgs-audit` 初冻基线只缩不涨（`SeismicSectionTool` 在 `src/qgis` 属豁免层，新增 QGIS 符号注意不扩散到其他目录）。
- 测试布线：新测试一律 `add_paleo_test(name [LIBS ...])`，`-j8` 以内。
- i18n：界面文案按现有 `tr()` 模式。
- 文档：`docs/PROJECT_AREA_PLAN.md`、`docs/dataops/IMPORT.md`、`docs/dataops/OPERATIONS.md`（操作矩阵加新菜单项）、`docs/SECTIONS_ALIGNMENT_PLAN.md`（吸附与井序行为）同步更新。

## 5. 非目标（YAGNI）

- 无地震体时画线生成纯测井连井剖面（D3 明确排除，WellSectionPanel 已承担）。
- 孤立受管文件的自动回收/定期清理（只做扫描 + 手动删除）。
- QGIS `snappingUtils` 全局吸附接入剖面工具（自定义井点吸附已满足需求）。
- 实体（井）重命名机制改造（sidecar 现状不动，本次只做数据资产命名）。
- 删除索引的撤销支持（不可撤销 + 确认文案明示，与物理删除同档；可恢复诉求由软删承担）。

## 6. 验收标准

1. 导入：UI 导入 LAS/DAT/图片默认不在工程目录产生副本，catalog 中 `managed=false`、`path` 为原文件绝对路径、SHA-256 有值；勾选「复制进工程」后行为同旧默认；SEG-Y 行为不变。
2. 命名：数据树中可对任意数据对象重命名（右键/F2），重开工程后名称保持；可撤销/重做；导入时可改显示名；物理文件名不受影响。
3. 画线：激活「地图绘制任意折线」后，鼠标靠近井位在容差内吸附到井口并有可视反馈；可混用吸附顶点与自由顶点；Backspace/Esc 行为正确。
4. 井序：吸附 ≥ 2 口井右键完成后直接生成过井剖面，对话框井列表同步为吸附顺序；「按空间关系重排」两种算法结果正确且只影响顺序。
5. 删除索引：「从目录移除（保留文件）…」执行后 catalog 中资产/版本/链接记录删除，磁盘上受管副本与外链源文件均在；存储治理能扫出孤立副本并可手动删除。
6. 全部既有测试保持绿（口径调整用例除外），`layering` / `file_budget` 等源码闸门不新增违规。
