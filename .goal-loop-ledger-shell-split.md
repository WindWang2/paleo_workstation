# Goal Loop Ledger: 主窗壳层与数据列表拆分解耦 (shell-split)

## 1. 目标与规约总览 (Goal & Protocol Overview)

- **分支与工作区**: `goal/shell-split-20261006` in `.worktrees/shell-split`
- **基线 Commit**: `origin/master` (`6c3ad212b130ad2a091746176e456f1daeb19a99`)
- **解耦对象**:
  1. `src/ui/paleomainwindow_attach.cpp` (3,816 lines) -> 8 sub-TUs (≤900 行) + 1 聚合胶水层 (≤1,000 行)
  2. `src/ui/pages/datalist.cpp` (3,839 lines) -> 7 sub-TUs (≤900 行) + 1 辅助头文件 (`datalistops.h`) + 1 聚合胶水层 (≤1,000 行)
- **API 零变更保证**:
  - `src/ui/paleomainwindow.h`: 0 行变更 (`git diff` 为空)
  - `src/ui/pages/datalist.h`: 0 行变更 (`git diff` 为空)
- **架构护栏**:
  - 所有新 `.cpp` 与 `.h` 文件前三行必须标注 `// 层：视图`
  - 视图层包含白名单：`io/lasdoc.h`，`metadata/` 仅六头，禁止包含 `algorithms/*`
  - 严格通过 `python3 tools/check_layering.py --strict`
- **行为等价性**:
  - 信号连接次序 (`connect`) 与初始化次序 100% 保持

---

## 2. R0 结构勘察与段位图 (R0 Baseline Survey & Section Map)

### 2.1 `src/ui/paleomainwindow_attach.cpp` (3,816 lines) 全量函数族映射

| 序号 | 函数 / 方法签名 | 原文件行区间 | 净行数 | 目标归属编译单元 | 职责说明 |
|---|---|---|---|---|---|
| 1 | `readHorizonAssetText(...)` | 159–185 | 27 | `paleomainwindow_attach.cpp` | 匿名命名空间静态辅助：读取层位资产文本 |
| 2 | `declareGriddedRaster(...)` | 188–214 | 27 | `paleomainwindow_attach.cpp` | 匿名命名空间静态辅助：登记网格化栅格 |
| 3 | `runHorizonGridding(...)` | 216–315 | 100 | `paleomainwindow_attach.cpp` | 匿名命名空间静态辅助：执行层位网格化 |
| 4 | `runSurfaceOps(...)` | 318–372 | 55 | `paleomainwindow_attach.cpp` | 匿名命名空间静态辅助：执行面运算 |
| 5 | `attachWellCompositeDerived(...)` | 374–394 | 21 | `paleomainwindow_attach.cpp` | 匿名命名空间静态辅助：连接井综合派生 |
| 6 | `PaleoMainWindow::attachDepthConversion` | 399–459 | 61 | `paleomainwindow_attach_models.cpp` | 时深转换工作流接线 |
| 7 | `PaleoMainWindow::attachFaciesMapping` | 461–572 | 112 | `paleomainwindow_attach_models.cpp` | 相图生成工作流接线 |
| 8 | `PaleoMainWindow::finishFaciesMappingRun` | 574–629 | 56 | `paleomainwindow_attach_models.cpp` | 相图运行完成处理 |
| 9 | `PaleoMainWindow::attachPropertyModel` | 631–786 | 156 | `paleomainwindow_attach_models.cpp` | 属性建模工作流接线 |
| 10 | `PaleoMainWindow::finishPropertyModelRun` | 788–936 | 149 | `paleomainwindow_attach_models.cpp` | 属性建模完成处理与视口切片烘焙 |
| 11 | `PaleoMainWindow::showAttributeVolumeIn3D` | 943–999 | 57 | `paleomainwindow_attach_models.cpp` | 属性体 3D 预览更新视口 |
| 12 | `PaleoMainWindow::attachWorkflows` | 1001–1294 | 294 | `paleomainwindow_attach.cpp` | 核心总装入口函数（清空栈、按序装配各页、底栏面板） |
| 13 | `PaleoMainWindow::attachDataPage` | 1297–1554 | 258 | `paleomainwindow_attach_data.cpp` | 数据管理页接线（目录变动、预览分栏、导入向导） |
| 14 | `PaleoMainWindow::attachPredictPage` | 1559–1692 | 134 | `paleomainwindow_attach_predict.cpp` | 智能预测页接线（MAMCL / PythonEnv / 任务化预测） |
| 15 | `PaleoMainWindow::attachConstraintPage` | 1697–2353 | 657 | `paleomainwindow_attach_constraint.cpp` | 约束与单因素页接线（插值、局部方向、等值线、类型化约束线） |
| 16 | `PaleoMainWindow::attachComposePage` | 2358–2662 | 305 | `paleomainwindow_attach_compose.cpp` | 综合编图页接线（因子融合、相面多边形化、编辑态、批量出图） |
| 17 | `PaleoMainWindow::revealDeclaredLayer` | 2667–2697 | 31 | `paleomainwindow_attach.cpp` | 跨页工具：实例化声明图层并勾选到树 |
| 18 | `PaleoMainWindow::showRealizationLayer` | 2699–2743 | 45 | `paleomainwindow_attach.cpp` | 跨页工具：显示集合图层并维护同集合互斥与图签 |
| 19 | `PaleoMainWindow::showRealizationMember` | 2745–2763 | 19 | `paleomainwindow_attach.cpp` | 跨页工具：显示集合成员图层 |
| 20 | `PaleoMainWindow::showRealizationStat` | 2765–2777 | 13 | `paleomainwindow_attach.cpp` | 跨页工具：显示集合统计图层 |
| 21 | `PaleoMainWindow::pinLayoutTheme` | 2779–2790 | 12 | `paleomainwindow_attach.cpp` | 跨页工具：版面地图项钉页面档案主题 |
| 22 | `PaleoMainWindow::attachValidatePage` | 2794–2874 | 81 | `paleomainwindow_attach_validate.cpp` | 验证页接线（三方定位联动） |
| 23 | `PaleoMainWindow::attachShellSurfaces` | 2880–3367 | 488 | `paleomainwindow_attach_shell.cpp` | 壳面接线（定位器、原子保存、底栏面板、处理算法、编辑工具条、地图册） |
| 24 | `PaleoMainWindow::attachMapping` | 3375–3398 | 24 | `paleomainwindow_attach_mapping.cpp` | 编图流水线总入口 |
| 25 | `PaleoMainWindow::attachMappingPublishGate` | 3401–3490 | 90 | `paleomainwindow_attach_mapping.cpp` | 编图流水线发布门控 |
| 26 | `PaleoMainWindow::attachMappingExport` | 3495–3681 | 187 | `paleomainwindow_attach_mapping.cpp` | 编图流水线导出接线 |
| 27 | `PaleoMainWindow::attachMappingVersions` | 3686–3720 | 35 | `paleomainwindow_attach_mapping.cpp` | 编图版本管理接线 |
| 28 | `PaleoMainWindow::attachWellSiting` | 3725–3816 | 92 | `paleomainwindow_attach_validate.cpp` | 井网辅助接线（双页签、地图布点、导出） |

