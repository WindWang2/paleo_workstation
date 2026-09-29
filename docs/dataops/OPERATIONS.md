# 数据管理页操作矩阵（P3 交付文档）

日期：2026-09-29。分支：`wave/data-page-operations`。
适用面：数据管理页（`DataListPanel` + `EntityPanel` + `DataPage` 薄壳）。

## 1. 操作入口总览

| 入口 | objectName / API | 说明 |
|---|---|---|
| 右键菜单（表/树） | `dataContextMenu` | 按 D1.3 矩阵分流（§2） |
| 拖放（树内） | `DataNavTree` | 挂接/转移/打标签（§3） |
| 拖放（外部→树） | `externalFilesDropped` | 导入流（§4，见 IMPORT.md） |
| 工具行 | `dataUndoButton` `dataRedoButton` `treeSortCombo` `columnConfigButton` | 撤销/排序/列配置 |
| 视图模式行 | `treeViewButton` `listViewButton` `iconViewButton` `virtualViewButton` `groupViewButton` | D7.1 五态 |
| 命令面板 | `Ctrl+K`（DataPage） | 见 KEYBOARD.md |
| 实体 CRUD 条 | `entityCreateButton` `entityRenameButton` `entityDeleteButton` `entityTopologyButton` `entityHistoryButton` | 属性面板顶部 |
| 编程式 | `DataListPanel::applyEntityDrop` `pushCommand` `batchXxx()` 系 | 测试/自动化直调 |

## 2. 右键菜单矩阵（D1.3）

`contextMenuActions(ContextMenuSpec)`（dataopspanelops.h，纯逻辑）：

| 选中态 | 菜单项 |
|---|---|
| 单资产（已决） | 打开预览 / 解除挂接 / 设为主关联 / 编辑挂接角色 / 转移到其它实体 / 打标签 / 改类型 / 导出清单 / 在文件管理器中显示 |
| 单资产（未决） | 打开预览 / 挂接到实体 / 打标签 / 改类型 / 导出清单 / 显示位置 |
| 多资产 | 打开预览（批量）/ 挂接到实体 / 打标签 / 改类型 / 导出清单 / 移除（软删） |
| 异构（资产+实体） | 资产公共子集 + 「定位选中实体」（实体 CRUD 不给——避免歧义） |
| 纯实体 | 重命名 / 编辑坐标备注 / 删除实体 |
| 任意态尾部 | 全选可见项 / 反选；有可回收条目时 + 可回收清单 |

## 3. 拖放语义（D3）

| 动作 | 语义 |
|---|---|
| 资产 → 井节点 | 未决链接 → 挂接（可撤销）；已决到别家 → 转移挂接（可撤销）；已挂到目标 → 跳过 |
| 资产 → 标签节点 | 打标签（可撤销；标签分组节点在树尾「标签 (N)」） |
| 多选拖拽 | 拖任一选中项带全部选中（mime = `application/x-paleo-asset-ids`，分号串） |
| 外部文件/夹 → 树 | >20 文件或目录 → 预估+分批确认（D8.6）→ `externalImportRequested(paths)` |
| 非法目标（分类/资产/测区/测线节点） | dragMove/drop 拒绝 + 红线 + viewport tooltip 原因（D3.6） |
| Esc / 拖出窗口 | Qt 取消语义 + `dragLeave` 清全部反馈（D3.8） |

**测试驱动注意**：`QApplication::notify` 会吞掉无 QDrag 会话的合成
DragMove/Drop——测试经 `DataNavTree::handleDrop/handleDragMove/handleDragLeave`
直调（生产路径不受影响）。

## 4. 批量操作（D1.4–D1.9）

- **挂接**：`applyEntityDrop(ids, entityId)`——`EntityPickerDialog` 选目标
  （井列表可搜索）；逐资产按链接态分派挂接/转移/跳过，状态栏汇总 toast。
- **改类型**（D1.5）：`BatchTypeDialog` 选词表（库内类型 ∪ 常用类型）→
  写 `.paleo/asset_overrides.json`（视图层改写，可撤销，catalog 原型不动）；
  sidecar 写失败走失败明细对话框。
- **移除**（D1.6）：软删进 `.paleo/recycle_bin.json`；确认对话框说明
  「软删可恢复可撤销」；`RecycleBinDialog` 恢复选中/全部恢复/清空记录
  （清空不可撤销，二次确认——D5.5）。
- **导出**（D1.7）：CSV/JSON 双格式，字段含路径/类型/版本/归属/标签/大小/
  时间；文件对话框选路径。
- **打开预览**（D1.8）：`batchOpenPreviewPlan`——前 8 项逐个
  `assetActivated`，超出状态栏提示。
- **全选/反选/按过滤器选中**（D1.9）：`selectAllVisibleAssets` /
  `invertAssetSelection` / `selectByCurrentFilter`（= 全选可见）。
- **选择保持**（D1.10）：`SelectionKeeper` 在 `refreshAssetTable` 前
  快照 id 集，重建后表（QSignalBlocker 静默）+ 树还原。

## 5. 实体操作（D4.6）

- 新建：`EntityCreateDialog`（类型/名称/井坐标）→ 重名校验 →
  `EntityCreateCmd`（addEntity；**撤销 = sidecar 软删**——catalog 无
  removeEntity API，见 GAPS.md）。
- 删除：`EntityDeleteDialog` 二选一——「保留资产（逐条 DetachLinkCmd
  解挂）」或「资产一并软删」；实体本身软删（可恢复）。
- 改名/坐标/备注（D4.1/D4.2）：树内 F2 或右键 → `EntityEditDialog`
  （校验非空/非法字符/重名）→ `EntityEditCmd` 写
  `.paleo/entity_overrides.json`（可撤销）。
- 角色编辑（D4.7）：属性面板角色表双击或右键 → `RoleEditDialog`（词表
  = roleRegistry.forEntity）——**不可撤销**（addLink 无删除对偶，确认
  对话框明示，D5.5 的正用例）。

## 6. 视图形态（D7）

五态：树形 / 列表（`assetTable`，兼容面冻结）/ 图标（IconMode）/
高速（`FlatAssetModel`，canFetchMore/fetchMore 每批 256，10k 行单批
<16ms）/ 分组（按类型/实体/标签/版本）。列配置（显隐/顺序）持久化到
QSettings `dataops/cols/*`；列头右键 = excel 风漏斗（D7.8）。

## 7. 布局契约（D9）

分栏 `dataListPreviewSplit` 宽度只允许三种变化：用户拖手柄、预览最大化
钮、窗口 resize。P3 新增代码**零 `setSizes` 调用**——
`dataops_d10_layerMarkersAndNoProgrammaticSplitterResize` 源码扫描 +
tst_ui 三条宽度回归用例钉死。新部件经「工具行拆两行 + FlowLayout +
两行过滤条」把列表最小宽压到 263px（≤ 用户常用 275px）。
