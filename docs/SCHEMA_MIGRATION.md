# Schema 迁移与并发策略（五类工程存储）

状态：**现行规范**（wave3/model-hardening；对应 TODOS P1「schema 迁移策略」与
「两实例同开一工程的并发写」）。本文所有行为都有代码指针与测试背书；改动
本文件描述的行为必须同步改测试。

## 0. 总表

| 存储 | 版本机制 | 当前版本 | 读到旧版本 | 写新版本时机 | 迁移失败行为 |
|---|---|---|---|---|---|
| `artifacts/metadata/catalog.json` | JSON 根键 `schema_version`（`CatalogStore::kJsonSchemaVersion`） | **1** | 缺键=当前版本；`1` 放行 | 不再是活库：只做迁入与 `CatalogStore::toJson` / `exportCatalogJson` 导出（§1、§8） | 值≠1 → 拒开迁入（原件保留，不建 sqlite） |
| `artifacts/metadata/catalog.sqlite` | `catalog_meta.schema_epoch` = `CatalogStore::kSchemaEpoch`（**不是** `MetaStore::kUserVersion`） | **1** | 缺表/缺键 = 1 | 建库与每次写 meta 时写当前 epoch | epoch 更高 → 拒开，不回退 `.bak`。`PRAGMA user_version` 高于本构建 → 拒开，错误含子串 `newer than this build`，文件不动 |
| `metadata/project.sqlite` | `PRAGMA user_version`（`MetaStore`） | **2** | `0`/`1` → 就地采纳为 2；`fault_set` 缺 `surface` 列 → `ALTER TABLE ADD COLUMN`（旧行 NULL，棒/切割仍在 payload） | 打开连接后、任何建表前（补列在 FaultSetStore::open） | 更高版本 → store 全部拒开（零写入，文件不动） |
| `project.gpkg` | GeoPackage 规范占用 `application_id`/`user_version`，Paleo **不用**它们；升级走加列/加表（OGR） | —（无 Paleo 版本号） | OGR 自然向前兼容（缺表 `CREATE IF NOT EXISTS`、缺列 `ensureField`） | 第一次破坏性变更时引入 `paleo_meta` 表（见 §3） | OGR 错误面如实上抛；工程级一致性由 catalog.sqlite（§8）+ 写锁兜底 |
| `*.qgz` | QGIS 自带工程版本属性 | —（交给 QGIS） | QGIS 原生读旧工程 | 每次 `QgsProject::write()` | 写失败不破坏 gpkg 权威数据态；`.qgz.bak` + 临时文件+rename |

版本递增规则（所有存储统一）：**只向前一小步、永不降级**。新版本号发布
当且仅当伴随一段迁移代码；本构建不认识更高版本时一律拒开而不是猜。

## 1. catalog.json（迁入与导出；活库见 §8）

活库是独立的 `artifacts/metadata/catalog.sqlite`（§8），不并入
`project.sqlite`。`catalog.json` 只做两件事：迁入（成功后改名为
`catalog.json.migrated`）和导出（`CatalogStore::toJson` /
`exportCatalogJson`）。sqlite 已在时 json 原样留着。下列读旧/拒开规则只约束
这份 JSON，不描述活库。

- 序列化面：`src/catalog/datacatalog.cpp`（`kSchemaVersion = 1`）。
- 读旧兼容：
  - 根键 `schema_version` **缺键视为当前版本**（手写/早期文件）；
  - 值为当前版本 → 放行；
  - 其他值 → `open()` 失败（错误点名文件），此后 `refusesWrites()` 为真，
    所有 mutator 如实失败——**绝不让空 catalog 覆盖坏文件**；
  - 装载期坏段（手改出的 `..` 受管路径等）逐段跳过并 qWarning，不整卷拒开。
  - D12 之后旧文件里的 `uwi`/`aliases` 实体键装载时静默丢弃、不再回写。
- 写新版时机：不再每次 `save()` 重写活文件。迁入成功后改名为
  `catalog.json.migrated`；导出写 `schema_version` =
  `CatalogStore::kJsonSchemaVersion`（1）。
- 失败/回滚行为：
  - `QSaveFile` 原子替换（temp + rename，`setDirectWriteFallback(false)`）——
    写一半不落坏文件；
  - 替换前把现存 catalog 轮转一份 `catalog.json.bak`——「写成功但内容错」
    时有上一代可回退；
  - 迁移失败（拒开）不触碰文件。
