# H4：当前 Agent 作用域的持久 operation 发现与关系分页 — 2026-11-23

- 分支：`codex/h4-agent-operation-discovery`（worktree `../paleo-h4-agent-operation-discovery`）
- base：`origin/master = 8eca4644db31b4dcfd5ff11bebf39f0bbcfaf3a5`（开工 fetch，0 开放 PR）
- OS：CachyOS x86_64（Linux 7.3.4）；依赖前缀：`vendor/prefix → paleo-h3 deb 闭包`
  （stamp 与 `vendor/deb-closure.lock` 匹配，QGIS_PREFIX=该前缀 usr）；`vendor/onnxruntime`、
  `vendor/ranuts-document` symlink 主仓；`vendor/Data_service` 按本分支 gitlink `905b5c1b` 初始化。
- 交付：`paleo.operation_list` —— 不要求已知 operation_id 的只读发现工具；
  当前工程 + 当前聊天作用域内有界列出合法 v2 持久操作摘要（operations 视图），
  或以已授权操作为锚点分页直接关系（relations 视图）；得到 ID 后走既有
  get/results/read/resume/cancel。发现自身零执行/零恢复/零对账/零 journal 写入。

## 设计与边界

- **快照语义（W4，已选定）**：一次受权读取形成不可变摘要快照，分页从快照顺序取出。
  journal 其后变化不使旧页失效、不混入新行；`coverage.snapshotDigest` 声明快照身份，
  `snapshotPolicy="immutable-snapshot@1"`；需要当前状态重新发现或调 operation_get（工具
  description 明说）。会话/工程/epoch 变化（Recovery `live()` 身份检测 →
  `invalidateRuntimeHooks` → `OperationDiscovery::invalidate()`）整体失效。
- **游标（W4）**：`opdisc1.<snapshotKey>.<position>.<expiry>.<HMAC-SHA256>`，常时比较；
  绑定 canonical 工程、document、callerScope、完整查询摘要（视图/筛选/锚点/页预算，
  缺省归一）与快照身份。篡改→`cursor_invalid`；过期→`cursor_expired`（不静默回第一页）；
  换作用域→`cursor_scope_mismatch`；换条件→`cursor_query_mismatch`。TTL 10 分钟、
  maxCursors 128、快照缓存 8 MiB/4 份；**只回收过期项**——缓存满给
  `discovery_cache_full`，仍有效的续页不被挤掉。游标/缓存不跨重启（无持久化身份）。
- **排序（W1）**：operations 视图 `operationId-lex@1`（UUID 不编码时间，不捏造时间序，
  时间字段一律不出现）；relations 视图 `relation-kind@1`（source_operation → consumer
  按 operationId → plan_step 记录序 → parent_version 记录序）。
- **关系分类（W5）**：直接持久目标必须是快照内同 document/caller 的 v2 记录；
  解析不到的来源只给 `unresolved_in_snapshot`（不回显目标 ID）。计划内 child 步骤保留
  nodeId 与记录的 operationId（记录事实），标注 `access=plan_step_not_independently_accessible`
  ——不打开父私有子 journal、不扩大 v1 授权门。stagePlan.parents 是 catalog 版本 ID，
  以 `kind=catalog_version` 标注，不伪装成 operationId。
- **作用域（W6）**：callerScope 一律由 `CapabilityHost::callerScope(Agent, currentSession)`
  在 owner 线程生成（runner 侧同源）；实参/游标不参与。服务自带 Read 门
  （`host::checkContext(..., Read)`），写权限不足不阻止只读发现；Read 被拒如实
  `effect_denied`。v1/未知 schema/隔离记录/他域记录一律不进清单。
- **线程模型（W3）**：owner 冻结 `DiscoveryScan{canonical project, operationDir, documentId,
  callerScope, writeClaim}` 后交 `PaleoTaskService` worker；服务经 `shared_ptr` 保活，
  Recovery 销毁后 worker 安全收尾（结果不再投递）。扫描 = 一次
  `OperationLog::snapshotScope`（与既有 get/events 同一把目录互斥、同一次严格加载——
  每快照一次，不逐页重扫；不改写路径）。投影循环每 64 条自查取消；Qt JSON 解析不可
  中途抢占，取消延迟受既有 128 MiB 输入上限约束（未提高）。快照只保留摘要行与关系
  事实，不复制完整 Record/frozenRequest。
- **完成防线（W7）**：`PaleoTask::finished`（SingleShotConnection）里复核 owner 存活、
  宿主身份（document/epoch/Read/工程 canonical 路径）、服务代次与取消窄窗
  （`task->cancelRequested()`），任一失败按独立 code 丢弃。取消只停本查询
  （`requestCancel`/`requestInterruption`），不触任何业务 operation 的 cancelRequested。

## 实现清单

