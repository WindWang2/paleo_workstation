# Goal-Loop Ledger — 方向 99：catalog 双巨兽解体

- 分支：`goal/catalog-split-20261010`（基线 origin/master `f474069b`，任务账面 32851a1e 已被 #318 前移，以 worktree 实际基线为准）
- Oracle：①公共区 diff 零变更+消费方零改动 ②主文件 ≤900/新 TU ≤700 ③tst_catalog 零改动×2+全量 ctest×2 红集合 diff 空 ④catalog_open 门无 >10% 劣化 ⑤schema_epoch 不动+事务语义零改动 ⑥layering 三档绿+无新警告+diff --check 干净

## R0 结构勘察（2026-10-10，行号 = 本分支口径）

### datacatalog.cpp 1,795 行 → 查询族盘点

| 区段 | 行 | 内容 |
|---|---|---|
| 匿名 ns 助手 | 28-105 | kExtraHistoryKeep（→_versions）、scanManagedSeqFloor（→主）、roleDiagnosis/annotateRoleDiagnostics/kUnknownRoleMark/kRoleTypeMismatchMark（→_links） |
| 生命周期+线程纪律+staging/journal | 107-489 | ctor/ensureOpen/checkWriteThread/noteRead/recordOp/createStagingCopy/versionFilePath/applyJournal/prepareOpen/adoptPrepared/open |
| 落盘面 | 491-748 | exportCatalogJson/save/commitStore/beginBatch/endBatch/BatchSave 四件套 |
| 实体族 | 750-775, 1463-1487, 1678-1706, 1738-1746 | addEntity/hasEntity/entities/entityById/normalizeWellName/wellsMatchingName/nextEntityId |
| 资产族 | 777-809, 1181-1254, 1489-1500, 1718-1726 | addAsset/assets/assetById/removeAsset(s)/nextAssetId |
| 链接族 | 903-1178, 1256-1267, 1531-1577 | addLink/attachLink/setLinkUnresolved/setLinkPrimary/invalidRoleLinks/linksForEntity/unresolvedLinks/linksForAsset/links |
| 版本族 | 811-901, 958-1027, 1269-1365, 1367-1461, 1502-1529, 1728-1736 | addVersion/updateVersionExtra×2/versionBySha256/removeStaleVersions/updateAssetExtra/resolvedVersionPath/sha256FileHex/verifyExternalVersionSha/isSafePathSegment/versionsForAsset/versionById/currentVersion/nextVersionId |
| 派生族 | 1579-1676 | downstreamClosure/markStaleDownstreamOf/markDownstreamStale |
| 导出族 | 1748-1851 | writeSurveyGeoJson/writeWellsGeoJson/entityCountsByType/indexHealthy |

**切分线定案（7 TU）**：
- `datacatalog.cpp`（主，留：生命周期/open/adopt/线程纪律/staging+journal/落盘面 commitStore+BatchSave/entityCountsByType/indexHealthy，估 ~640 行）
- `datacatalog_entities.cpp`（~130）
- `datacatalog_assets.cpp`（~170）
- `datacatalog_links.cpp`（含 role 诊断匿名 ns，~390）
- `datacatalog_versions.cpp`（含 kExtraHistoryKeep + 静态路径/sha 助手，~470）
- `datacatalog_derivation.cpp`（~120）
- `datacatalog_export.cpp`（exportCatalogJson/writeSurveyGeoJson/writeWellsGeoJson，~150）

**internal.h 定案：不需要。** 全部跨族共享面都是 DataCatalog 私有成员（头文件已声明）；匿名 ns 助手均为单 TU 使用（R0 逐一核对：role 助手仅 _links；kExtraHistoryKeep 仅 _versions；scanManagedSeqFloor 仅主；g_threadViolations 仅主）。少一个内部头 = 少一份 API 面。

### realization 边界（R0 结论）

realization 集合查询已独立成文件：`realizationset.h`(141)/`realizationset.cpp`(191)，布线 extra-realization.cmake——**不经 DataCatalog 门面**。任务建议的 `_realization` TU 不需要；边界保持现状。