- 测试背书：`tests/tst_catalog.cpp` `refusesUnsupportedSchemaVersion` /
  `rotatesBakAndVerifiesWrite` / `refusesWritesAfterFailedOpen` /
  `unsafeManagedPathSkippedOnLoad` / `legacyCatalogFieldsIgnoredAndNotRewritten`。

## 2. metadata/project.sqlite（声明清单 + 版本状态机 + 发布台账）

- 共享同一 sqlite 文件的三个 store（各自命名连接，惰性打开）：
  - `LayerManifest`（`layer_declarations`，src/metadata/layermanifest.cpp）
  - `MapVersionStore`（`map_versions` / `map_products`，src/metadata/mapversionstore.cpp）
  - `ReleaseStore`（`releases`，src/metadata/releasestore.cpp）
- 版本门：`MetaStore::ensureUserVersion`（src/metadata/metastore.h/.cpp），
  在每个 store 的连接打开后、**任何建表/补列之前**执行：
  - `user_version == 0` 或 `1` → 新库或上一版本 → **就地写 2**（采纳）；
  - `== kUserVersion(2)` → 放行；
  - `> 2` → 本构建不认识 → 拒开，错误写明读到的版本号与构建支持的上限，
    此时没有发生过任何写入，**原件完整**（升级应用，而不是猜文件）。
  - `1 → 2` 的结构差只有 `fault_set.surface`（TEXT，可空）。表还不存在时由
    FaultSetStore 按新表头创建；表已在且缺列时 `ALTER TABLE ADD COLUMN`。
    旧行 `surface` 为 NULL，payload 里的棒/切割不改写。
- 列级迁移（版本内前向兼容）：`CREATE TABLE IF NOT EXISTS` + 逐列
  `PRAGMA table_info` 查缺后 `ALTER TABLE ADD COLUMN`（新列可空，旧行读默认）。
  现存实例：`layer_declarations.title`（layermanifest.cpp:73-87）、
  map_versions 的发布门列（mapversionstore.cpp `ensureColumn`）。
- 失败行为：user_version 门拒绝 → 三个 store 的 `open()` 全部 false（各自
  错误串），后续读写全部失败；文件未被修改。
- 升级流程（将来 bump 到 3 时）：改 `kUserVersion` + 在采纳分支后加一段幂等
  迁移（`2 → 3`），同提交里更新 `futureVersionRefusedByAllStores` 的造库值
  （该测试用 `kUserVersion + 1`，随常量走）。
- 当前 2：`src/metadata/faultsetstore.cpp` 补 `surface` 列。断面 mesh 不进
  payload，避免旧读者把棒文档和网格绑死。
- 测试背书：`tests/tst_metastore.cpp` `freshDbAdoptsUserVersion` /
  `legacyZeroVersionUpgradedInPlace` / `futureVersionRefusedByAllStores`
  （断言拒开后连表都没建）/ `currentVersionPassesIdempotently`。

## 3. project.gpkg（约束/成果矢量权威库）

- 现状：由 OGR/GDAL 驱动读写——`ConstraintStore`（src/io/constraintstore.cpp）
  经 `OGR_L_CreateField` 逐列补建；算法输出先写临时 gpkg 再经
  `PaleoProjectStore::enqueueWrite` 队列合并（§41.2b）；编辑提交走
  QgsVectorLayer 事务前先过同一队列（§41.2c）。
- **为什么不用 `PRAGMA user_version`**：GeoPackage 规范把
  `application_id`/`user_version` 留给 GPKG 自身版本标识（GDAL 写入），
  Paleo 占用它会和规范/OGR 工具链冲突。sqlite 那套（§2）不搬过来。
- 读旧兼容：OGR 自然向前兼容；缺表即建（`IF NOT EXISTS`）、缺列即补
  （`ensureField`），旧行新列取 NULL。
- 写新版本时机：**目前无 Paleo 级版本号**——所有演进都是加列/加表（非破坏），
  不需要。第一次破坏性变更时引入 `paleo_meta(key TEXT PRIMARY KEY, value TEXT)`
  表并写 `schema_version` 行，读门对齐 §2 的三段式（未知更高版本 → 经
  `PaleoProjectStore` 拒绝写路径）。这页更新时补实现指针。
