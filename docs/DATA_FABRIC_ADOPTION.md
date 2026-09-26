# DATA_FABRIC_ADOPTION.md — 数据管理/流转采纳计划

参考：`/home/kevin/projects/paleo_project`（data-fabric-v11 目标架构 +
`libs/catalog` / `libs/ingest` / `libs/data_suite` / `libs/project` C++ 契约）。
**只采纳数据管理与流转语义，不采纳其 UI。**

核心约束（用户指令）：**一切数据管理行为必须锚在工程生命周期上**——
无工程不导入、不落盘、不登记；catalog 变更与 `.qgz` 保存经同一写队列排序。

## 对方语义 → 我方落点

| 对方件 | 我方现状 | 采纳 | 状态（2026-09-27） |
|---|---|---|---|
| RoleRegistry（角色词表唯一权威） | `EntityAssetLink.role` 自由字符串 | **A 包**：`src/catalog/roleregistry.*` | ✅ 已合并 `d2bcaf1`（`roleRegistry()`，工程可覆盖） |
| IngestPlan 三段式（plan→confirm→execute） | folderRows+确认框，plan 内嵌执行 | **C 包**：`src/io/ingestplan.*` 拆分 | ✅ 已合并 `6d1f3cb`（`buildIngestPlan`/`executeIngestPlan`） |
| link.ordinal + primary 不变量 | 无 ordinal | **B 包**：catalog 字段+排序 | ✅ 已合并 `6e34d9d` |
| EntityDataView（角色槽视图） | `linksForEntity` 原始面 | **B 包**：`src/catalog/entityview.*` | ✅ 已合并 `6e34d9d`；⏳ DataPage 接线=wave-5 p5a |
| staleness/downstream 闭包 | 无 | **B 包**：catalog 查询+extra 标记 | ✅ 已合并 `6e34d9d`；⏳ 预览/发布门接线=wave-5 p5b |
| CommitCoordinator（journal+幂等+三段提交） | `PaleoProjectStore::saveAll` 已有序写队列 | **D 包**：journal-lite+幂等 op | ✅ 已合并 `12aa9b1`（`commitAll`/`recoverCommitJournal`）；⏳ 生产调用点待接线（见下） |
| role 词表强制 | 无 | wave-5 p5c：诊断不硬拦 | ⏳ 外包中 |
| working-copy / trash / typed RunPort | 无 | **defer**（规模不符，派生链短） | 递延 |

## A 包 — RoleRegistry（`data/role-registry`）

工程作用域的角色词表：

```cpp
struct RoleDef {
  QString role;            // well_head | well_log | trajectory | tops |
                           // time_depth | seismic_volume | horizon | …
  QStringList entityTypes; // 允许挂接的实体类型
  int maxCount = 0;        // 0 = 不限（0..N）；1 = 单成员（0..1）
  bool ordered = false;    // 同角色多成员需 ordinal 排序
  QString display;         // UI 显示名（中文）
  QString stageDefault;    // 建议阶段（RAW/DERIVED/…）
  QString description;
};

class RoleRegistry {
public:
  static RoleRegistry defaults();                    // 内置词表（不读盘）
  static RoleRegistry fromJson(const QJsonObject&);  // 工程自定义覆盖
  const RoleDef* find(const QString &role) const;
  QVector<RoleDef> forEntity(const QString &entityType) const;
  bool isKnown(const QString &role) const;
};
```

工程集成：`DataCatalog::open()` 时从 `project_area.json` 的 `roles` 节装载
（缺省 → `defaults()`），`catalog.roleRegistry()` 暴露。不同工区可有不同词表。
**只新增文件 + datacatalog 末尾追加成员/装载段，不改既有方法签名。**

## C 包 — IngestPlan 三段式（`data/ingest-plan`）

把 `importFolder` 的「扫描+确认+执行」显式分层：

```cpp
struct PlannedItem {
  QString path, type, format;
  qint64 size = 0;
  QString sha256;              // 小文件算，SEG-Y 大文件跳过 hash
  QString entityType, entityId, entityName;  // 身份匹配结果
  bool entityAmbiguous = false;              // 双候选 → unresolved，不猜
  QString role;                // 推断角色
  bool suggestedPrimary = true;              // 同 (entity,role) 首成员
  QString duplicateOfVersionId;              // plan 期 sha 去重命中
  QString decision;            // accept | skip | as_new_version（确认框可改）
  QString note;
};

struct IngestPlan {
  QVector<PlannedItem> items;
  QStringList issues;          // 不可读文件、未知类型等
  int unresolvedCount() const;
  int duplicateCount() const;
};

IngestPlan buildIngestPlan(const QString &root, const DataCatalog &catalog);
// 纯函数：扫描/分类/匹配/去重，不落盘不改 catalog

ImportResult executeIngestPlan(const IngestPlan &plan, DataImportService &svc,
                               ProgressCb cb);  // 幂等：path+sha 已注册即跳过
```