| 文件 | 角色 |
|---|---|
| `src/workflow/operationdiscovery.{h,cpp}` | 新：发现服务（快照/HMAC 游标/预算/关系投影/诊断计数/注入时钟） |
| `src/workflow/capabilityrecovery_discovery.cpp` | 新：`CapabilityRecovery::listOperations`（owner 冻结→worker 扫描→完成复核） |
| `src/workflow/capabilitygateway.h` + `capabilityops.cpp` | 薄缝：`OperationLog::ScopedSnapshot snapshotScope()`（单锁单次加载 + 作用域内摘要）。相对基线**纯新增 48 行**：`readJson`/`loadOperations` 签名与既有读写路径一行未改 |
| `src/workflow/capabilityrecovery.{h,cpp}` | 薄缝：`listOperations` 声明 + `m_discovery` shared_ptr；`invalidateRuntimeHooks` 顺带失效发现缓存；状态机零改动 |
| `src/ai/chat/operationdiscoverytools.cpp` | 新：`paleo.operation_list` 工具声明（effects=Read，operation_id 非必填） |
| `src/ai/chat/operationtools.cpp` | 薄缝：`appendAgentOperationTools` 末尾追加注册调用 |
| `src/workflow/aichattoolrunner_operationlist.cpp` | 新：runner 分支（世代号/callId 守卫） |
| `src/workflow/aichattoolrunner_operations.cpp` | 薄缝：`runPersistentOperation` 在 operation_id 必填校验前加 list 专用分支 |
| `src/workflow/aichattoolrunner_readiness.cpp` | 薄缝：operation_list 精确资格（recovery + TaskService） |
| `tests/tst_h4_operation_discovery.cpp`、`tests/tst_h4_operation_list_tool.cpp` | 新：见下 |
| `cmake/extra-h4-agent-operation-discovery.cmake` + 根 manifest 一行 | 构建接线 |
| `translations/paleo_{zh_CN,en}.ts` | 仅追加本任务 9 条文案（+36 行/文件，未整库重排） |

复用（未改动语义）：G4 隔离/无损 loader（`readRecord`/quarantine/`persistOperations`
写保护）、`journalHealth`、`lookupRecordById` 的三态区分、ProjectQuery 的 HMAC 游标/
queryDigest 口径、`read()` 的异步完成复核范式、既有六工具全部路径。

## 契约（paleo.operation-discovery.v1）

请求（已声明筛选，未声明键不参与读取决策）：`view`（operations|relations，缺省
operations）、`anchor_operation_id`（relations 必填，规范 op-uuid 形态）、
`execution_state`（8 值词表精确筛选）、`capability_id`（≤512B 精确匹配）、
`page_size`（1–32，缺省 16）、`max_bytes`（2048–24576，缺省 8192）、`cursor`。

响应：`operations[]`/`relations[]`、`returnedCount`、`hasMore`、`nextCursor`、
`coverage{documentId, snapshotDigest, journalHealth(ok|incomplete), snapshotPolicy, sort,
matched, pageComplete}`。摘要行字段：operationId/capabilityId/contractVersion/execution/
commitState/presentation/revision/computeCount/cancelRequested/requestHash/resultsAvailable/
resultPorts/sourceOperation/consumers/planSteps/parentVersions/external——全部为已存在的
记录字段或其计数，无任何捏造时间戳。

稳定错误码：`invalid_argument_type/invalid_discovery_view/invalid_execution_state_filter/
invalid_capability_filter/invalid_anchor_operation/invalid_page_budget/cursor_invalid/
cursor_expired/cursor_scope_mismatch/cursor_query_mismatch/cursor_capacity_exceeded/
discovery_cache_full/operation_log_unavailable/operation_not_found/operation_record_quarantined/
page_budget_too_small(+minimumBytes)/discovery_response_budget_exceeded/cancelled/
document_unavailable/stale_session/owner_thread_required/task_service_unavailable/effect_denied`。
错误消息不回显 frozenRequest、externalRequest、路径或无界文本；坏邻居只给
`journalHealth=incomplete`（不带条数）。

## 验证（本地，Linux；ctest -j1）

（构建与测试结果将在最终 head 复跑后填写——见下方"结果"节。）

## 结果

- TODO：构建、ctest、source gates、review 结论（最终 head 复跑后填写）。

## 已知边界

- `coverage.snapshotDigest` 只覆盖**本作用域记录与隔离事实**（同一次加载内按 operationId
  排序的 v2 记录规范序列 + 隔离有无，长度前缀后 SHA-256），刻意不取整本 journal 的哈希：
  journal 是整个工程一份，整本哈希会让当前作用域看到"其他会话/document 的操作是否变化"
  的旁路信号。他人行变化不改变本作用域摘要（`scopeAndDocumentIsolation` 钉住）。
- query() 的「扫描期间 invalidate → stale_session」防线（generation 比对）无确定性测试
  夹具（需故障注入才能稳定命中窗口）；由代码审查与完成侧归属复核双重兜底。

- `--transitive` 的 func/ui 两指标只在 CI lint 车道全闸；本地以受控 QGIS_PREFIX 预跑通过。
- 规模夹具用满足 loader 严格校验的合成 journal 行（含 4000 行全量分页 walk），
  未执行数千次真实科学计算（受影响解析/投影路径与真实记录同一条 loader 代码路径）。
- Qt JSON 整文档解析不可中途抢占：取消延迟受既有 128 MiB journal 上限约束（未提高上限）。