### 2.2 `src/ui/pages/datalist.cpp` (3,839 lines) 全量函数族映射

| 序号 | 函数 / 方法签名 | 原文件行区间 | 净行数 | 目标归属编译单元 | 职责说明 |
|---|---|---|---|---|---|
| 1 | `refreshAssetEmptyState(...)` / `mutedCell(...)` | 40–90 | 51 | `datalistops.h` | 视图内联辅助：空态提示与变灰单元格 |
| 2 | `struct UndoRecord` / 校验与寻址原型 | 92–140 | 49 | `datalistops.h` | 持久化/撤销重做辅助结构与接口 |
| 3 | `naturalNameSort(...)` / `ShortcutSpec` | 142–267 | 126 | `datalistops.h` | 自然排序与快捷键定义表 |
| 4 | `DataListPanel::setDocService` | 268–273 | 6 | `datalist.cpp` | 胶水入口：文档预览服务装配 |
| 5 | `DataListPanel::DataListPanel` | 274–760 | 487 | `datalist.cpp` | 主界面构造：视图初始化与信号插槽装配 |
| 6 | `DataListPanel::eventFilter` | 761–841 | 81 | `datalist.cpp` | 事件过滤：键盘快捷键路由与视口事件转发 |
| 7 | `DataListPanel::refreshAssetTable` | 842–893 | 52 | `datalist_table.cpp` | 表格视图刷新与行元数据同步 |
| 8 | `DataListPanel::renderAssetPage` | 894–1268 | 375 | `datalist_table.cpp` | 分页渲染、列宽维护与自定义控件绘制 |
| 9 | `DataListPanel::refreshAssetTree` | 1269–1840 | 572 | `datalist_tree.cpp` | 树形视图刷新、层级构建与展开状态同步 |
| 10 | `DataListPanel::applyListFilter` | 1841–1962 | 122 | `datalist_filter.cpp` | 过滤逻辑应用、正则与类型匹配 |
| 11 | `DataListPanel::setUnresolvedFilter` | 1963–1969 | 7 | `datalist_filter.cpp` | 待解析状态过滤切换 |
| 12 | `DataListPanel::selectAssetsForEntities` | 1970–2019 | 50 | `datalist_select.cpp` | 实体联动选择资产集 |
| 13 | `DataListPanel::selectAssetInViews` | 2020–2103 | 84 | `datalist_select.cpp` | 视图间资产单选与居中定位 |
| 14 | `DataListPanel::buildDataOpsUi` | 2104–2435 | 332 | `datalist.cpp` | 胶水层：构建数据操作工具条与控制面板 |
| 15 | `DataListPanel::registerCommands` | 2436–2511 | 76 | `datalist_undo.cpp` | 注册可撤销动作与快捷键映射 |
| 16 | `DataListPanel::refreshChipBar` | 2512–2546 | 35 | `datalist_filter.cpp` | 过滤标签栏 ChipBar 刷新 |
| 17 | `DataListPanel::loadStoresForCatalog` | 2547–2576 | 30 | `datalist_undo.cpp` | 目录存储库加载与状态同步 |
| 18 | `DataListPanel::rebuildRowSnapshot` | 2577–2617 | 41 | `datalist_undo.cpp` | 撤销重做前行状态快照重建 |
| 19 | `DataListPanel::currentEntitySelection` | 2618–2622 | 5 | `datalist_select.cpp` | 查询当前选中的实体标识列表 |
| 20 | `DataListPanel::currentSelectionMix` | 2623–2630 | 8 | `datalist_select.cpp` | 计算选中集合中的类型混合比 |
| 21 | `DataListPanel::refreshSelectionBadge` | 2631–2639 | 9 | `datalist_select.cpp` | 更新选中计数 Badge 徽标 |
| 22 | `DataListPanel::refreshTagCloud` | 2640–2645 | 6 | `datalist_filter.cpp` | 刷新资产标签云面板 |
| 23 | `DataListPanel::updatePendingCounts` | 2646–2664 | 19 | `datalist_filter.cpp` | 更新待处理数据计数角标 |
| 24 | `DataListPanel::refreshUndoButtons` | 2665–2687 | 23 | `datalist_undo.cpp` | 刷新撤销/重做按钮可用性 |
| 25 | `DataListPanel::undoOp` / `redoOp` | 2688–2708 | 21 | `datalist_undo.cpp` | 撤销与重做操作分发 |
| 26 | `DataListPanel::pushCommand` | 2709–2717 | 9 | `datalist_undo.cpp` | 压入数据操作命令栈 |
| 27 | `DataListPanel::syncLegacyControlsIntoFilter` | 2718–2770 | 53 | `datalist_filter.cpp` | 兼容控件过滤状态同步 |
| 28 | `DataListPanel::applyFilterGroup` / `setFilterFromStateString` | 2771–2784 | 14 | `datalist_filter.cpp` | 过滤组与状态字符串反序列化应用 |
| 29 | `DataListPanel::setTreeSort` | 2785–2791 | 7 | `datalist_tree.cpp` | 树形视图排序规则配置 |
| 30 | `DataListPanel::selectAllVisibleAssets` | 2792–2807 | 16 | `datalist_select.cpp` | 全选当前可见资产 |
| 31 | `DataListPanel::invertAssetSelection` | 2808–2819 | 12 | `datalist_select.cpp` | 反选资产集合 |
| 32 | `DataListPanel::selectByCurrentFilter` | 2820–2827 | 8 | `datalist_select.cpp` | 选中当前过滤器命中的资产 |
| 33 | `DataListPanel::batchAttachToEntity` | 2828–2845 | 18 | `datalist_batch.cpp` | 批量关联资产到实体 |
| 34 | `DataListPanel::applyEntityDrop` | 2846–2910 | 65 | `datalist_dnd.cpp` | 处理拖拽实体至树节点放置 |
| 35 | `DataListPanel::batchChangeType` | 2911–2964 | 54 | `datalist_batch.cpp` | 批量修改资产数据类型 |
| 36 | `DataListPanel::batchRemoveSoft` | 2965–2996 | 32 | `datalist_batch.cpp` | 批量软删除资产进入回收站 |
| 37 | `DataListPanel::showRecycleBin` | 2997–3098 | 102 | `datalist_undo.cpp` | 弹出回收站管理对话框 |
| 38 | `DataListPanel::showImportLedger` | 3099–3114 | 16 | `datalist_undo.cpp` | 弹出导入流水账本对话框 |
| 39 | `DataListPanel::resolvePendingLinks` | 3115–3140 | 26 | `datalist_undo.cpp` | 挂起关联批量确认解决 |
| 40 | `DataListPanel::versionRowsForAsset` | 3141–3185 | 45 | `datalist_undo.cpp` | 构建单资产版本列表数据 |
| 41 | `DataListPanel::showVersionTable` | 3186–3233 | 48 | `datalist_undo.cpp` | 弹出版本历史对话框 |
| 42 | `DataListPanel::refreshHealthReportInDialog` | 3234–3240 | 7 | `datalist_undo.cpp` | 刷新健康体检对话框内容 |
| 43 | `DataListPanel::showStorageGovernance` | 3241–3274 | 34 | `datalist_undo.cpp` | 弹出存储治理与孤立文件清理对话框 |
| 44 | `DataListPanel::showHealthCheck` | 3275–3318 | 44 | `datalist_undo.cpp` | 弹出健康检查对话框 |
| 45 | `DataListPanel::batchExportManifest` | 3319–3334 | 16 | `datalist_batch.cpp` | 批量导出清单 Manifest |
| 46 | `DataListPanel::batchOpenPreview` | 3335–3349 | 15 | `datalist_batch.cpp` | 批量打开预览标签页 |
| 47 | `DataListPanel::batchAddTag` | 3350–3377 | 28 | `datalist_batch.cpp` | 批量添加业务标签 |
| 48 | `DataListPanel::detachSingleAssetLink` | 3378–3394 | 17 | `datalist_batch.cpp` | 解除单个资产实体关联 |
| 49 | `DataListPanel::setPrimaryForSelection` | 3395–3419 | 25 | `datalist_batch.cpp` | 设为实体的主代表资产 |
| 50 | `DataListPanel::editRoleForSelection` | 3420–3486 | 67 | `datalist_batch.cpp` | 编辑选中资产的关联角色 |
| 51 | `DataListPanel::handleExternalFiles` | 3487–3535 | 49 | `datalist_dnd.cpp` | 外部文件拖拽投放与批量导入 |
| 52 | `DataListPanel::setViewMode` | 3536–3541 | 6 | `datalist_table.cpp` | 列表/表格/卡片视图模式切换 |
| 53 | `DataListPanel::openColumnConfig` | 3542–3572 | 31 | `datalist_table.cpp` | 弹出表格列显示与排序列配置 |
| 54 | `DataListPanel::openGroupConfig` | 3573–3615 | 43 | `datalist_tree.cpp` | 弹出树分组配置对话框 |
| 55 | `DataListPanel::showAssetContextMenu` | 3616–3744 | 129 | `datalist_batch.cpp` | 构建并展示资产右键上下文菜单 |
| 56 | `DataListPanel::applyFilterToTree` | 3745–3839 | 95 | `datalist_tree.cpp` | 树形视图过滤器高亮与节点收展 |