工程集成：
- `duplicate_of` / `as_new_version` 决策落版本链（`parentVersionIds` 维持现状）
- 已注册判定 = `catalog.versionBySha256(sha)` 命中 → plan 期 decision=skip（可改）
- shp 族归组：`.shp/.shx/.dbf/.prj` 同主名 → 单个 PlannedItem（members 列表），
  导入时复制全组、sha 聚合
- execute 走现有 catInvoke marshal + BatchSave + 协作取消；重跑天然幂等

**注意**：wave-4 `p4a-runtime-resilience`（外链重定位 API）也在
`dataimportservice.cpp` 文件末尾追加——合并接缝，编排者已知晓。

## B 包 — 实体视图 + ordinal + staleness-lite（`data/entity-view`）

**依赖 A 包的 `RoleRegistry`**（角色槽枚举用），A 落地后启动。

- `EntityAssetLink` 加 `int ordinal = 0`；同 （实体，角色） 成员按 ordinal 排序展示
- `EntityDataView` 纯查询 facade：
  ```cpp
  struct RoleSlot { RoleDef def; const EntityAssetLink *primary;
                    QVector<const EntityAssetLink*> members;
                    QVector<const EntityAssetLink*> unresolved; };
  struct EntityView { CatalogEntity entity; QVector<RoleSlot> slots;
                      QVector<CatalogVersion> derivedProducts;
                      QStringList missingSources; };  // 下游引用了不存在的版本
  EntityView entityDataView(const DataCatalog&, const QString &entityId);
  ```
- `downstreamClosure(versionId)`：`parentVersionIds` 反查全闭包
- 重新派生/外链 sha 失配时：下游版本 `extra["stale"]=true` + `extra["staleReason"]`
- 发布门（PublishGate）补一条：stale 下游存在 → 提示（不阻断，只是 advisory）

## D 包 — commit-coord-lite（`data/commit-coord`）

对齐对方 CommitCoordinator 的**语义**（journal + 幂等 + 有序提交），规模裁剪：

- `PaleoProjectStore` 加 `commitJournal/` 目录语义：每次 `saveAll` 的写单元
  带 `opId`（内容 hash）+ 阶段记录；重跑同 opId → 跳过已完成的阶段
- 崩溃恢复：`openProject` 时扫描 journal——`staged`/`catalog_done` 未
  `complete` 的 op 记日志 + 状态栏提示（不自动重放，honest failure）
- `.qgz` 与 catalog 的写序：**catalog 先落盘，qgz 后写**（saveAll 现有顺序
  保持），保证「catalog 说已存 ⇒ 工程文件已写」的方向性
- 不做：乐观锁 base_version（单用户单工程不需要）、异步写（saveAll 已串行）

## 合并序

A（独立新增）→ C（dataimportservice 接缝）→ D（paleoprojectstore 独立）
→ B（依赖 A 的 registry）。CMakeLists.txt 每包末尾追加连续块，合并冲突仅限
该文件与 `.goal-loop-ledger.md`，编排者处理。

## 通用验证

每包：新增 QtTest 用例 + `ctest` 全绿（基线 61/61 + 新增数）。
真数据 smoke（`tst_import`/`tst_smoke_realdata`）在 C 包必跑。

## 合并后接缝（本会话追加，2026-09-27）

四包落地后仍开着的接线口——下一波（wave-5 已在跑 p5a/b/c）之外的显式缝：

- **`commitAll` 尚无生产调用方**。保存按钮走 `store->saveAll(gpkgCommit,
  writeQgz)`（`paleomainwindow.cpp` save 连接，不 journal）。接线方案：
  saveAll 内部改走 commitAll（opId 每次保存生成，complete 记档靠
  `pruneCommitJournal` 封顶）；或把 catalog 提交并进 saveAll 的单元链。
  journal 阶段名按单元语义映射（unit1=gpkgCommit 记 catalog_done 是
  「第一存储单元完成」的位置语义——接线时若嫌误导可参数化阶段名）。
  **已落地辅助**：`pruneCommitJournal()`（complete 记档封顶，未完成/损坏
  永不删）+ `projectOpened` 里顺带调用——接线前记档只增不减的债先封。
- **`EntityView`/`downstreamClosure` 已进 `ProjectDataFacade`**：
  `entityView(entityId)` / `downstreamClosureOf(versionId)` 直通
  （catalog 未开 → 如实空）。DataPage（p5a）与派生链（p5b）可以消费
  facade 而不直接摸 catalog。
- **放弃项（如实记录）**：曾考虑给 `buildIngestPlan` 加「推断角色↔词表」
  note 标记——`roleForType` 只产词表内角色、`project_area.json` 的 roles
  覆盖是增量补丁不能删内置，标记路径不可达 = 死代码，不做。
