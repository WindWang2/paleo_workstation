# Goal Loop ledger — catalog.sqlite（存储引擎替换）

- 分支：`goal/catalog-sqlite-2026-10-02`（自 origin/master `eaaf46d` 起，worktree `.worktrees/catalog-sqlite`）
- 构建：`cmake -S .worktrees/catalog-sqlite -B .worktrees/catalog-sqlite/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix -DQt6_DIR=C:/deps/Qt/6.8.0/msvc2022_64/lib/cmake/Qt6`，另需 GDAL/QGIS 库路径（`paleo-qgis-deps/Library`）。CMake 3.29（`C:/Qt/Tools/CMake_64`）。本机 Qt 6.8 缺 SerialPort/Positioning/Pdf，已用 aqt 补进同一前缀。
- 目标：DataCatalog 内存模型、查询 API、journal/mutationSeq/owner 线程契约不变，盘上主存储换成独立 `artifacts/metadata/catalog.sqlite`（不并入 project.sqlite）。

## Oracle（验收，全绿即收）

1. 金样 catalog.json → sqlite：行数与抽样字段一致，`catalog.json.migrated` 存在且后续 upsert 不改它。
2. JSON 迁移与 sqlite 重载字段级相等（含 5288.67）。
3. applyJournal 在第 k 个 op 后中止：sqlite 停在基线；BatchSave 持有期间文件 mtime 不变，析构一次提交。
4. 坏 sqlite 头从 .bak 恢复，`backupRecovered` 一次，坏主文件不进 .bak。
5. 缺库 + readOnly：不建文件、不 mkpath。
6. 工作线程只读内存；staging 不共享连接，sqlite 字节不变。
7. 合成库打开 ≤ JSON 打开 × 1.5；单次变更快于整文件 JSON 重写。比率，不卡墙钟。
8. 本 ledger + `docs/progress/catalog-sqlite.md`；TODOS P3 划掉；SCHEMA_MIGRATION 登记第五个库。WAL 只给 catalog.sqlite，project.sqlite 仍不采用。

## 轮 0 — 勘察

- Qt SQLITE 不是静态插件。驱动在 `C:\deps\Qt\6.8.0\msvc2022_64\plugins\sqldrivers\qsqlite.dll`。测试要 `QT_PLUGIN_PATH` 指到 `plugins`，无头跑加 `QT_QPA_PLATFORM=offscreen`。
- 写路径除 `DataCatalog::save` 外没有第二份 catalog.json 作者。消费者只拿 `catalogPath()` 字符串。
- `MetaStore::kUserVersion` 与错误串里的 `project.sqlite` 不改。catalog 的 schema 代际走 `catalog_meta.schema_epoch`。
- 备份改在健康库的**下一次 open**做整文件拷贝，不在每次增量 save。integrity 通过之后才拷，避免把坏主文件转进 .bak（#79）。规格里的「先拷再查」会污染备份链，不采用。

## 轮 1 — CatalogStore（DataCatalog 未切换）

改动：`src/catalog/catalogstore.h/.cpp`，`cmake/extra-catalog-sqlite.cmake` 登记进 `_paleo_extra_manifest`，`tests/tst_catalogstore.cpp`。`tst_datacatalog_sqlite.cpp` 仍是占位（QSKIP）。DataCatalog::save 仍写 catalog.json。

验证：

```
python tools/check_layering.py --strict
检查通过：无层违规。
```

```
cmake --build .worktrees/catalog-sqlite/build --target tst_catalogstore
BUILD_EXIT=0
```

```
tst_catalogstore.exe -v1
QT_PLUGIN_PATH=C:\deps\Qt\6.8.0\msvc2022_64\plugins
QT_QPA_PLATFORM=offscreen
Totals: 11 passed, 0 failed, 0 skipped, 0 blacklisted, 644ms
```

覆盖：金样迁移（ver-9 不入库、5288.67、备注原文、`.migrated` 后续不改、journal_mode=wal、synchronous=2、user_version=1）、只读缺文件不建库且 MetaStore 不 mkpath、schema 999 / 坏 JSON 不建 sqlite、第二次 open 才出 `.bak`、坏主文件恢复 well-A 而不是之后的 well-B、`rewritePrimary` 隔离 `catalog.sqlite.corrupt-*` 且不改 bak 大小、user_version=99 与 schema_epoch=99 拒开且不回退 bak、事后插入的 `../` 版本留在盘上但从内存跳过、rollback 不留下 well-B。