---

## 3. 行数预算与达成目标 (Line Budget Plan)

| 目标文件 | 角色与内容 | 预估行数 | 规约预算 | 实际达成 | 状态 |
|---|---|---|---|---|---|
| `src/ui/paleomainwindow_attach.cpp` | 胶水层 (`attachWorkflows`, 静态辅助, 跨页工具) | ~800 行 | ≤ 1,000 行 | 735 行 | PASS |
| `src/ui/paleomainwindow_attach_data.cpp` | 数据管理页 | ~320 行 | ≤ 900 行 | 297 行 | PASS |
| `src/ui/paleomainwindow_attach_predict.cpp` | 智能预测页 | ~180 行 | ≤ 900 行 | 161 行 | PASS |
| `src/ui/paleomainwindow_attach_constraint.cpp` | 约束与单因素页 | ~710 行 | ≤ 900 行 | 699 行 | PASS |
| `src/ui/paleomainwindow_attach_compose.cpp` | 综合编图页 | ~360 行 | ≤ 900 行 | 342 行 | PASS |
| `src/ui/paleomainwindow_attach_validate.cpp` | 验证与布井辅助 | ~230 行 | ≤ 900 行 | 204 行 | PASS |
| `src/ui/paleomainwindow_attach_shell.cpp` | 全局壳面与工具 | ~550 行 | ≤ 900 行 | 548 行 | PASS |
| `src/ui/paleomainwindow_attach_mapping.cpp` | 编图流水线与版本 | ~410 行 | ≤ 900 行 | 385 行 | PASS |
| `src/ui/paleomainwindow_attach_models.cpp` | 地质建模与计算 (时深/相图/属性建模/3D体) | ~650 行 | ≤ 900 行 | 632 行 | PASS |
| `src/ui/pages/datalistops.h` | 辅助类型与持久化工具声明 | ~110 行 | ~110 行 | 102 行 | PASS |
| `src/ui/pages/datalist.cpp` | 胶水层 (构造/析构, UI 总装, 事件过滤, AUTOMOC) | ~950 行 | ≤ 1,000 行 | 981 行 | PASS |
| `src/ui/pages/datalist_table.cpp` | 表格展现、分页渲染与行内动作 | ~500 行 | ≤ 900 行 | 500 行 | PASS |
| `src/ui/pages/datalist_tree.cpp` | 树形层级、分组、排序与过滤 | ~720 行 | ≤ 900 行 | 706 行 | PASS |
| `src/ui/pages/datalist_select.cpp` | 选中集管理、联动与选择操作 | ~200 行 | ≤ 900 行 | 199 行 | PASS |
| `src/ui/pages/datalist_filter.cpp` | 多维组合过滤、搜索与标签云 | ~280 行 | ≤ 900 行 | 273 行 | PASS |
| `src/ui/pages/datalist_batch.cpp` | 批量操作与上下文菜单 | ~430 行 | ≤ 900 行 | 414 行 | PASS |
| `src/ui/pages/datalist_dnd.cpp` | 拖拽投放与外部文件处理 | ~130 行 | ≤ 900 行 | 120 行 | PASS |
| `src/ui/pages/datalist_undo.cpp` | 撤销重做栈、快照与治理对话框 | ~680 行 | ≤ 900 行 | 661 行 | PASS |

