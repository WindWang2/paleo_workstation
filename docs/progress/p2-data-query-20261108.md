# P2 data-query：统一筛选、列漏斗与批操作选区（2026-11-08）

任务：`P2` · 分支：`codex/paleo-p2-data-query-20261108` · 基线：origin/master
`86454671` · CMake 片段：`cmake/extra-p2-data-query.cmake` · 目标：在现有数据页
建立一致的「查询状态 → 完整逻辑结果 → 五种视图 → 可操作选区」。

## 交付面（八个工作包）

### W1 唯一查询状态与旧控件编辑规则

- 新增 `src/ui/pages/dataops/dataopsquery.{h,cpp}`（纯值，QtCore only）：
  `DataQuery = FilterGroup + activeTag + 列约束` 单一事实源。
  `DataListPanel` 的 `m_filter/m_activeTag` 成员废除，全改 `m_query`。
- 旧搜索框/类型框只是「首个正向条件」的编辑器：`setPositiveValue` 值空只删
  首个正向条件，否定条件/其他维度/同维度多值不动（原 `clearDim` 路径会删掉
  仅否定条件——已消除）。刷新时 `mirrorQueryIntoLegacyControls` 以
  `QSignalBlocker` 回写镜像（无反馈环、无回填）。`syncLegacyControlsIntoFilter`
  删除。
- chip 单删/取反改 `removeFirstMatching/toggleFirstMatching`（首个匹配）——
  一次点击不再翻转所有同值条件；`addCondition` 对完全重复条件规范化去重。
- 快捷开关 `setSwitch`：on 补正向（去重）、off 只删正向（否定同维保留）。

### W2 可复用的编译查询

- `CompiledQuery::compile` 一次准备（正则编译、比较值 toLower 规范化），
  `matches` 只消费 `AssetRowInfo`（零 catalog 回查/零磁盘）。正则编译计数
  `regexCompileCount` 随条件数变化（回归锁死）。无效正则沿用既有「退字面量」
  契约并在 `summaries()` 里如实标注。`applyListFilter` 单次编译、五种视图共用
  同一份逻辑结果（visibleIds/filteredRows 一次更新）。

### W3 列漏斗纳入逻辑查询

- 生产列头漏斗改走 `openColumnFunnel`（datalist.cpp 表头接线）：候选 =
  `DataQuery::columnCandidates`（应用其他条件、排除本列约束、并入零命中已选
  值），提交 = `setColumnConstraint`（同列 OR、跨列 AND、与组+标签相交）。
  不再扫当前 QTableWidget 页面 + `setRowHidden`（旧 `ColumnFunnel::execForColumn`
  删除；`applyColumnFilter` 静态保留供既有兼容测试直调）。
- 类型走 effectiveType；关联列按 `entityNames` 逐值匹配（回归证明拼接串
  「W1、W2」不命中）；显式空集合（零命中）与未设置严格区分；全选规范化为
  清除约束；取消弹层不提交。活动列约束以 `columnChip` chip 呈现、点击清除。

### W4 有诊断的状态串与预设

- v2 格式 `paleo://dataops-filter?v=2&op=…&f=<dim>~<p|n>~<转义值>&tag=…&col=…`：
  值与 negate 分开编码（转义仅 `%`/`~`），字面 `!`、中文、空格、`#`、`&`、
  `%`、`+`、空值、重复维度全部往返保真（回归逐条件对账 + 语义匹配对账，非
  仅字符串相等）。
- `parseQueryStateString` 返回 `QueryStateParse{valid, legacy, query, diagnostic}`：
  错 scheme/host、未知版本、未知字段、未知维度、载荷损坏、坏 op 均拒绝并给
  可读诊断；v2 未知字段不再静默丢弃。无 `v` 字段按 v1 既定语义读取（`!` 前缀
  歧义如实保留）。
- `setFilterFromStateString` 兼容面保留但只在有效时替换；
  `tryApplyQueryStateString` 为诊断入口（坏串不清查询/选区，状态条上报）。