判定：Oracle 1 与 5 在 CatalogStore 层实测通过。Oracle 2–4、6–8 还要等 DataCatalog 切换。

下一步：DataCatalog 打开/保存改走 CatalogStore；按 open 时备份改写会红的 JSON 备份测试。

## 轮 2 — DataCatalog open/save 改走 CatalogStore

改动：`src/catalog/datacatalog.h/.cpp`。`open` / `save` 走 `CatalogStore`；脏集增量 upsert；`m_forceFullSave` 或连接不可写时 `rewritePrimary`。`catalogPath()` 仍返回 `catalog.json`。新增 `sqliteCatalogPath()`、`exportCatalogJson()`（当前内存、revision 不加一、不写 sqlite、不发 `changed`）。`createStagingCopy` 不拷 `m_store`，`save` 在任何 store 访问之前见 `m_staging` 即返回。不双写 `catalog.json`。`applyJournal` 仍无 sqlite 事务，无 `debugAbortJournalAfter`（轮 3）。

测试改指向 sqlite，不恢复 JSON 双写：

- `tests/tst_catalog.cpp`：bak 为 `sqliteCatalogPath()+".bak"`，出现在健康库的下一次 open；链演练 close+reopen；损坏前 `close()` 并删 `-wal`/`-shm`；隔离名 `catalog.sqlite.corrupt-*`；`user_version=99`；双坏文件；uwi/aliases/ordinal 字节改 `exportCatalogJson`；`unsafeManagedPathSkippedOnLoad` 用第二连接对 `catalog.sqlite` 做 INSERT（不用 `MetaStore::closeConnectionsFor`）。
- `tests/tst_entityview.cpp`：ordinal 字节改 `exportCatalogJson`。
- `tests/tst_perf_catalog.cpp`：备份轮转、坏主文件恢复、锁定失败不改 sqlite 字节。
- `tests/tst_previewdoc.cpp`：`catalogRecoveredFromBackupForwards` 先再 open 一次做出 bak，再换目录释放锁、截断 `catalog.sqlite`。源已改，本轮未编译、未跑。
- `tests/tst_import.cpp` `buildIngestPlanLeavesCatalogUntouched`、`tests/tst_aiassistworkflow.cpp` `catalogBytes`：落盘字节从 `catalog.json` 改为 `catalog.sqlite` + `-wal`。源已改，本轮未编译、未跑。

验证（本轮实测，日志在 `build/test-logs/`）：

```
python tools/check_layering.py --strict
检查通过：无层违规。
```

```
cmd /c "call VsDevCmd.bat -arch=amd64 -host_arch=amd64 && cmake --build C:\Users\wangj.KEVIN\projects\paleo_workstation\.worktrees\catalog-sqlite\build --target tst_catalog tst_catalogstore tst_entityview tst_perf_catalog"
BUILD_EXIT=0
```

运行：`System.Diagnostics.ProcessStartInfo`，`-v1 -o build\test-logs\<name>.txt,txt`，`PATH` 含 `C:\deps\Qt\6.8.0\msvc2022_64\bin`、`paleo-qgis-prefix\bin`、`paleo-qgis-deps\Library\bin`，`QT_PLUGIN_PATH=C:\deps\Qt\6.8.0\msvc2022_64\plugins`，`QT_QPA_PLATFORM=offscreen`。四个进程退出码均为 0。

```
tst_catalog:       Totals: 41 passed, 0 failed, 1 skipped, 0 blacklisted, 3085ms
tst_catalogstore:  Totals: 11 passed, 0 failed, 0 skipped, 0 blacklisted, 584ms
tst_entityview:    Totals: 12 passed, 0 failed, 0 skipped, 0 blacklisted, 739ms
tst_perf_catalog:  Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 73960ms
```

跳过的是既有 Windows 用例 `TestCatalog::applyJournalIsAtomicOnFailure`（`tests/tst_catalog.cpp`：目录权限不生效）。`thousandEntityQueryBudget`：`json=2297KB open=300ms save=2ms`（预算 2000ms）。

`cmake --build ... --target tst_previewdoc` 退出码 1。失败在 `src/qgis/qgislayerprofile.cpp` 与 `src/qgis/previewmapcanvas.cpp`：`fatal error C1090: PDB API call failed, error code 23 (0x000006BA)`（`paleo_qgis.pdb`）。`tst_previewdoc.cpp` 未编到，QGIS 初始化未到达。`build\tst_previewdoc.exe` 不存在。不记为通过。

