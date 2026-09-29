# 数据管理页过滤器（P3 交付文档）

日期：2026-09-29。分支：`wave/data-page-operations`。
实现：`src/ui/pages/dataops/dataopsfilter.h`（纯逻辑）+
`dataopswidgets.h`（FilterBar/FilterChipBar/PendingQuickBar/TagCloud/
FilterEmptyState）。

## 1. 维度（D2.1）

| 维度 | 取值 | 匹配语义 |
|---|---|---|
| 搜索词 `q` | 自由文本 | 名称/类型/关联实体/文件名 contains（大小写不敏感） |
| 类型 `type` | 词表（库内类型 ∪ 改写后） | effectiveType 精确（含用户改写） |
| 状态 `status` | RAW / DERIVED | 当前版本 stage |
| 角色 `role` | 链接角色词表 | 任一链接角色命中 |
| 实体 `entity` | 已决实体名（含改名） | 归属实体命中 |
| 标签 `tag` | 用户标签 | tags 命中 |
| 文件名正则 `re` | QRegularExpression | 文件名/显示名 match；**无效模式退字面量**（不炸） |
| 未挂接 `unlinked` | 开关 | 无已决实体（未决链接也算未挂接——如实口径） |
| 类型未知 `unknownType` | 开关 | 类型空或 unknown |
| 有警告 `warned` | 开关 | 存在未决链接 |

兼容面：既有 `assetSearchEdit`/`assetTypeFilter` 是 Search/Type 两个维度
的常驻输入口（`syncLegacyControlsIntoFilter` 每次应用前以控件值替换这两
维——旧测试与旧肌肉记忆全兼容）。

## 2. 组合（D2.2）

`FilterGroup`：条件向量 + AND/OR 模式（`filterModeButton` 切换）。
chip 行逐条显示；**点击 chip 删除**、**右键 chip 取反**（`非 状态: DERIVED`）；
「清空全部」一键。AND 语义下多维过滤与标签云激活标签（D2.4）取交集。

## 3. 未决项视图（D2.3）

`PendingQuickBar` 三个开关钮（`quickUnlinked`/`quickUnknownType`/
`quickWarned`）带**实时计数徽标**（「未挂接 (2)」），点击即过滤。
与 T31「只显示未决资产」条（`unresolvedFilterBar`）并存：后者是资产表
重建级过滤（沿用旧管线），前者是行快照级过滤（新管线）。

## 4. 标签体系（D2.4）

- 存储：`.paleo/asset_tags.json`（`{tags: {assetId: [标签]}}`）——视图层
  sidecar（catalog 无标签 API，见 GAPS.md）。
- 打标签：右键「打标签」/ 标签节点拖放 / `batchAddTag`；40 字上限、
  去重不区分大小写。
- 标签云：使用计数降序；点击 = 按 该标签过滤（chip-active 样式），
  再点取消。
- 树尾「标签 (N)」分组：每个标签一个叶节点（拖放目标 + 组织面）。

## 5. 树排序记忆（D2.5）

`treeSortCombo`：名称（自然序）/时间/类型/大小——选择写 QSettings
`dataops/treeSort`，下次会话恢复（`recalledTreeSort`）。

## 6. 保存的过滤器（D2.6）

QSettings `dataops/filterPresets`：名称 → 状态串。FilterBar 的
「存为预设」+ 预设下拉应用；40 字名称上限。

## 7. 命中高亮（D2.7）

`HighlightDelegate`（assetTable）：搜索词命中区间 primary 蓝加粗重绘
（基类先画底/选中态，主题一致）；`matchRanges` 给区间（测试断言面）。

## 8. 性能（D2.8）

行快照（`AssetRowInfo`）一次装配（含 stat 大小/mtime），过滤循环零
catalog 回查、零 IO。10k 资产 × 3 条件断言 < 100ms
（`dataops_d2_filterPerformance10k`，实测毫秒级）。

## 9. 空结果态（D2.9）

过滤中零命中 → `FilterEmptyState`：原因行（当前 N 个条件 + 标签）+
「放宽条件」（逐级去掉最后一个条件，旧控件同步清空防回填）+
「清除全部过滤」。空态指引行（§42.4）不受影响。

## 10. 状态串（D2.10）

`paleo://dataops-filter?op=or&q=...&type=well_log&tag=!核心`
（`!` 前缀 = 取反；URL 编码）。`toStateString`/`fromStateString` 往返；
「复制状态串」进剪贴板，粘贴 `setFilterFromStateString` 恢复——坏串
回空组不炸。tst_ui 用状态串驱动 D9.3 宽度回归。