- 预设沿用 `dataops/filterPresets` 同一存储（`saveQueryPreset/loadQueryPreset`
  v2 全量含列约束+标签；`saveFilterPreset/loadFilterPreset/deleteFilterPreset`
  旧 API 保留，旧入口读新预设取条件组部分）。

### W5 五视图、分页与可操作选区一致

- `m_visibleAssetIds` 由完整逻辑结果一次更新；树/表/图标/虚拟/分组同源。
  虚拟表未 fetch 行、当前页外行属于逻辑全集（回归：320 资产分页模式下全选
  = oracle 全集）。全选/反选以同一全集为底。
- `currentAssetSelection` = 记忆选区 ∩ 当前可见 ∩ 存活 ∩ 未回收；漏斗隐藏
  后批命令输入立刻排除（回归）；清除过滤后身份按 SelectionKeeper 契约恢复。
- 实体/统计节点不冒充资产（树选井节点 → 实体面非空、资产面空，回归）。
- 外部定位被过滤资产：状态条如实说明「被当前过滤条件隐藏——未修改过滤」，
  查询保持、隐藏身份不进批操作（回归）。

### W6 可理解的状态、词表与操作入口

- 空态文案带实际生效条件全集（组条件 + 标签 + 列约束 + 正则字面量回退说明，
  来自与匹配同一份编译产物）；计数标签/页标签/选中徽标口径一致。
- 旧类型下拉词表 = catalog 原型 ∪ 行快照 effectiveType（仅存在于覆盖值的
  类型可选，如回归中的 `custom-type-x`）；FilterBar 词表本就走 effectiveType。
- 快捷按钮勾选态随查询同步（`setActiveStates`，信号阻断无回环）；同维存在
  否定条件时 tooltip 明示补集语义。
- 查询/筛选/切视图零工程写入（见 W8 证据）；查询控件不随工程只读锁禁用
  （本任务未改既有可用性规则）。

### W7 独立 oracle 与跨页端到端回归

- `tests/tst_dataquery.cpp`（纯值，19 用例）：7 行夹具覆盖同名不同 ID/类型
  覆盖/空关联/多实体/标签/未决/保留字符文件名；期望集全部手写显式 ID 集，
  不用被测 matcher 反推。覆盖：仅否定、正负混合、同维多值、OR+列约束、chip
  单条编辑、旧控件镜像、编译计数、字面量回退、空集 vs 未限、候选规则、
  v2 往返、legacy 兼容、坏串诊断矩阵、预设往返。
- `tests/tst_dataquery_ui.cpp`（真实临时工程 320 资产跨页 + DataListPanel，
  13 用例）：漏斗约束边界化批输入与清除恢复、跨页候选（值只在其他页出现）、
  五视图/翻页/排序/刷新查询不丢 + 显示顺序按排序契约、虚拟表未 fetch 行
  全选、实体节点隔离、坏分享串保查询后恢复正常交互、隐藏定位提示、零写入
  （catalog revision + 工程目录哈希 + GUI 线程 stat interposer 三重对账）、
  窄窗空态入口、快捷/词表一致、预设全量往返。
- 既有 dataops/assetpaging 回归保留未动（新覆盖只在任务专属测试）。

### W8 性能边界、真实界面验证与交付

- 正则编译次数 ∝ 条件数：`tst_dataquery::regexCompileCountScalesWithConditionsNotRows`
  （3 条件 × 500 行扫描，编译计数恒 3）。
- 10k 规模证据（本机实测，`PALEO_CATALOG_SCALE=10000 ./tst_assetpaging`，
  2026-11-08，CachyOS x64 / Qt 6.12.0 / GCC 16.2.1，RelWithDebInfo）：
  `SCALE asset table 10000 rendered 4 pageSize 4 legacyStats 120000
  refreshStats 0 refresh/legacy 0.103015 filter/legacy 0.0224562
  locate/legacy 0.00925013 ms: fixture 1452 open 369 legacy 541 refresh 55
  filter 12 sort 22 locate 5 restoreRefresh 52`——GUI 线程零 artifact stat
  （legacy 对照 120000），过滤 12ms，14/14 用例过（含改型后过滤保持）。