### 词表面 R0 结论（目标 5 定案）

1. `catalogRoles()` 全仓 0 命中——方向 92 的 RoleRegistry 扩展尚未合入本分支基线；本方向不造、不等（谁先合谁为准）。
2. 角色权威 = `RoleRegistry::defaults()` + `DataCatalog::roleRegistry()`（open() 时 project_area.json roles 节覆盖）；消费 5 文件。
3. **头注释漂移实锤**：`datacatalog.h:59`（EntityAssetLink.role 注释）枚举 `well_head|well_log|tops|time_depth|horizon|seismic_volume|reference` 7 角色，其中 `reference` 已不在 defaults()；实际 defaults() 共 20 角色（well×11：+trajectory/core/cuttings/lab_analysis/interpretation/qc/other；seismic_survey×7：+geometry/velocity/fault/interpretation/other；planned×2：siting_note/other）。处置：注释改为指向 RoleRegistry 权威，不再内联枚举（注释级改动，公共 API 零变更）。

### catalogstore.cpp 1,605 行 → SQL 域盘点

| 区段 | 行 | 内容 |
|---|---|---|
| 匿名 ns 助手 | 22-595 | kCatalogUserVersion/catalogConnectionName/textArg/execSql/mapToJson(FromJson)/corners*/parents*/unsafeVersionReason/absorbSeqs/kSchemaSql(DDL)/isSchemaReject/forgetConnection/execScript/applyWritablePragmas/readEpoch/integrityOk/tableExists/absorbMetaRow/loadTables/rotateFiles/removeSqliteFamily/JsonRead+readJsonFile/migratedDest/renameAside |
| JSON 编解码 | 602-780 | sqlitePathFor/jsonPathFor/toJson/fromJson |
| 连接生命周期 | 782-866, 1061-1090, 1154-1172 | close/resumeConnection/begin/commit/rollback/attachWritable/connectPrimary |
| CRUD | 868-1059, 1092-1152 | upsert×4/delete×2/replaceAllLinks/writeMeta/replaceAll |
| open 编排/迁移/恢复 | 1174-1622 | createEmpty/migrateFromJson/recover/openExisting/openProject/rewritePrimary |

**切分线定案（3 新 TU + 主瘦身 + internal 头）**：
- `catalogstore.cpp`（主，留 open 编排：dtor/openProject/openExisting/recover/migrateFromJson/createEmpty/rewritePrimary + 单 TU 助手 isSchemaReject/rotateFiles/removeSqliteFamily/readJsonFile/migratedDest/renameAside/readEpoch/integrityOk，估 ~690）
- `catalogstore_schema.cpp`（连接/DDL/事务：close/resumeConnection/attachWritable/connectPrimary/begin/commit/rollback + kSchemaSql/execScript/applyWritablePragmas，~360）
- `catalogstore_crud.cpp`（upsert×4/delete×2/replaceAllLinks/writeMeta/replaceAll + textArg/mapToJson/cornersToJson/parentsToJson，~310）
- `catalogstore_json.cpp`（sqlitePathFor/jsonPathFor/toJson/fromJson/loadTables + 各 FromJson 助手/absorb*/unsafeVersionReason/tableExists，~500）
- `catalogstore_internal.h`（namespace paleo::catalog_detail，inline 共享：kCatalogUserVersion/catalogConnectionName/forgetConnection/execSql/execScript/kSchemaSql/applyWritablePragmas + loadTables 声明——R0 逐一核对仅这 8 项跨 ≥2 TU；先例 metadata/storeerrors_internal.h）

### 事务边界语义（逐条保留红线）

- begin=BEGIN IMMEDIATE / commit / rollback（事务三件套整体进 _schema TU，语义零改动）
- replaceAll：begin → 4×DELETE → upsert 全表 → writeMeta → commit，失败 rollback（进 _crud TU）
- commitStore（datacatalog 主 TU 不动）的增量序：脏行按行号升序 upsert → 删除 → replaceAllLinks → writeMeta → commit
- createEmpty/migrateFromJson/rewritePrimary 的 attachWritable→replaceAll→失败清理（removeSqliteFamily/close）路径逐行保留
- schema_epoch=1、kCatalogUserVersion=2 不动（零迁移）

