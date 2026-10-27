# 文档清理（2026-10-27）

## 清理范围

- 95 份根目录 `.goal-loop-ledger*.md` 迁入 `docs/ledger-archive/goal-loop-*.md`。
- 过期的 `GOAL_LEDGER.md` 运行记录与 3 份旧全库审计报告移入同一归档目录。
- 新增 [文档入口](../README.md) 和 [历史记录索引](../ledger-archive/README.md)，修正 Markdown 链接及源码注释中的账本路径。
- 94 份任务书保留原件；目录索引移除旧执行纪律、预算与在飞状态，明确历史用途，份数声明继续受原文档漂移检查约束。
- TODOS 从 632 行整理到 243 行；完整旧内容保留在 [清理前快照](../ledger-archive/backlog-before-cleanup-20261027.md)。已交付事项移出工作清单，未完成的需求、人工验收与接口迁移继续跟踪。

## 内容对账

按当前源码和 CI 配置修正以下过期说明：

- Windows 本机同链开发与 `windows-2025` 四分片配置；OSGeo4W `packages`/`closure` 钉版。
- catalog.sqlite 是当前持久层；工程清单中的旧 JSON 字段保留兼容，不代表运行时仍使用 JSON 存储。
- GUI 新建使用来源目录与保存目录分离的向导；就地创建属于兼容入口。
- `DataCatalog::updateEntity`、`updateLink` 已存在；剩余是 sidecar/角色消费和撤销迁移。
- 3D 层位传播、显式倾角引导和属性体等入口已交付，不再重复立为待接入工作。
- 早期测试数量与耗时不再出现在当前构建操作说明中作为规模或通过结论。

## 验证

- `tools/check_docs_drift.py` 通过，`--selftest` 13/13 通过。
- `tools/check_layering.py --strict` 通过。
- 变更文档的本地链接检查通过；归档迁移的文件引用可解析。
- 源码、测试和 CMake 文件仅修改历史账本路径注释；运行逻辑未变，不重建产品或执行功能回归。