- 查询操作序列零工程写入：`tst_dataquery_ui::queryOperationsWriteNothing`
  （revision 不变 + 目录哈希不变 + stat 计数 0）。
- 原生界面实测：本环境以 offscreen Qt 实测窗口部件行为（窄窗 240px、状态/
  清除入口在场、空态说明含列约束）；漏斗弹层为 QMenu 原生键盘导航（方向键/
  空格/Esc），`menu.exec` 模态路径无法在 offscreen 自动断言——提交面
  （candidates/checked/keep 全选规范化/取消不提交）已由代码路径共享并被
  端到端覆盖，真机键盘走查留手工清单（见「限制」）。
- 限制：Windows 车道未在本机执行（无 Windows 环境）；10k 万条 Windows 完整
  结果未取得，不报告提速倍数（只报绝对值与比值来源）；高 DPI 真机走查未做。

## 验收矩阵对照（任务 E 节）

| 场景 | 证据 |
|---|---|
| 仅否定 Search/Type，旧输入框为空 | tst_dataquery::negativeOnlyConditionsSurviveLegacyEdit；tst_dataquery_ui::negativeOnlySurvivesRefreshAndMirror（含刷新后保留 + 全选 240 oracle） |
| 正负混合、同维度多值、AND/OR | mixedPositiveNegativeOracle / orGroupSemantics / legacyEditKeepsSameDimMultiValues / chipEditTargetsSingleCondition |
| 一列多值与多列组合 | columnSameOrCrossAndWithGroup（OR∩AND）；columnEmptySetVersusUnrestricted（空集≠未限） |
| 300+ 资产，候选值只在其他页 | tst_dataquery_ui::funnelCandidatesSpanLogicalSet（320 资产，四类型全列出 + 零命中已选值可见） |
| 漏斗隐藏已选项 | funnelConstraintBoundsSelectionAndRestores（批输入排除 + chip 清除恢复） |
| 换页、排序、刷新及五视图切换 | viewsPagingSortKeepQueryAndSelection（含显示顺序契约） |
| 全选、反选、未 fetch 行、实体节点 | unfetchedRowsAndEntityNodes + negativeOnly…（反选） |
| 清除过滤、定位到被过滤资产 | locatingHiddenAssetExplainsAndKeepsQuery；funnel…（恢复契约） |
| 字面 !、中文、保留字符、重复条件 | v2RoundTripPreservesSpecials（逐条件+语义双对账） |
| 坏格式/错 scheme/未知版本字段 | badStringsReturnDiagnosticsNotSilentDrop（9 种）；UI 侧 badShareString…（查询/选区保持） |
| 无效正则、仅覆盖类型、空结果 | regexCompileCount…（字面量回退+说明）；quickButtonsAndVocabConsistent（custom-type-x 可选）；narrowWindowStatusEntriesPresent |
| 规模、文件访问、控件数量 | 10k 证据（上）；queryOperationsWriteNothing；tst_assetpaging 全过 |
| 只读工程、关闭重开或切工程 | queryOperationsWriteNothing（零写入）；跨工程选区清理沿用 refreshAssetTable ∩ live（未改约定） |
| 与 P1 并行及交付 | 未触碰 dataopscommands/undo/model 存储面与 datalist_undo/batch/indexops/dnd 写路径；公开接口（filterGroup/applyFilterGroup/setFilterFromStateString/currentAssetSelection/selectAssetInViews）签名不变 |

## 文件清单