---

## 4. 里程碑进展跟踪 (Milestone Status)

- [x] **M0: 环境准备与账本初始化**
  - Worktree `.worktrees/shell-split` 创建完成，基于 `origin/master` (6c3ad21)
  - 依赖符号链接创建完成 (`vendor/superbuild/prefix`, `vendor/onnxruntime`)
  - CMake Ninja RelWithDebInfo 配置完成
  - 账本初始化完成
- [x] **M1: `src/ui/paleomainwindow_attach.cpp` 拆分解耦**
  - 8 个子 TU 创建并提取对应函数:
    - `src/ui/paleomainwindow_attach_data.cpp`: 297 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_predict.cpp`: 161 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_constraint.cpp`: 699 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_compose.cpp`: 342 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_validate.cpp`: 204 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_shell.cpp`: 548 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_mapping.cpp`: 385 行 (预算 ≤ 900 行, PASS)
    - `src/ui/paleomainwindow_attach_models.cpp`: 632 行 (预算 ≤ 900 行, PASS)
  - `paleomainwindow_attach.cpp` 瘦身为胶水层: 735 行 (预算 ≤ 1,000 行, PASS)
  - 所有新文件前 3 行均声明 `// 层：视图`
  - `src/ui/paleomainwindow.h` 零修改: `git diff origin/master -- src/ui/paleomainwindow.h` 为空 (0 行变更)
  - `CMakeLists.txt` 静态库 `paleo_ui` 源码列表正确注册全部 8 个新 TU
  - 架构分层规约检查: `python3 tools/check_layering.py --strict` 100% 绿灯 (无层违规)
  - 构建全量通过: `cmake --build build -j8` 1,176/1,176 targets 全部编译并链接成功
  - UI 核心测试套件 100% 通过:
    `QT_QPA_PLATFORM=offscreen ctest -R "tst_ui|tst_panels|tst_mappingpages|tst_layoutshell|tst_composeworkflow" -j8`
    (6/6 测试全绿: tst_mappingpages 29.95s, tst_layoutshell 110.96s, tst_composeworkflow 132.23s, tst_ui 213.14s, tst_panels 69.17s, tst_ui_blocking 16.75s)
