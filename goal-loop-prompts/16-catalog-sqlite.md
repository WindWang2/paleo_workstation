# Goal-Loop 方向 16：catalog 持久层迁 SQLite（存储引擎级替换）

## 背景（实测事实，勿再勘察）

- **存储底座现状**：`catalog.json`（`<projectDir>/artifacts/metadata/catalog.json`，`src/catalog/datacatalog.h:129`）是数据织物唯一事实源；写路径 `QSaveFile` 原子替换 + `.bak` 轮转（默认 3 代，`datacatalog.h:159`）+ 解析失败回退最新 .bak（`recoveredFromBackup` 信号一次，~329 行）。
- **本阶段注明递延**：`catalog.sqlite 按计划递延 TODOS P3`（datacatalog.h 头注释）——本方向就是收这个递延项。
- **SQLite 基础设施已齐**：`src/metadata/metastore.h` 的 `openConnection(path, connectionName, error, readOnly)`（路径参数化，#80 只读降级已落地）、`closeConnectionsFor`、`readUserVersion`/`ensureUserVersion`；`FaultSetStore`/`PaleoProjectStore` 等共库 `metadata/project.sqlite` 先例（`faultsetstore.h:8`）。
- **produce-then-commit 机制**（#106 收口，本方向不得破坏）：`CatalogOp` journal → worker 拿 `createStagingCopy` 副本改 → owner 线程 `applyJournal` 按 `mutationSeq` 基线同序重放（datacatalog.h:85-106、312、325）；`BatchSave` 挂起 `save()` 攒批。
- **读面线程契约**：mutator 限 owner 线程，读跨线程允许（`noteRead` 警告计数）——SQLite 连接不是跨线程安全的，所以本方向**只做持久层换引擎，读面继续走内存结构**（见目标形态，勿做全 SQL 读面）。
- **痛点量化**：`save()` 每次全量重写 JSON（O(版本总数)）；applyJournal 逐 op 重放期间崩溃 = 半回放状态靠下次全量 save 覆盖抹平（非真原子）；大 catalog（DERIVED 资产churn 快）下 `CatalogIndex` 邻接索引全量重建。
- 纪律：读 DESIGN.md；每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；oracle 不接受「应该没问题」。

## 目标形态

**持久层换引擎：内存模型、查询 API、journal 机制全部不变，只换落盘介质。**

```
DataCatalog（内存：m_entities/m_assets/m_versions/m_links + CatalogIndex）
   │  mutator → 变更集（JournalOp 照旧记）
   │  save()/BatchSave 释放点 → 单事务写 catalog.sqlite（增量 SQL，非全量）
   │  applyJournal(ops) → 重放 mutators → 全程一个 SQLite transaction
   ▼
<projectDir>/artifacts/metadata/catalog.sqlite   ← 新事实源（WAL 模式）
<projectDir>/artifacts/metadata/catalog.json    ← 迁移输入 + 导出/交换格式保留
<projectDir>/artifacts/metadata/catalog.bak[.N] ← 文件级备份轮转照旧
```

- **独立库文件**：catalog 用独立 `catalog.sqlite`（MetaStore 路径参数化天然支持）——**不并入 project.sqlite**：catalog 有自己的 .bak 轮转/腐败恢复契约，共库会把备份语义耦合（文件级轮转旋转的是整个 DB）。
- **schema**：四表对现有四结构（`entities`/`entity_asset_links`/`assets`/`versions`）+ `catalog_meta` kv 表（mutationSeq、备份策略、schema_epoch）。索引：`links(entityType,entityId,role)`、`links(assetId)`、`versions(assetId)`、`entities(entityType,name)`。`extra`/`note` JSON 文本列直存。
- **open() 迁移序**：catalog.sqlite 存在 → 装表进内存（权威）；不存在但 catalog.json 存在 → JSON 解析 → 单事务导入 sqlite → `catalog.json` 改名 `catalog.json.migrated`（不删，可回溯）→ 后续读写全走 sqlite。全新工程 → 建 sqlite。
- **备份/恢复语义保持**：open() 时先把现存 `catalog.sqlite` 文件级拷贝进 `.bak` 轮转链（同保留代数策略）；sqlite 打开失败/integrity_check 不过 → 回退读最新 `.bak` 拷贝并照旧发 `recoveredFromBackup` 信号。
- **事务原子性**：`save()` = 把挂起变更集打进一个 transaction（BatchSave 语义不变：挂起期间不落盘）；`applyJournal` = 重放全部 ops 包进一个 transaction（中途崩 → 整体回滚，基线不变——比现状更严）。
- **JSON 保留为导出**：`exportCatalogJson(path)` 调试/交换出口（可选 cheap 实现：内存结构序列化，复用现有 writer）。
- **user_version/schema 门**：走 `MetaStore::readUserVersion/ensureUserVersion`；schema epoch 定义成常量，后续表演进走 `docs/SCHEMA_MIGRATION.md` 既有流程。
- **只读打开**：`openConnection(readOnly=true)` 支持「不创建缺失库」的查看态（#80 先例）。

## Oracle 验收（全部须实测通过并记账本）