### 消费清单存档（API 冻结对拍基线）

- `grep -rln "datacatalog.h\|catalogstore.h" src tests | grep -v ^src/catalog/` = **234 文件**（明细 /tmp/catalog_consumers_r0.txt，拆分后对拍须逐字节一致）
- 消费大头：tests/（tst_catalog/tst_catalogstore/tst_datacatalog_sqlite/tst_realizationset/tst_mkprojectfixture/tst_coldstart/tst_perf_catalog 等全部）、src/workflow、src/services、src/app、src/ui（经白名单头间接）、src/metadata（mapversionstore 反向依赖 datacatalog）

### CMake 布线（拆分后更新点）

- datacatalog.cpp 在 CMakeLists.txt paleo_store 主列表（L449）→ 追加 6 新 TU
- catalogstore.cpp 在 cmake/extra-catalog-sqlite.cmake → 追加 3 新 TU
- 测试零新增（行为保留拆分，不加不删测试）

### 性能门基线

- tst_coldstart catalog_open 段 + tst_perf_catalog（R0 数值待基线 ctest 后回填）

## 迭代记录

第2轮 | 拆分落地：10 新 TU + internal.h 写入，两主文件瘦身，CMake 布线，头注释刷新；符号守恒对拍（DataCatalog 66 defs / CatalogStore 26 defs 集合一致；逐行守恒 diff 仅剩 TU 横幅注释与 namespace 骨架）；layering 三档绿 | 待基线 ctest 完成后重编验证 | 未通过（基线 ctest 跑中） | 基线红集合 → 增量重编 → tst_catalog×2 → 全量×2

wc 证据（拆分后实测，2026-10-10）：
```
src/catalog/datacatalog.cpp            689（≤900 ✓ 主）
src/catalog/datacatalog_versions.cpp   432（≤700 ✓）
src/catalog/datacatalog_links.cpp      317（≤700 ✓）
src/catalog/datacatalog_export.cpp     149
src/catalog/datacatalog_assets.cpp     148
src/catalog/datacatalog_derivation.cpp 112
src/catalog/datacatalog_entities.cpp   104
src/catalog/catalogstore.cpp           654（≤900 ✓ 主）
src/catalog/catalogstore_json.cpp      444（≤700 ✓）
src/catalog/catalogstore_crud.cpp      306
src/catalog/catalogstore_schema.cpp    155
src/catalog/catalogstore_internal.h    148
```

第1轮 | R0 勘察完成，切分线定案（上表）；worktree+基线构建启动 | 进行中 | 未通过（构建未完） | 等构建+基线 ctest 红集合

### 环境事件（R0 附记）

1. **PowerShell 5.1 解析 UTF-8 无 BOM 脚本失败** → 一律用 `pwsh`（PS7 默认 UTF-8）。
2. **系统 PATH 的 cmake 3.27 < 3.28 门槛** → 前置 `/c/Qt/Tools/CMake_64/bin`（3.29.3，兄弟 worktree 同口径）。
3. **新 worktree 缺 gitignored `vendor/onnxruntime`** → `find_library(ORT_LIB)` NOTFOUND → paleo_ai 空壳 → `aiassistworkflow.cpp.obj` 引用 `suggestHorizonTracking/runTileInference` 未解析 → **135 个可执行链接失败**（含 paleo.exe）。主 checkout 同样无 onnxruntime（本地口径常态红）。处置：从同基线兄弟 worktree（constraintpage-split，f474069b）复制 vendor/onnxruntime 闭包，重配重编——避免 135 个环境红淹没回归信号。

### R0 基线快照（2026-10-10，onnxruntime 接线后的干净基线）