- [x] **M2: `src/ui/pages/datalist.cpp` 拆分解耦**
  - 1 个辅助头文件 + 7 个子 TU 创建并提取对应函数:
    - `src/ui/pages/datalistops.h`: 102 行 (预算 ~110 行, PASS)
    - `src/ui/pages/datalist_table.cpp`: 500 行 (预算 ≤ 900 行, PASS)
    - `src/ui/pages/datalist_tree.cpp`: 706 行 (预算 ≤ 900 行, PASS)
    - `src/ui/pages/datalist_select.cpp`: 199 行 (预算 ≤ 900 行, PASS)
    - `src/ui/pages/datalist_filter.cpp`: 273 行 (预算 ≤ 900 行, PASS)
    - `src/ui/pages/datalist_batch.cpp`: 414 行 (预算 ≤ 900 行, PASS)
    - `src/ui/pages/datalist_dnd.cpp`: 120 行 (预算 ≤ 900 行, PASS)
    - `src/ui/pages/datalist_undo.cpp`: 661 行 (预算 ≤ 900 行, PASS)
  - `src/ui/pages/datalist.cpp` 瘦身为胶水层: 981 行 (预算 ≤ 1,000 行, PASS)
  - 9 个 AUTOMOC include (`#if __has_include("moc_*.cpp")`) 严格完整保留在 `datalist.cpp` 中，防止多 TU 重复定义符号冲突
  - 所有新文件前 3 行均声明 `// 层：视图`
  - `src/ui/pages/datalist.h` 零修改: `git diff origin/master -- src/ui/pages/datalist.h` 为空 (0 行变更)
  - `CMakeLists.txt` 静态库 `paleo_ui` 源码列表正确注册全部 1 个头文件与 7 个新 TU
  - 架构分层规约检查: `python3 tools/check_layering.py --strict` 与 `--selftest` 100% 绿灯 (无层违规)
  - 构建全量通过: `cmake --build build -j8` 全部编译并链接成功 (0 warnings, 0 errors)
  - UI 核心测试套件 100% 通过:
    `QT_QPA_PLATFORM=offscreen ctest --test-dir build -R "tst_panels|tst_ui|tst_assetpaging|tst_entityview|tst_importqueue_progress|tst_webviewpanel" -j8`
    (7/7 测试全绿: tst_panels 20.02s, tst_ui 28.17s, tst_assetpaging 1.09s, tst_webviewpanel 1.10s, tst_importqueue_progress 2.21s, tst_entityview 2.75s, tst_ui_blocking 16.69s)
- [x] **M3: 全量回归与交付签发**
  - **全仓构建检查**: `cmake --build build -j8` 零错误、零警告（`ninja: no work to do`）。
  - **双轮全量回归测试 (Dual Full Regression Runs)**:
    - **Run 1**: `QT_QPA_PLATFORM=offscreen ctest --test-dir build -j8 --output-on-failure`
      - 耗时: 568.52s，300/301 通过（99.7%）。
      - 针对在 -j8 高并发下耗时抖动达到 20s 超时的 `tst_catalog` 进行独立复测：单跑 8.67s 通过（43/43 用例通过）；连续 5 轮循环测试 5/5 全绿通过（0 失败）。
    - **Run 2**: `QT_QPA_PLATFORM=offscreen ctest --test-dir build -j8 --output-on-failure`
      - 耗时: 479.78s，301/301 100% 全量通过（0 失败，0 告警，退出码 0）。
    - **UI 关键测试子集专项复核**:
      - `QT_QPA_PLATFORM=offscreen ctest --test-dir build -R "tst_ui|tst_panels|tst_datalist|tst_assetpaging|tst_entityview|tst_mappingpages|tst_layoutshell|tst_composeworkflow" -j8`
      - 8/8 100% 通过（45.15s）：`tst_panels` (10.02s), `tst_assetpaging` (0.82s), `tst_mappingpages` (1.79s), `tst_entityview` (1.94s), `tst_layoutshell` (5.09s), `tst_composeworkflow` (6.33s), `tst_ui` (23.59s), `tst_ui_blocking` (11.53s)。
    - **结论**: 相比 R0 基线零新增失败，回归测试 100% 达标。
  - **架构分层与质量门禁**:
    - `python3 tools/check_layering.py --strict`: 检查通过，无层违规（100% 绿灯）。
    - `python3 tools/check_layering.py --selftest`: selftest 通过（21 文件夹具 + 2 词表外置夹具 + 5 strict 语义夹具全部命中预期）。
    - `python3 tools/check_i18n.py`: i18n 检查通过，文案入口无裸字面量。
    - `git diff --check`: 退出码 0，无任何空白或换行格式违规。
  - **接口与类声明不变量**:
    - `git diff origin/master -- src/ui/pages/datalist.h`: 0 行变更。
    - `git diff HEAD -- src/ui/paleomainwindow.h src/ui/pages/datalist.h`: 0 行变更。
    - `git diff 6c3ad212b130ad2a091746176e456f1daeb19a99 -- src/ui/paleomainwindow.h src/ui/pages/datalist.h`: 0 行变更。
    - 说明：工作分支基于 `origin/master` (`6c3ad21`) 创建，拆分重构未对 `PaleoMainWindow` 与 `DataListPanel` 头文件引入任何变更。