判定（当时）：Oracle 2（字段往返，含 5288.67 的内存断言仍过）、Oracle 4（坏 sqlite 从 bak 恢复，隔离文件不进 bak）、Oracle 6 的 staging 字节不变，已在 DataCatalog 层实测。Oracle 3 留轮 3。Oracle 7 的千实体迁移在 2000ms 预算内；比率墙钟未单列对比。

## 轮 3 — applyJournal 注入中止

`debugAbortJournalAfter(k)`：`k<=0` 关闭。第 k 个 op 成功后、`endBatch`/`save` 之前 `restore()` 并返回。错误含子串 `catalog 提交在第 k 个 op 后中止（测试注入）`。此前回放不打开 sqlite 事务，所以 `catalog.sqlite` / `-wal` / `-shm` 字节与存在性不变。空 journal 仍立即返回。嵌套 `BatchSave` 仍由外层结算。

`tests/tst_datacatalog_sqlite.cpp` 替换 QSKIP 桩：`abortJournalAfterKLeavesSqliteBytesIdentical`、`batchSaveLeavesSqliteUntouchedUntilFlush`、`workerReadsDoNotTouchSqlite`。

文档与 `missingMembers` 另提交 `4fa2c74`（`TODOS.md` 划掉查询索引 P3、`docs/SCHEMA_MIGRATION.md` 第五库且 WAL 仅 catalog、`docs/progress/catalog-sqlite.md`、catalog 成员任一 sqlite/json/migrated 即在场）。`projectFileForQgz` 默认串未改。

验证：

```
cmake --build .worktrees/catalog-sqlite/build --target tst_datacatalog_sqlite tst_catalog
```

同一 offscreen 环境，`-o build\test-logs\<name>.txt,txt`，两进程退出码 0。

```
tst_datacatalog_sqlite: Totals: 5 passed, 0 failed, 0 skipped, 0 blacklisted, 230ms
tst_catalog:            Totals: 41 passed, 0 failed, 1 skipped, 0 blacklisted, 3037ms
```

跳过仍是 `applyJournalIsAtomicOnFailure` 的目录权限探针。

判定：Oracle 3 实测（k=1、k=2、k=journal 长度中止后三文件字节不变，随后完整重放成功）。Oracle 6 实测（staging 写入与 worker 只读都不改三文件字节；批次持有期间 mtime 不变，flush 后才一次 `changed()`）。Oracle 8 文档已落。

## 轮 4 — 消费方测试

`paleo_qgis` 能编过的前提是 QGIS 前缀里有 `ui_qgs*.h`（本机从 `paleo-qgis-build\qgis-vendor\src\ui` 补进 `paleo-qgis-prefix\include\qgis`，不进仓库）。QGIS 动态库要跟 `paleo-qgis-deps` 的 Qt 6.11.2 走；把 Qt 6.8 放在 PATH 最前会在加载 `qgis_core.dll` 时 `STATUS_ENTRYPOINT_NOT_FOUND`（0xC0000139）。

`tst_aiassistworkflow` 挂在 `PALEO_HAVE_ORT` 下。本机没有 ONNX Runtime，CMake 没生成这个目标，未编未跑。

```
cmake --build .worktrees/catalog-sqlite/build --target tst_import tst_previewdoc --parallel 4
BUILD_EXIT=0
```

运行：`PATH` 以 `paleo-qgis-deps\Library\bin`、`paleo-qgis-prefix\bin` 开头，其后才是 Qt 6.8。`QT_PLUGIN_PATH` 指 deps 与 prefix 的 plugins。`QT_QPA_PLATFORM=offscreen`。`-o build\test-logs\<name>.txt,txt`。

```
tst_previewdoc: Totals: 13 passed, 0 failed, 0 skipped, 0 blacklisted, 4988ms
tst_import:     Totals: 49 passed, 0 failed, 2 skipped, 0 blacklisted, 29628ms
```

`catalogRecoveredFromBackupForwards` 通过（原因含 `catalog.sqlite`）。`buildIngestPlanLeavesCatalogUntouched` 在 49 个通过项里。两处跳过都是既有探针：`failingCommitSurfacesErrorAndRollsBack` 目录权限不生效，`folderImportRealAreaSmoke` 因为 `PALEO_REAL_PROJECT_AREA` 未设置。实工区烟测按此跳过，不记为通过。