- 全量 ctest：381 项，**15 红**（/tmp/redset_r0.txt）：tst_cache_las / tst_composeworkflow / tst_datapreview / tst_errorhub_storm / tst_fspath / tst_manifestproj / tst_mapping / tst_officepreview / tst_perfbudget / tst_services2 / tst_singlefactor_perf / tst_startup_trace / tst_wellcomposite_perf / tst_wellsection_ui / tst_workflows
- catalog 全家族基线全绿：tst_catalog 13.42s / tst_catalogstore / tst_datacatalog_sqlite / tst_catalog_scale / tst_perf_catalog / tst_realizationset / tst_mkprojectfixture / tst_coldstart / tst_catalog_catalogindex
- 性能门基线（tst_coldstart）：**catalog_open_process_cold(1000 assets)=34.2ms；catalog_open_dir_cold(200 assets)=18.7ms**

## 验收证据（2026-10-10 收口）

### Oracle 逐条

1. **类零变更 ✅**：datacatalog.h 仅 1 行注释改动（role 字段词表漂移修正，指向 RoleRegistry 权威）；catalogstore.h 零改动；`git diff --stat -- tests/` 空；外部消费清单 227 文件对拍一致（新增恰为 10 个内部 TU，零删除）。
2. **体量 ✅**：主文件 datacatalog.cpp 689 / catalogstore.cpp 654（均 ≤900）；新 TU 最大 catalogstore_json.cpp 444（≤700）。
3. **底座回归 ✅**：tst_catalog 零改动通过×2（单跑 2 遍 + 全量×2 中共 4 次）；tst_mkprojectfixture/tst_catalogstore/tst_datacatalog_sqlite/tst_realizationset/tst_catalog_scale/tst_catalog_catalogindex/tst_coldstart 全绿×3；全量 ctest×2：**14 个稳定红三遍（R0/P1/P2）完全一致**，与 R0 的差异仅 2 个抖动红——tst_errorhub_storm（R0 红，拆分×2+复跑全绿）与 tst_crossplot_controller（R0 SEGFAULT、复跑复现 SEGFAULT、全量×2 恰绿——supervisedChain 的 PROJ/本地 .tif 路径问题，与 catalog 无关，基线自身不稳定）。
4. **性能门 ✅**：catalog_open_process_cold 32.5ms vs R0 34.2ms（**-5.0%**）；catalog_open_dir_cold 19.0 vs 18.7（+1.6%）——无 >10% 劣化。
5. **schema ✅**：kSchemaEpoch=1 不动、kCatalogUserVersion=2 原值迁 internal 头；kSchemaSql DDL 字符串逐字节 diff 一致（57 行 md5 级）；零迁移。
6. **三档绿 ✅**：layering/--strict/--selftest 全过；构建零错误（EXIT=0）零新警告（新 TU 0 警告，残留警告均为预存：tst_catalog.cpp:1590/perfgroup.cpp C4834 + QGIS 头 C4996）；git diff --check 干净。

### 符号守恒对拍

- DataCatalog：66 个成员定义集合拆分前后一致（sort+diff 空）
- CatalogStore：26 个成员定义集合拆分前后一致
- 逐行守恒 diff（剥 include/namespace 骨架）：仅剩 TU 横幅注释与 internal 头 inline 前缀行

### Review 两轮（五维）

- R1（静态）：抓出 3 个真问题并修复——①catalog_detail 助手无限定调用缺 using（3 TU）②QSqlError 不完整类型（internal 头缺 include）③sed 对拍边界跑飞改用行号切（工具问题非代码）
- R2（全绿后）：API 等价/事务边界/schema/TU 边界/性能门五维复核零 High/Medium
- Low（记 PR body 不修）：catalogstore_json.cpp 残留未用 include <QDateTime>（零害）；using 列表可换 namespace 别名（纯风格）

## 结论

方向 99 六项 Oracle 全过。拆分形态：datacatalog 1,795→689+6 TU；catalogstore 1,605→654+3 TU+internal.h；realization 边界维持独立文件；词表权威唯一（RoleRegistry，无 catalogRoles）。