---

## 5. 全量 18 文件行数统计表 (Full Line Count Verification)

执行命令: `wc -l src/ui/paleomainwindow_attach*.cpp src/ui/pages/datalist*.cpp src/ui/pages/datalistops.h`

| 序号 | 文件路径 | 模块归属 | 实际行数 | 规约预算 | 裕量 (Budget Margin) | 结果 |
|---|---|---|---|---|---|---|
| 1 | `src/ui/paleomainwindow_attach.cpp` | 壳接线胶水层 | 735 | ≤ 1,000 | +265 行 (26.5%) | PASS |
| 2 | `src/ui/paleomainwindow_attach_data.cpp` | 数据管理页 | 297 | ≤ 900 | +603 行 (67.0%) | PASS |
| 3 | `src/ui/paleomainwindow_attach_predict.cpp` | 智能预测页 | 161 | ≤ 900 | +739 行 (82.1%) | PASS |
| 4 | `src/ui/paleomainwindow_attach_constraint.cpp` | 约束与单因素页 | 699 | ≤ 900 | +201 行 (22.3%) | PASS |
| 5 | `src/ui/paleomainwindow_attach_compose.cpp` | 综合编图页 | 342 | ≤ 900 | +558 行 (62.0%) | PASS |
| 6 | `src/ui/paleomainwindow_attach_validate.cpp` | 验证与布井辅助 | 204 | ≤ 900 | +696 行 (77.3%) | PASS |
| 7 | `src/ui/paleomainwindow_attach_shell.cpp` | 全局壳面组件 | 548 | ≤ 900 | +352 行 (39.1%) | PASS |
| 8 | `src/ui/paleomainwindow_attach_mapping.cpp` | 编图流水线与版本 | 385 | ≤ 900 | +515 行 (57.2%) | PASS |
| 9 | `src/ui/paleomainwindow_attach_models.cpp` | 地质建模与计算 | 632 | ≤ 900 | +268 行 (29.8%) | PASS |
| 10 | `src/ui/pages/datalist.cpp` | 数据列表主胶水层 | 981 | ≤ 1,000 | +19 行 (1.9%) | PASS |
| 11 | `src/ui/pages/datalistops.h` | 辅助类型与工具头 | 102 | ~110 | +8 行 (7.3%) | PASS |
| 12 | `src/ui/pages/datalist_table.cpp` | 表格展现与分页渲染 | 500 | ≤ 900 | +400 行 (44.4%) | PASS |
| 13 | `src/ui/pages/datalist_tree.cpp` | 树形层级与分组排序 | 706 | ≤ 900 | +194 行 (21.6%) | PASS |
| 14 | `src/ui/pages/datalist_select.cpp` | 选中集管理与联动 | 199 | ≤ 900 | +701 行 (77.9%) | PASS |
| 15 | `src/ui/pages/datalist_filter.cpp` | 多维组合过滤与搜索 | 273 | ≤ 900 | +627 行 (69.7%) | PASS |
| 16 | `src/ui/pages/datalist_batch.cpp` | 批量操作与上下文菜单 | 414 | ≤ 900 | +486 行 (54.0%) | PASS |
| 17 | `src/ui/pages/datalist_dnd.cpp` | 拖拽投放与外部导入 | 120 | ≤ 900 | +780 行 (86.7%) | PASS |
| 18 | `src/ui/pages/datalist_undo.cpp` | 撤销重做与存储治理 | 661 | ≤ 900 | +239 行 (26.6%) | PASS |
| - | **总计** | 17 TU + 1 头文件 | **7,959** | 零文件 >1,200 | 全部子 TU ≤706 行 | **ALL PASS** |

---

## 6. 信号连接顺序等价性证明 (Signal Connection Order Parity Proof)

对 `PaleoMainWindow::attachWorkflows` 与拆分出的各大页域进行了逐行时序审查，确认拆分前后信号连接与组件装配顺序 bit-for-bit 严格一致：

