# Agent 查询分页实施（W4a/W4b）

基线：master `dd7baa1f`，从计划/入口清单分支独立实现；后续由父任务堆叠到 W1–W3。
对应[整改计划 W4](../planning/AGENT_CALL_READINESS_PLAN.md)。**SDK 与基础目录回归已过；
客户端 12 个相关测试目标正在增量编译，主仓 W4 尚未提 PR。**

## 查询边界

- `ProjectQueryCapability` 是只读数据能力，聊天 runner 只适配结果和异步回报。
  `paleo_services_query` 编译同一生产实现，并承载原有 ArtifactVersionCapability；
  `paleo_services` 公共链接保留旧 API，未创建执行替身或第二份事实库。
- CatalogIndex 在 open/rebuild 时建立有序 ID、类型、角色、名字、模型井 ID 与父子索引；
  add/update/rename/rollback/staging/adopt 维护它们。页用 upper_bound，父引用按不可变数组
  序号直接取窗；currentVersion 保留原先“同号末行优先”语义并走 O(1) 派生索引。
- 列表采用稳定 catalog ID 顺序；关联采用稳定链接行顺序；父版本保持原数组顺序。
  这是带版本分页契约的明确顺序，显示名不承担身份。`page_size` 是最大条目数，
  UTF-8 完整 JSON 字节预算可先达到；`count` 仍表示主题的全量/筛选后计数。
- `catalog.version.describe.v1` 原严格对象和 128 父引用摘要保持不变；新
  `query_project topic=version_parents` 使用 `catalog.version.parents.page.v1` 续读完整父引用。
  普通项目页标 `paleo.project.query.page.v1`，血缘页标 `paleo.lineage.page.v1`。

## 游标和覆盖

opaque 游标含版本、随机会话/页身份、到期与 HMAC，不含路径或裸容器迭代器。
宿主保存查询摘要、doc/epoch/context generation/chat scope、Catalog revision/mutationSeq、
必要的 SDK store/generation/sequence，以及 after-key 或增量遍历状态。查询和重放均重验
快照；筛选、页预算、排序、文档或修订不符明确拒绝，不偷偷重开第一页。
无持久文档身份的旧只读装配使用显式 `session:<UUID>` 身份，重绑即失效，不把工程
路径伪装成 documentId，也不放宽严格版本描述或发布的身份要求。

默认页最大 50 条、8 KiB，可在 1–256 条及 2–64 KiB 内选定；活动游标最多 128 个、
状态/缓存预算 8 MiB、TTL 30 分钟。允许重放最近消费的一页；消费其后续页后回收更早的
已消费页，旧重放给 `cursor_replay_unavailable`。这保证一万条小页不会被“第 128 页”
截断，也不把无界页历史留在内存。最近页重放不重新遍历，继续页仍持有原始冻结状态。
此窗口只适用于查询续页，不改 W1 不可变聊天结果缓存的 TTL 契约。

血缘用 `(direction, versionId)` 分别记最短深度，节点与边另外去重；只沿请求方向扩展，
不在途中节点切换方向纳入兄弟分支。每页限制节点、边、字节及工作量；状态限制节点
4096、边 16384。跨页边允许引用前页/后页节点，selection.version_ids 只表示本页节点。
未完成时 totalCount 为 null，完成时只统计请求深度范围内的已遍历集合。

## 有效模型井

查询集合与原 `entities()` 口径一致：目录登记的井/计划井；模型独有井仍由既有登记链
纳入目录。对井页逐条点读当前模型名称/TD，保留人工/旧式井及模型映射缺失时的既有
值语义，不借“stored 页”静默丢弃有效覆盖，也不加载曲线/测斜/备注等未用载荷。

SDK 新增 WellQueryValue 和规范化井名索引：可写 open/rebuild 在现有 ModelWriteLease
与 SQLite 事务内建立派生索引；后续 well after-image 在同一 journal 事务维护。旧写者
留下 dirty 或旧库缺索引时，规范化查询明确 unavailable；只读查询不偷偷迁移或写库。
井名按 Qt 字符小写、忽略空白/连字符/下划线，与旧目录匹配语义相同；歧义至少两项即
拒绝，唯一性搜索本身按有界候选页续读，`well_id` 可直接进入关联页。

