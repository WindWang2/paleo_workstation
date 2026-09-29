# 数据管理页交互审计（P3 Phase 0 产出）

日期：2026-09-29。分支：`wave/data-page-operations`（基线 master@93b4195）。
范围：`src/ui/pages/datalist.{h,cpp}`、`src/ui/pages/entitypanel.{h,cpp}`、
`src/ui/pages/datapage.{h,cpp}`（W5 分家后三件套），对照
`tests/tst_panels.cpp` 22 个 dataPage 用例 + `tests/tst_ui.cpp` 分栏契约用例。

## 1. 现有操作清单（基线）

### 1.1 导入段（dataImportSection）
| 入口 | objectName | 行为 |
|---|---|---|
| 导入井数据 | `importWells` | 发 `importRequested("wells")`，壳执行单文件导入 |
| 导入测井数据 | `importWellLogs` | `importRequested("well_log")` |
| 导入地震数据 | `importSeismic` | `importRequested("seismic")` |
| 导入边界数据 | `importBoundary` | `importRequested("boundary")` |
| 导入工区文件夹 | `importFolder` | `importRequested("folder")`（T22 确认表流程） |

均为单发信号，无进度、无重试、无预设——D8 增强面。

### 1.2 列表段（dataListSection）
- 搜索框 `assetSearchEdit`（名称/类型/关联三列 contains 匹配）。
- 类型下拉 `assetTypeFilter`（按 catalog 资产类型集重建，中文标签映射）。
- 计数 `assetCountLabel`（「显示 x / 共 y 条」）。
- 未决过滤条 `unresolvedFilterBar`（T31「查看未决」：只留有 unresolved
  链接的行；`clearUnresolvedFilterButton` 清除）。
- 视图切换 `treeViewButton`/`listViewButton`（QButtonGroup →
  `dataViewStack` 0=树 `dataTree`、1=表 `assetTable`）。
- 展开/折叠全部：`expandAllTreeButton`/`collapseAllTreeButton`。
- 资产表 3 列（名称/类型/关联）；关联列嵌单元格控件：
  已决名 + `unresolvedBadge` + `resolveEntityCombo` + `attachLinkButton`
  → 页内确认条（`attachConfirmText` + 确认/取消）→ `undoAttachButton`
  （T28 undo vault，sidecar `.paleo/undo_stack.json`）+ `setPrimaryButton`。
- 树五分组：测区 / 井（实体子树按角色序）/ 测井（柱状图+曲线）/ 地震
  （体下挂 Inline/Crossline 测线节点）/ 层位 / 辅助资料（相图+文档）。

### 1.3 属性段（entityViewSection = EntityPanel）
五折叠组：基本信息 / 空间与几何 / 业务角色与关联（`entityRoleTable`
4 列）/ 属性明细（`propDetailsText`）/ 下游派生产物
（`derivedProductsTable` + `missingSourcesLabel` 悬空血缘）。
三态：空态（工程未开/未选中）、资产态（8 类类型的属性渲染）、实体态
（entityDataView 角色槽 + 派生闭包）。

## 2. 键盘可达性（基线）

| 面 | 现状 | 缺口 |
|---|---|---|
| 树 | Qt 内建：↑↓ 移动、Enter/双击激活（handleTreeActivation）、Space 无勾选语义、Ctrl+方向有 | 无多选（SingleSelection）、无 F2、无快捷键表、无 Esc 语义 |
| 表 | Qt 内建：↑↓、Enter 选中即 `assetActivated` | SingleSelection；无 Ctrl+A/反选；无命令面板 |
| 搜索 | QLineEdit 聚焦即输入，无 `/` 快捷聚焦 | 无快捷聚焦、无高亮命中（D2.7） |
| 全局 | 主窗焦点环 2px #1B73D0（tst_ui 断言）；无 Ctrl+K（DESIGN.md 规划 QgsLocator 属壳） | 数据页内 Ctrl+K 面板、Vim 风可选导航均无 |

## 3. 多选支持度（基线）：**零**

`dataTree`/`assetTable` 均默认 SingleSelection（QAbstractItemView 缺省）。
`selectAssetsForEntities` 用 QItemSelection 一次多行选中是唯一多行路径
（地图联动），但用户不可 Ctrl/Shift 手选。D1 全部为新增面。

## 4. 右键菜单矩阵（基线）：**零**

无任何 contextMenuEvent/customContextMenuRequested。全部操作走
单元格按钮与双击。D1.3/D7.8/D4.x 的菜单面全部新增。

## 5. 自动布局行为（冻结契约，D9 回归基线）

- 分栏 `dataListPreviewSplit`（壳持有）：宽度只在**用户拖手柄 / 最大化钮
  （`previewMaxButton`）/ 窗口 resize** 三种情况变。tst_ui 两条用例钉死：
  `previewSplitterBudgetAndMaximize`、`dataListWidthUnchangedOnItemActivationAndFollowsUser`。
- 列宽：`DataListPanel::eventFilter` 在 Resize 时按视口宽比例调
  树 1 列（35–75px 夹紧）与表 1/2 列（30–60/35–75px 夹紧）——
  `dataPage_columnWidthPreservesFilenameOnShrink` 钉住收缩时名称列
  （Stretch）不吞其它列。新增代码路径不得引入对分栏的 setSizes。
- EntityPanel 值标签 `QSizePolicy::Ignored` 水平——内容不撑宽 dock。

## 6. 模型/选中/刷新时序（测试断言面）

- 刷新入口 `refreshAssetTable()`：清控件→重建表→重建类型下拉→
  `refreshAssetTree()`→`applyListFilter()`；确认条状态经动态属性
  `paleo.page.pendingConfirm` 跨刷新存活（`dataPage_confirmStripSurvivesRefresh`）。
- 动作身份寻址（T28）：挂接/撤销/设主在**点击时刻**按
  (assetId, role[, entityId]) 重扫 `links()`，不持行下标
  （`dataPage_actionsHitOwnLinkAfterInterleavedChanges`）。
- undo vault 随工程 sidecar 持久，跨 open() 存活
  （`dataPage_undoRestoresDemotedPrimaryAndNoteAcrossReload`）；
  被降级主关联在撤销时恢复（D4）。

## 7. 已知行为边界（审计结论，本波改造承接）

1. `assetTable` 为 QTableWidget 全量建行——10k 资产行数即性能悬崖，
   D7.3 虚拟滚动以新视图页落（不换 `assetTable` 本体，测试兼容面冻结）。
2. catalog 无 entity 更新/删除、asset 删除/改型、link 删除/改角色 API
   （datacatalog.h 只有 add*/attach/setLink*）——D4.1/D4.2/D4.6/D4.7/D1.5
   的写回经视图层 sidecar override + 组合既有 mutator 实现，缺口登记
   GAPS.md。
3. 预览区/标签栏属壳（datapreview/ 领地外）——D3.3 拖拽到预览区以
   拖源 + mime + 信号交付，宿主接线记 GAPS。
4. 树节点 `dataTree` 五分组是语义导航（井实体×资产混合），与 D7.6
   「分组平表」正交；分组平表落新视图页，不重载 dataTree 语义。