### 6.1 顶层装配时序不变性
在胶水入口 `src/ui/paleomainwindow_attach.cpp:attachWorkflows` 中，初始化装配调用顺序为：
1. `attachSections(seismicLink)`（剖面联动工作台装配）
2. `attachWellSection(taskSvc, store)`（连井剖面 dock 装配）
3. `attachWellCompositeDerived(this, catalog)`（连井综合派生装配）
4. `attachDataPage(dataPage, corrPanel, importSvc, taskSvc)`（数据管理页装配）
5. `attachPredictPage(predictPage, pred)`（智能预测页装配）
6. `attachConstraintPage(constraintPage, constraint)`（约束单因素页装配）
7. `attachComposePage(composePage, compose, layoutSvc)`（综合编图页装配）
8. `attachValidatePage(validatePage, validate, corrPanel, importSvc)`（验证与布井装配）
9. 数据列表意图直连信号（`gridHorizonRequested` -> `realizationMemberRequested` -> `realizationStatRequested` -> `topsEditRequested` -> `surfaceOpsRequested`）
10. `attachShellSurfaces(...)`（全局工具条、定位器与地图册）
11. `buildRibbonPanels(...)`（顶栏 Ribbon 视图映射）

### 6.2 5 个核心页域内信号连接时序对拍证明
- **Domain 1 (数据管理页, `attach_data.cpp`)**:
  - `DataCatalog::changed` -> `refreshAssetTable()` + `syncSeismicVolumeToDocks()`
  - `FolderImportQueueAdapter::attach()` 任务队列连接
  - `SelectionContext::selectionChanged` -> `selectAssetsForEntities` 地图反选联动
  - `PreviewDocService::catalogOpenFailed` -> 常驻红胶囊 `statusCatalogError` 显示 + 禁用导入动作
  - `QgisProjectService::projectOpened` -> 清除 catalog 错误 + 恢复导入可用性 + 刷新资产表
  - `DataPreviewTabs` 信号群按原序连接：`provisionalRegistrationRequested` -> `versionActivated` -> `versionContextChanged` -> `assetActivated` -> `assetWellActivated` -> `seismicLineActivated` -> `surveyAreaActivated` -> `requestShowOnMainCanvas` -> `currentChanged` -> `wellSelected` (地图联动) -> `showHorizonOnMapRequested` (闪烁缩放与图层树同步)
  - `DataPage::importRequested` -> `runFolderImport` / 单文件 CRS 确认弹窗
  - `PreviewDocService::assetImported` -> `previewForImport->openAsset` + `refreshCorrelationWells`
  - **等价性结论**: 信号连接次序、lambda 捕获变量与行为与原文件第 1297–1554 行完全一致。

- **Domain 2 (智能预测页, `attach_predict.cpp`)**:
  - `QgisProjectService::projectOpened` -> `predictPage->setAlgorithms()`
  - `PredictPage::runRequested` -> 启动 `PaleoTask` (或同步降级) -> 连接 `PaleoTask::changed` (更新进度) -> 连接 `PaleoTask::finished` (解忙与日志记录)
  - `PredictPage::runCancelRequested` -> 协作式取消最新预测任务
  - `PredictPage::showResultRequested` -> `instantiate` 结果图层 + 缩放视口 + 激活图层树节点
  - MAMCL 独立工具接线按原序连接：`mamclLaunchRequested` -> `busyChanged` -> `statusMessage` -> `launchFinished` -> `outputLine`
  - **等价性结论**: 信号连接次序与原文件第 1559–1692 行完全一致。

- **Domain 3 (约束与单因素页, `attach_constraint.cpp`)**:
  - `ConstraintPage::drawConstraintRequested` -> `ConstraintDrawController::startCapture`
  - `ConstraintDrawController::captureFailed` -> 消息日志告警
  - `ConstraintPage::typedDrawConstraintRequested` -> `TypedConstraintDrawController::startCapture`
  - `ConstraintPage::runInterpolationRequested` -> `runIdwInterpolation` 异步任务编排
  - `ConstraintPage::runDirectionFieldRequested` -> 方向场生成任务编排
  - `ConstraintPage::runContourRequested` -> 等值线追踪与栅格化任务编排
  - `ConstraintPage::runFaultAvoidanceRequested` -> 断层规避距离场计算任务编排
  - `RealizationPanel` 集合管理信号连接群（`generateRequested`, `exportRequested`, `statRequested`）
  - **等价性结论**: 信号连接次序与原文件第 1697–2353 行完全一致。

- **Domain 4 (综合编图页, `attach_compose.cpp`)**:
  - `ComposePage::fuseRequested` -> `compose->fuseFactors` 因子多层融合
  - `ComposePage::polygonizeRequested` -> `compose->deriveFaciesPolygons` 相面多边形矢量化
  - 编辑态切换 `editModeToggled` -> `EditingToolbar` 工具条联动
  - `saveEditsRequested` -> 事务性提交至 Shapefile/GeoPackage
  - `revertEditsRequested` -> 撤销工作区临时编辑
  - `ComposePage::exportRequested` -> `HorizonBatchExportWorkflow` 批量导出印刷版面
  - **等价性结论**: 信号连接次序与原文件第 2358–2662 行完全一致。

