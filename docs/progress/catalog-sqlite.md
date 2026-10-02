# catalog-sqlite — 目录持久化换独立 sqlite（goal/catalog-sqlite-2026-10-02）

分支 `goal/catalog-sqlite-2026-10-02`（自 origin/master `eaaf46d` 起）。迭代账本
`.goal-loop-ledger-catalog-sqlite.md`。本文只记语义决策和边界。
事实源：`src/catalog/catalogstore.h` 头注释。

## 交付

| 面 | 内容 |
|----|------|
| 落盘 | 独立文件 `artifacts/metadata/catalog.sqlite`，不并入 `project.sqlite`。WAL + `synchronous=FULL`。变更集单事务提交 |
| 版本 | `catalog_meta.schema_epoch` = `CatalogStore::kSchemaEpoch`（1）。不是 `MetaStore::kUserVersion` |
| 表 | `entities`、`entity_asset_links`、`assets`、`versions`，加 `catalog_meta` 键值。无外键，无 BLOB |
| JSON | 只做迁入和导出。迁入成功后 `catalog.json` 改名为 `catalog.json.migrated` |
| 备份 | open 时把健康 sqlite 整文件拷贝。不在每次 save 拷 |

## 语义决策

1. **改的是盘，不是内存。** `DataCatalog` 的四张表仍是查询事实源。查询 API、
   journal、`mutationSeq`、owner 线程契约不动。打开后查询不扫 SQL。
2. **JSON 只做迁入和导出。** 只有 `catalog.json` 时解析后单事务导入，成功则
   改名为 `catalog.json.migrated`；之后的 upsert 不改这份改名文件。导出是
   `CatalogStore::toJson` / `exportCatalogJson`（根键 `schema_version` =
   `kJsonSchemaVersion`）。sqlite 已经在：它是权威，json 原样留着。
   JSON 不再是活库，也不再每次 save 全量重写。
3. **第五个库。** 与 `metadata/project.sqlite` 分开。`project.sqlite` 仍不采用
   WAL（`docs/SCHEMA_MIGRATION.md` §6）。本库例外：单文件、写期间仍有读者，
   `PRAGMA journal_mode = WAL`，`PRAGMA synchronous = FULL`
   （禁止 `synchronous=OFF`）。这不改变 `project.sqlite` / gpkg / .qgz 的决议。
4. **代际。** `schema_epoch` 写在 `catalog_meta`，缺表或缺键视为 1。
   epoch 高于本构建拒开，错误含 `unsupported catalog schema`。
   `PRAGMA user_version` 高于本构建也拒开，错误含子串 `newer than this build`。
   两条都**不回退 `.bak`**（降级不是恢复），原件不动。
5. **备份。** open 时做，不在每次 save 做——否则增量写被整文件拷贝吃掉。
   `integrity_check` 通过之后才把当前好库整文件拷进
   `catalog.sqlite.bak` / `.bak.N`（保留代数与 JSON 时代相同）。
   已损坏的主文件不进 `.bak` 链。
6. **工程束默认串不改。** `projectFileForQgz` 仍写
   `artifacts/metadata/catalog.json`。`missingMembers` 把声明路径、
   `artifacts/metadata/catalog.sqlite`、`artifacts/metadata/catalog.json`、
   `artifacts/metadata/catalog.json.migrated` 任一存在视为 catalog 在场。

## 递延

- full SQL read path
- curve index table

内存四表继续当查询事实源。上面两件都不在本轮。写路径超线性 profiling
（TODOS，2026-10-01 WP2）保持关闭，不重开。