- 失败行为：OGR 错误如实上抛给调用方（无静默降级）；工程级「谁是对的」
  由 catalog.sqlite（§8）+ 工程写锁（§6）兜底。catalog.json 只是迁入/导出。
- 测试背书：`tests/tst_constraintstore.cpp`（列补建往返）、
  `tests/tst_runtime.cpp`（队列串行无 BUSY，§33 脊线）。

## 4. .qgz（显示态投影）

- QGIS 自带工程版本属性：`QgsProject::write()` 写、QGIS 原生读旧工程。
  Paleo 不加自己的版本层——**评估结论：交给 QGIS**，Paleo 只保证写路径安全。
- 写路径（src/qgis/qgisprojectservice.cpp `writeProject`）：临时文件
  （同目录、同后缀——后缀决定 zip/xml 后端）→ `QgsProject::write(tmp)` →
  rename 回权威路径；失败删临时文件、原件不动。
- 备份：`PaleoProjectStore::saveAll`（src/metadata/paleoprojectstore.cpp）
  在 gpkg commit 之后把现存 `.qgz` 轮转 `.qgz.bak`（temp+rename），再原子写
  新 `.qgz`。次序 = gpkg commit → .qgz 备份 → .qgz 原子写。
- 语义边界（§41.2）：`.qgz` 是**可再生显示态**——层声明集的真源是
  project.sqlite 的 manifest（§37），`.qgz` 只内嵌一份投影（rehydrate 仅在
  sidecar 缺失时）。`.qgz` 损坏/丢失不破坏权威数据态。
- 测试背书：`tests/tst_manifestproj.cpp`（投影/回水）、
  `tests/tst_runtime.cpp`（写次序与备份）。

## 5. 版本化总原则

1. **长生命周期优先**：读路径必须能打开上一版本产出的文件；做不到就拒开
   并保留原件（`user_version` 更高、`schema_version` 不符两类都走这条路）。
2. **迁移只前进一小步**：每个版本号 = 一段幂等迁移；跳版本靠逐步重放，
   不写 N→N+2 特例。
3. **缺省即采纳**：无版本痕迹的文件（早期/手写）视为当前版本起始态，
   第一次写时落版本号（catalog.json 缺键=当前；sqlite `0→1`）。
4. **拒开 ≠ 报废**：错误信息写明「文件是版本 X、本构建支持到 Y」，原件
   一个字节不动。

## 6. 两实例同开一工程（并发写）

**决议：拒绝第二个写实例**（单机单地质家的桌面现实下最诚实的选择）。

评估过的三个选项：

| 选项 | 结论 |
|---|---|
| sqlite WAL | **`project.sqlite` 不采用 WAL。** 它只救 sqlite 并发读，救不了 gpkg / .qgz 的跨进程写；且把「同工程双写」从错误变成难以察觉的竞态。工程级单写者决议不变。**例外：** `artifacts/metadata/catalog.sqlite` 使用 WAL + `synchronous=FULL`，因为它是单文件、写期间仍有读者（`CatalogStore`，§8）。这不把 `project.sqlite` 改成 WAL，也不改变 gpkg / .qgz 的决议，也不开放第二个写实例。 |
| 每存储锁 | 四类存储各自加锁=四套陈旧锁恢复逻辑，且仍挡不住「gpkg 提交了、catalog.json 没提交」的跨存储半截态。**不采用**。 |
| **工程级单写者（采用）** | 一个工程目录同时至多一个写实例；第二个实例尝试作为写实例打开 → 明确拒绝并告诉用户谁在编辑。锁住的是「写权威」，只读浏览不受影响。 |

机制：`ProjectDirLock`（src/metadata/projectlock.h/.cpp）——Qt `QLockFile`
适配锁落在 `<projectDir>/artifacts/metadata/.project.lock`：

- 原子创建；`staleLockTime(0)` + 持有者 pid 校验：持有进程已死（崩溃/被杀）
  时锁自动可回收，**不留死锁**；
- 拒绝时读出持有者 `pid@hostname, appname` 写进错误文案；
- 进程内多工程目录互不影响；显式 `unlock()` / 析构释放。

进程内并发（不需要锁的部分）已经由既有机制覆盖：
`PaleoProjectStore` 是唯一写汇聚点（进程级互斥，§41.2），`DataCatalog`
所有落盘在主线程串行。跨进程剩下的口子就是「第二个实例」，由本锁关闭。
catalog.sqlite 的 WAL 不改变这条：仍是单写者，变更集单事务提交；
`synchronous=FULL`，禁止 `synchronous=OFF`。