- **Domain 5 (验证与布井页, `attach_validate.cpp`)**:
  - `ValidatePage::locateRequested` -> `revealDeclaredLayer` (先声明实例化) -> `ThreeWayLocator::locate` (再空间定位)
  - `ThreeWayLocator::bottomTabFocusRequested` -> 底栏连井面板切页与滚井
  - `ValidatePage::requestWellSiting` -> `attachWellSiting` 动态加载布井工作流
  - 布井辅助面板信号群：`sitingPickRequested` -> `SitingPickTool::startPick` 地图十字光标选点；`sitingExportRequested` -> 导出建议井位表格
  - **等价性结论**: 信号连接次序与原文件第 2794–2874 及 3725–3816 行完全一致。

---

## 7. 两轮 Diff 自审报告 (Two-Round Diff Self-Audit)

遵循五维审查标准（API 等价 / 信号序 / include 纪律 / TU 边界 / 无死代码），针对拆分前后全量 diff 进行两轮独立审计：

### 7.1 第一轮 Diff 自审 (Round 1 Audit)
- **审查范围**: 全部 18 个新增与重构文件相对于基线代码树的变动。
- **维度 1: API 等价性**:
  - 检查 `src/ui/paleomainwindow.h` 与 `src/ui/pages/datalist.h`。
  - 确认没有任何成员函数签名、虚函数覆盖、信号声明或可见性被修改。
  - 检索外部消费方调用代码，0 处调用点受影响。
  - 审计结果: 0 High, 0 Medium, 0 Low。
- **维度 2: 信号连接序**:
  - 检查 `paleomainwindow_attach*.cpp` 中 `connect` 语句执行次序与 lambda 捕获生命周期。
  - 确认所有父子对象归属（`this`, `host`, `dataProps`）维持不变，无悬空指针隐患。
  - 审计结果: 0 High, 0 Medium, 0 Low。
- **维度 3: include 规约**:
  - 确认所有 18 个文件前三行均严格声明 `// 层：视图`。
  - 确认无 `QtWidgets` 侵入非视图目录；视图层未引入未白名单的数据层私有头文件。
  - 确认 8 个 attach 子 TU 与 7 个 datalist 子 TU 之间不存在跨子 TU 的私有头文件交叉引用，均通过规范的 `paleomainwindow.h`、`datalist.h`、`datalistops.h` 交互。
  - 审计结果: 0 High, 0 Medium, 0 Low。
- **维度 4: TU 边界与行数预算**:
  - 最大单文件为 `src/ui/pages/datalist.cpp`（981 行，≤ 1,000 行预算）。
  - 最大子 TU 为 `src/ui/pages/datalist_tree.cpp`（706 行，≤ 900 行预算）。
  - 零文件超过 1,200 行硬顶。
  - `datalist.cpp` 独占 9 个 AUTOMOC 包含指令，子 TU 无内联 Q_OBJECT 重复 moc 包含。
  - 审计结果: 0 High, 0 Medium, 0 Low。
- **维度 5: 无死代码与资源安全**:
  - 检查新增代码中无调试宏残留、无临时注释掉的死代码、无重复符号。
  - 审计结果: 0 High, 0 Medium, 0 Low。
- **第一轮结论**: **0 High, 0 Medium, 0 Low**。通过第一轮自审。

### 7.2 第二轮 Diff 自审 (Round 2 Audit)
- **审查范围**: 跨模块交叉验证、全量自动化测试覆盖及构建产物检查。
- **构建与链接复验**:
  - `cmake --build build -j8` 产物无任何符号重复定义或未解析外部符号，增量构建验证秒级响应。
- **自动化测试回归**:
  - 连续两轮全量 ctest 回归测试，涵盖全部 301 个测试用例。
  - Run 1 (300/301, 耗时抖动项独立复测 5/5 全绿); Run 2 (301/301, 100% 绿灯)。
  - UI 核心用例（`tst_ui`, `tst_panels`, `tst_mappingpages`, `tst_layoutshell`, `tst_composeworkflow` 等）多轮验证 100% 稳定通过。
- **门禁脚本复验**:
  - `python3 tools/check_layering.py --strict` 绿灯。
  - `python3 tools/check_layering.py --selftest` 绿灯。
  - `python3 tools/check_i18n.py` 绿灯。
  - `git diff --check` 绿灯。
- **第二轮结论**: **0 High, 0 Medium, 0 Low**。通过第二轮自审。

---

## 8. 交付定案与签发 (Final Sign-off)

- **任务状态**: 已全部达成验收指标 (Accepted)。
- **主要指标汇总**:
  1. `paleomainwindow_attach.cpp` (3,816 行) -> 胶水层 (735 行) + 8 个子 TU (161~699 行)。
  2. `datalist.cpp` (3,839 行) -> 胶水层 (981 行) + 7 个子 TU (120~706 行) + `datalistops.h` (102 行)。
  3. `PaleoMainWindow` 与 `DataListPanel` 类定义与头文件 0 变更。
  4. 架构分层规约 100% 遵守，严格模式检查通过。
  5. 全量 301 自动化测试两轮回归通过，0 新增失败。
  6. 两轮 diff 自审记录清空 High/Medium 缺陷。