1. **迁移正确性**：黄金夹具工程（含 entities/links/assets/versions 全覆盖 + unresolved 链接 + extra/note 各形态）走 catalog.json → open() → catalog.sqlite：行数逐表相等 + 抽样字段哈希一致 + `catalog.json.migrated` 存在且原文件不再被写。
2. **读面零回归**：同一工程分别走 JSON 旧版 / sqlite 新版载入 → `entities()/links()/assets()/versions()/linksForEntity()/unresolvedLinks()` 等全部查询 API 结果逐项相等（字段级 diff 断言，非行数）。
3. **事务原子性**：`applyJournal` 重放中注入失败（第 k 个 op 后强制 abort）→ catalog.sqlite 保持基线状态（逐表断言无半回放）；`BatchSave` 挂起期间盘上无写入（断言文件 mtime 不变），析构时单事务提交。
4. **腐败恢复**：人为破坏 catalog.sqlite 头 → open() 从 `.bak` 拷贝恢复 + `recoveredFromBackup` 信号恰一次 + 代数轮转正确。
5. **只读降级**：库文件不存在 + readOnly → openConnection 不建库、查询返回空集、无写盘（#80 语义）。
6. **并发/线程面不变**：worker 线程读 catalog 不报新错；owner 线程 mutator 走 sqlite 连接；`createStagingCopy` 行为不变（副本仍走 JSON 内存序列化，不共享 sqlite 连接）。
7. **性能比率门**：合成 catalog（e.g. 1k 实体 / 5k 链接 / 20k 版本）：open 时间不劣于 JSON 版 ×1.5；连续 100 次单 mutator save 总耗时优于 JSON 全量重写（增量写收益，断言比率不钉墙钟）。
8. ledger 全账 + `docs/progress/catalog-sqlite.md`（schema 定稿、迁移序、备份语义差异、递延项——含「全 SQL 读面」与「曲线索引表」两个后续立项）；`TODOS.md` P3 项划掉；`SCHEMA_MIGRATION.md` 登记新 store。

## 勘察指引

- `src/catalog/datacatalog.{h,cpp}`（save/open 落盘点 ~652/394 行、`.bak` 轮转、applyJournal 重放循环、CatalogIndex 重建点——增量写要同步索引语义）
- `src/metadata/metastore.{h,cpp}`（连接生命周期/读-only/user_version 门全套路——直接复用不新造）
- `src/metadata/faultsetstore.{h,cpp}`（独立 store 表+共库先例的写法模板）、`paleoprojectstore.h`（写通道纪律注释）
- `src/workflow/derivedassets.h`（CatalogOp/Staging 契约——不改接口，只包事务）
- `tools/check_layering.py` + `tools/layering_vocab.json`（QtSql 在数据层的既有豁免先例——petrophyscomputeservice 已 QSqlDatabase）
- `docs/SCHEMA_MIGRATION.md`、`TODOS.md` P3 段（本方向收口的原始记录）
- 测试先例：`tst_import`/`tst_folderimport`（catalog 断言面）、`tst_factorworkflow`（journal 路径消费者）

## 禁区

- **不改 DataCatalog 公开 API 签名**——所有调用方（projectdata/derivedassets/各 workflow/UI 门面）零改动是硬约束，读面仍走内存结构；全 SQL 查询面是后续立项。
- **不并入 project.sqlite**（理由见目标形态）；catalog 独立文件。
- **不做全 SQL 读面重构**（mutator/read 全部改 SQL 是大手术，风险收益不成比例，另立项）。
- **不建曲线索引表**（well_logset 的 `(well,version,mnemonic)` 索引是 15 号/后续方向的事，本方向只管四表迁移）。
- 不删 JSON 读写代码路径——保留为迁移输入 + `exportCatalogJson` 出口（一个版本后可视情况退到仅导出）。
- 不改 `createStagingCopy`/`applyJournal` 的跨线程契约与 `mutationSeq` 语义；不改 owner-thread 写守卫（#106）。
- 不把文件本体/BLOB 入 sqlite——四表纯元数据；artifacts 文件树不动。
- WAL/同步级别不许偷懒到 `PRAGMA synchronous=OFF`（崩溃安全 > 微秒收益）。

## 迭代协议

- **轮0**：勘察定案——QtSql 驱动可用性（vendored Qt 的 SQLITE 插件是否存在/静态还是插件目录）；schema DDL + 索引 + kv 表定稿进 ledger；确认 `datacatalog.cpp` 全部写盘点（save 之外有无直写）。
- **轮1**：`catalogstore` 持久层（`src/catalog/` 内新模块或并入 datacatalog，先 `scripts/new_module.sh` 登记）——open/migrate/save(事务) + schema 门；`tst_catalogstore`（Oracle 1/5）。
- **轮2**：DataCatalog 内嵌切换（open 迁移序、save 走事务变更集、.bak 拷贝轮转、recovery 信号语义保持）+ `tst_datacatalog_sqlite`（Oracle 2/4）。
- **轮3**：applyJournal→事务包裹 + 失败注入测试（Oracle 3）+ createStagingCopy 回归。
- **轮4**：全测试面回归（catalog 消费者：import/folderimport/derivedassets/factorworkflow/petrophys）+ exportCatalogJson 出口。
- **轮5**：真工区实测（`PALEO_REAL_PROJECT_AREA` 工程带既有 catalog.json 走迁移，断言无损 + 后续会话直开 sqlite）+ 性能比率记账 + docs/progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/catalog-sqlite -b goal/catalog-sqlite-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：
   - `git diff origin/master...HEAD` 全量自审：无调试残留/死代码；层标记齐；`check_layering --strict` 绿；
   - vendor 前缀全量构建零新警告，ctest 全绿；
   - Oracle 每条有命令+输出摘要证据；
   - 发现问题先修再验直到干净。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