**接线点（一行，集成时落地）**：`AppContext::projectOpened` 绑定写路径处
（src/app/appcontext.cpp `setProjectPaths` 调用旁）`ProjectDirLock::tryLock`，
失败走现有错误面（状态栏/消息区）拒绝作为写实例打开。本包未直接改
appcontext.cpp：该文件同时被并行 UX 包与编排会话修改，接线留给集成者，
语义与错误文案以本节为准。

测试背书：`tests/tst_metastore.cpp` `projectLockSecondInstanceRefused`
（第二锁拒绝 + 错误带 pid）、`projectLockUnlockAndIndependence`
（解锁重取、跨目录独立）。

## 7. 已知缺口（下次评审项）

- project.gpkg 尚无 Paleo 级版本号（§3 的 `paleo_meta` 表在第一次破坏性
  变更时引入）；在此之前 gpkg 演进只允许加列/加表。
- `project.sqlite` 的 QSQLITE 连接未显式设 journal 模式（默认 journal）；单写者决议下无正确性
  影响，若将来允许只读多开再评估 WAL。catalog.sqlite 是已落地的例外（§6、§8）：WAL + `synchronous=FULL`，不外推到 project.sqlite。
- ~~`ProjectDirLock` 的 AppContext 接线未落地~~（已收口：appcontext.cpp
  建锁+`tryLock`、失败降级只读，qgisprojectservice.cpp 亦有检查）。

## 8. artifacts/metadata/catalog.sqlite（第五个库）

与 `metadata/project.sqlite` 不是同一个文件，也不走 `MetaStore::kUserVersion`
做 catalog 的 schema 代际。代码：`src/catalog/catalogstore.h`（`kSchemaEpoch`）
/ `src/catalog/catalogstore.cpp`（`kSchemaSql`、`applyWritablePragmas`）。

- 路径：`<projectDir>/artifacts/metadata/catalog.sqlite`。
- 查询事实源仍是内存里的四张表（`DataCatalog`）。本库只负责落盘。
  变更集单事务提交。
- **四表，无外键，无 BLOB**：`entities`、`entity_asset_links`、`assets`、
  `versions`。角点、extra、父版本列表用 TEXT 装 JSON，不进 BLOB。
  另有 `catalog_meta(key TEXT PRIMARY KEY, value TEXT)` 键值表。
  `schema_epoch` 行 = `CatalogStore::kSchemaEpoch`（**1**）。同表还存
  `catalog_revision` / `mutation_seq` / `asset_seq` / `version_seq` /
  `backup_keep`，这些不是版本门。
- 缺 `catalog_meta` 或缺 `schema_epoch` 视为 1（与 JSON 缺
  `schema_version` 同口径）。
- epoch 高于本构建 → 拒开，错误含 `unsupported catalog schema`。
  **不回退 `.bak`**（那是降级，不是恢复），原件一个字节不动。
- `PRAGMA user_version` 高于本构建同样拒开，错误含子串
  `newer than this build`（连接经 `MetaStore::openConnection` 打出这句；
  catalog 的代际仍是 `catalog_meta.schema_epoch`，不是
  `MetaStore::kUserVersion`）。同样不回退 `.bak`，文件不动。
- 日志：`PRAGMA journal_mode = WAL`，`PRAGMA synchronous = FULL`。
  只这一库：单文件、写期间仍有读者。`project.sqlite` 维持 §6，不设 WAL；
  gpkg / .qgz 的单写者决议不变。
- `catalog.json` 只做迁入和导出（`CatalogStore::toJson` /
  `exportCatalogJson`），不是活库。只有 json 时解析后单事务导入，成功则改名为
  `catalog.json.migrated`。sqlite 已在：它是权威，json 原样留着。
- 备份在 **open** 时做，不在每次 save 时做。`integrity_check` 通过之后才把
  当前好库整文件拷进 `catalog.sqlite.bak` / `.bak.N`（保留代数与 JSON 时代
  相同）。已损坏的主文件不进 `.bak` 链。
- 测试背书：`tests/tst_catalogstore.cpp`
  `futureUserVersionRefusesWithoutBak`（断言错误含子串
  `newer than this build`，且 `.bak` 大小不变）/
  `futureSchemaEpochRefusesWithoutBak`。