新增：`src/ui/pages/dataops/dataopsquery.{h,cpp}`、`tests/tst_dataquery.cpp`、
`tests/tst_dataquery_ui.cpp`、`cmake/extra-p2-data-query.cmake`、本文档。
修改：`datalist.{h,cpp}`（接线/成员收敛，ctor 442→438、buildDataOpsUi 332→308
——file-budget 基线同步下调）、`datalist_filter.cpp`（编译查询/镜像/漏斗提交/
诊断入口）、`datalist_select.cpp`（隐藏定位提示）、`datalist_table.cpp`
（词表 ∪ effectiveType）、`datalist_undo.cpp`（命令改走 setSwitch/clearAll）、
`dataopsviews.h`（ColumnValuePicker；旧 execForColumn 移除、applyColumnFilter
保留兼容）、`dataopswidgets.h`（列约束 chip / 快捷态同步）、`CMakeLists.txt`
（manifest 登记一行）、`tools/file-budget-baseline.txt`（两行下调）。

## 独立复审与修复（第 8 轮）

独立 reviewer 对实际 head 做了静态审读 + 新测试/闸门实跑 + 自写探针
（新旧 matcher 3100 组合对比、状态串边界值往返）。8 项主审 5 项直接 PASS
（W1 编辑规则/反馈环、五视图一致、oracle 独立性、层合规与 P1 边界、资源
安全）；3 项提出问题，已全部修复并补回归：

1. **v2 载荷双重解码/col `~` 拆散**（值含 `%XX` 形子串、col 值含 `~` 往返
   损坏）→ 载荷编码改 base64url（[A-Za-z0-9_-]，QUrl 查询值零干预），
   字符集校验坏载荷拒收；`v2RoundTripPercentShapedAndTildeInColumnValues`
   `v2RejectsBadPolarityAndBadPayloadCharset` 锁死。
2. **列漏斗候选未应用兄弟列约束**（与 Excel 惯例/头文件自述矛盾）→
   `columnCandidates` 按「组+标签+其他列约束、排除本列」计算；
   `candidatesNarrowedBySiblingColumnConstraints` + UI 用例（名字约束后
   类型候选收窄到该行类型）锁死；跨页断言改为「无约束时其他页的值全列出」。
3. **matcher 边缘语义对齐旧实现**：Type/Status 不再 trim、Role/Entity/Tag
   空值回零命中（旧口径）、极性标记非 p/n 拒收；
   `emptyRoleEntityTagValuesKeepLegacyZeroHitSemantics` 锁死。同时移除
   `CompiledColumn::norm` 死代码与 `matchesWithoutColumns`（语义并入
   rest-query 全量 matches）。

修复后：tst_dataquery 23/23、tst_dataquery_ui 13/13、闸门全绿、
tst_panels_dataops 仅剩已知基线红（importledgerdialog setSizes）、
tst_assetpaging 全过。

## 闸门与回归状态（本机）

- 源码闸门 26 项全绿（layering 族/file_budget 族/ui_size/ui_invariants/
  i18n/ui_tokens/qgis_versions/docs_drift/cpp_mainline 等）。
- `tst_dataquery` 19/19、`tst_dataquery_ui` 13/13、`tst_assetpaging` 14/14
  （含 10k 档）。
- 其余引用 DataListPanel 的回归：`tst_datagraphpage` / `tst_projectsetup` /
  `tst_paleomaplibrary` / `tst_shell_projectlifecycle` / `tst_shell_chrome` /
  `tst_ui_blocking` 全过。
- `tst_panels_dataops` / `tst_ui` / `tst_panels` 与基线（stash 对照）失败集合
  逐用例一致：`dataops_d10_layerMarkers…`（importledgerdialog setSizes）、
  `faciesPartitionRibbon…`、`mapContextAndLayerSelection…`、
  `composePage_mappingChain…`——origin/master 固有红，与本任务无关，
  dataops 全部既有用例两态均过。
- `tst_shortcuts_shell::everyShellScopedCatalogEntryIsApplied` 红
  （缺 PaleoMainWindow/subIntervalSchemeCombo）：本 diff 对
  paleomainwindow*/shortcuts*/subintervalselectorbar.cpp 零改动
  （git diff 86454671..HEAD 该域 0 行），同输入同代码——基线固有红。