井结果附 `well_ref`：document_id、catalog_entity_id、逻辑 model_store_id=`area-model`、
实际 model_store_identity、十进制字符串 model_well_id/model_head、model_generation 和
model_available。不可用时不伪造 SDK 身份；model_head 是查询水位，不冒充输入值修订。
后续预测仍须另行冻结曲线/区间/模型的准确值版本。

SDK 钉版 `7738eb2a2b09818c80b55720e5972360206e7444`，
[独立 SDK PR #18](https://github.com/changyanyanchang/Data_service/pull/18) 叠加在原钉版
分支，只含三个兼容新增/修复提交。该版本同时提供 W6 精确 IL/XL 有界单道读口。
井查询派生表不进入业务变更日志；可写修复会清理旧 SDK 曾为这些表安装的通用
journal 触发器，避免误报整库重建或 unmanaged。

结果卡只对新页契约展示“本页、总数未知、尚有后续、请求范围已读完”；selection
只代表本页，跨页边保留在原始 JSON，图只画端点均在本页的边并标清。旧历史契约
保持原渲染，字体/颜色/布局不变。未知井提示只列有界完整井名示例；错误 JSON 同样
遵守 `max_bytes`。跨平台读库时，Windows/UNC 外部绝对路径同样仅暴露文件名。

## 验证记录（客户端整体验收进行中）

- 已完成基础 `paleo_store` 的真实 Qt 6.11.2/QGIS 4.2.3/SDK 生产编译。
- 六个新增生产 query/runner TU 的原生产编译参数 `-fsyntax-only` 通过；不代替链接或测试。
- 基础真实 CTest 四靶全部通过：`tst_project_query`（当时 13/13，0 skip）、
  `tst_wellref`、`tst_catalogstore`、`tst_datacatalog_sqlite`。
- 新 `tst_project_query` 已写：一万条/超过128小页完整读回、无全量 accessor、重放/
  筛选/字节预算、批次/回滚/重开/预备收养、400父引用、方向/环/兄弟分支、有效模型井
  与 head 失效、TTL；后来增加 300 分支/零深度/重放零读取、260 有效井名候选续查与
  归一化歧义、便携路径和错误预算测试，尚待本次 12 靶整体验收。旧聊天查询回归改为
  验证续页完整性。
- 独立 SDK 真实配置/编译/链接后，`well_query_contract`、`model_change_contract`、
  `seismic_service_tests` 三套 CTest 全过（1.18s，0 skip）：Unicode、负 TD/NaN、
  点读缺失/预算、复合索引查询计划、写失败回滚、旧写者 dirty、只读不迁移、旧触发器
  修复重开，以及真实非 unit IL/XL/缺道/实际 ordinal/窗口/预算/取消。
- 最新完整 Source gates 32/32，通过锁版 QGIS 头的全传递检查。TS 中英键各 10821，
  missing/extra 均 0，10798 个未修改英文键的既有翻译逐条保持。
- 首轮失败保留日志：SDK journal after-image 错取顶层 name 导致 SQL NOT NULL，已改
  为读取 value.name 的类型化参数；访问 hook 接错写线程检查被一万条验收抓到，已移回
  noteRead；新增文案缺 TS 与一次合并误删均已修复并重跑全闸。未通过提高基线转绿。

整体验收包含 `tst_aichatprojectquery`、`tst_aiwiring`、`tst_aitoolresultview`、严格
版本描述和原 CatalogIndex/目录/角色/血缘回归；最终结果将在完成后补记。父任务堆叠
与 rebase 后仍须重跑 Source gates。

临时脚本/完整编译日志位于 worktree `local/query-validation` 及 WSL 独立 build，
不提交额外运行框架。构建总并行遵守全队上限；本批未新增或提高任何源码闸门基线。
