# 数据列表模型关系图（P3 Phase 0 产出）

日期：2026-09-29。分支 `wave/data-page-operations`。

## 1. 基线（改造前）

```
DataCatalog (catalog/datacatalog.h, 数据层)
  assets() / entities() / links() / versionsForAsset() / currentVersion()
        ▲ 只读
        │
PreviewDocService (services/previewdoc.h) —— 数据页唯一门面
  catalog() / absolutePathForVersion() / assetIds() / assetSource()
        ▲ 动态属性 "paleo.page.importsvc" 下发
        │
DataPage (pages/datapage.{h,cpp}, 兼容薄壳)
  ├─ DataListPanel (pages/datalist.{h,cpp})
  │    ├─ dataTree   QTreeWidget   —— 语义树：测区/井/测井/地震/层位/辅助
  │    │    节点角色数据 UserRole: assetId | +1 wellId | +2 nodeType | +3 lineMode
  │    └─ assetTable QTableWidget  —— 平表：名称/类型/关联(控件列)
  │         行身份: item(0)->data(Qt::UserRole) = assetId
  │         过滤: setRowHidden（applyListFilter）+ 未决过滤(重建时剔除)
  └─ EntityPanel (pages/entitypanel.{h,cpp})
       entityRoleTable / derivedProductsTable —— 每次全量重建
       上下文: setContext(entityId, assetId)（壳经 selectAsset/
               selectAssetsForEntities 驱动）
```

没有 QAbstractItemModel 子类、没有代理/过滤模型——树与表是**widget 持
item** 的直构形态，过滤 = setRowHidden/setHidden，刷新 = clear+重建。

## 2. 改造后（本波新增，src/ui/pages/dataops/）

```
DataListPanel
  ├─ dataTree (DataNavTree : QTreeWidget)   ← 启用拖放：资产→实体=挂接、
  │     │                                      →标签节点=打标签（D3.1/3.5/3.7）
  │     └─ 高亮委托 HighlightDelegate          ← D2.7 搜索命中高亮
  ├─ assetTable (QTableWidget, 兼容面冻结)     ← ExtendedSelection + 列配置
  │                                            + 列头漏斗（D7.8）+ 排序（D7.4）
  ├─ assetIconView (QListWidget, IconMode)     ← D7.1 图标态
  ├─ assetVirtualTable (QTableView)            ← D7.3 10k 虚拟滚动
  │     └─ FlatAssetModel : QAbstractTableModel ← canFetchMore/fetchMore 批量
  ├─ assetGroupTree (QTreeWidget)              ← D7.6 分组平表
  ├─ 过滤条 FilterBar（D2.1/2.2/2.3/2.9/2.10）
  │     └─ FilterState（条件/组 AND-OR/chip/URL 串/预设）
  ├─ TagCloud（D2.4）→ TagStore（sidecar .paleo/asset_tags.json）
  └─ DataOpsUndoStack（D5，命令对象 redo/undo/text/mergeWith，深 50）
        │  Attach/Detach/Primary/RoleChange/Tag/TypeOverride/
        │  SoftDelete/Restore/EntityCreate/RenameOverride 命令
        └─ 回放全部经 DataCatalog 既有 mutator + sidecar store

EntityPanel
  ├─ 五折叠组（基线保留）+ 版本时间线 VersionTimeline（D4.3）
  ├─ 版本 diff 摘要（D4.4，清单级）
  ├─ 拓扑小图 TopologyGraph : QGraphicsView（D4.5）
  ├─ 实体 CRUD 工具行（D4.6：新建=addEntity；删除/改名=回收站+override）
  ├─ 角色编辑（D4.7：setLinkUnresolved+addLink 组合）
  ├─ 多选批量概要态（D4.9）
  └─ 最近操作 OperationsHistory（D4.10，会话内）

DataPage（薄壳扩展）
  ├─ 命令面板 DataCommandPalette（D6.1，Ctrl+K）
  ├─ 命令注册表 CommandRegistry（D6.2，面板/快捷键表/菜单共用）
  ├─ 快捷键表 ShortcutsDialog（D6.3）+ 冲突检测（D6.6）+ Vim 可选（D6.5）
  └─ 导入队列 ImportQueuePanel（D8.1/8.2/8.5）+ 预设/查重/预估（D8.3/8.4/8.6）
```

## 3. 选中模型（D1）

- `dataTree`/`assetTable`/`assetIconView` 统一 `ExtendedSelection`
  （Ctrl/Shift/框选，跨类型混合选中）。
- 选中身份 = assetId 集合（`SelectedAssets`：从三个视图聚合，刷新前
  快照、重建后按 id 还原，D1.10）。
- 批量操作按「资产/实体混合选中」分流（D1.3）：公共子集 = 资产类操作
  （预览/导出/打标签/改型/移除/挂接）；纯实体选中给实体操作。
- 表 ↔ 树选中互镜像（选中同步到另一视图的对应项，单向避免回环）。

## 4. 刷新时序（不变量）

refreshAssetTable() 的清控件→重建→过滤→计数管线保持；新增：
重建前 `SelectionKeeper` 快照选中 id；重建后还原 + 重算选中计数徽标。
分栏宽度与该管线完全解耦（D9.1：新代码零 setSizes）。

## 5. 持久化面（视图层 sidecar，全部 .paleo/ 下）

| 文件 | 内容 | 既有/新增 |
|---|---|---|
| `.paleo/undo_stack.json` | T28 挂接 undo vault + note 记忆 | 既有（datalist.cpp） |
| `.paleo/asset_tags.json` | D2.4 用户标签 {assetId: [tag…]} | 新增 |
| `.paleo/asset_overrides.json` | D1.5 类型改写 {assetId: type} | 新增 |
| `.paleo/entity_overrides.json` | D4.1/D4.2 实体名/坐标/备注 override | 新增 |
| `.paleo/recycle_bin.json` | D1.6 软删清单（id/类型/时间/原路径） | 新增 |
| QSettings `paleo/paleo` | D2.5 树排序记忆、D2.6 过滤器预设、D6.5 Vim 开关、D7.2 列配置 | 新增（键前缀 `dataops/`） |
