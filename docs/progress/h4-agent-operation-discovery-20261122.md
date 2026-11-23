# H4：当前 Agent 作用域的持久 operation 发现与关系分页 — 2026-11-22

## 身份

- 分支 `codex/h4-agent-operation-discovery`，worktree `../paleo-h4-agent-operation-discovery`，
  专属挂点 `cmake/extra-h4-agent-operation-discovery.cmake`（根 `_paleo_extra_manifest` 仅加自己一行）。
- base：`origin/master = 8eca4644db31b4dcfd5ff11bebf39f0bbcfaf3a5`（开工与提 PR 前两次 fetch 均未前移）。
- head：`6cae05dc`（R4 修复轮；最终 PR 以推送时分支 tip 为准）
- 环境：CachyOS x86_64（Linux 7.3.4-1-cachyos），GCC 16，CMake 4.5.1（主机），
  Qt 6.10.2 / QGIS 4.2.3（vendored），RelWithDebInfo，`-j4`，`ctest -j1`。
- 依赖前缀：`vendor/prefix → /home/kevin/projects/paleo-h3-agent-plan-outputs/vendor/prefix`
  （deb 闭包；`./paleo-dev` 每次 configure/build 自行校验 closure 收据与 `vendor/deb-closure.lock`
  一致）；`vendor/onnxruntime`、`vendor/ranuts-document` symlink 主仓（configure 报
  `office-editor: 已就绪`，无缺失警告）；`vendor/Data_service` 按本分支 gitlink `905b5c1b` 初始化。
  该前缀的寿命不受本任务控制（同批记录其曾被删除后恢复），见"已知边界"。
- 相关 PR：#1063（G2）、#1064（G3）、#1065（G4）、#1066（G5）、#1067（G1）、#1068（整合复审）已合入，
  为本任务的复用基线；同批开放 PR #1069（H1）、#1070（H6）、#1071（H3）、#1072（H7）、
  #1073（H5）——接缝见下。
- 交付：`paleo.operation_list`——不要求已知 operation_id 的只读发现工具。Agent 在当前工程 +
  当前聊天作用域内有界列出合法 v2 持久操作摘要（operations 视图），或以已授权操作为锚点分页
  直接关系（relations 视图）；得到 ID 后显式调用既有 get/results/read/resume/cancel。
  发现自身零执行、零恢复、零对账、零 journal 写入，不需要用户先打开任何新面板。

## Scope ledger（开工核对 → 本任务增量）

"已有能力"一列来自对 base 源码的只读核对（非运行复现）；"证据"一列均为本地运行验证。

| 包 | 生产入口 | 已有能力（复用，未改语义） | 本任务增量 | 证据（测试函数） |
|---|---|---|---|---|
| W1 契约与摘要 | `operationdiscoverytools.cpp` 工具声明；`OperationDiscovery::validateRequest` | G3 的 `AiToolSpec`/严格外部档 | 版本化请求/结果（`paleo.operation-discovery.v1`）：7 个已声明参数、筛选词表、页预算域；摘要只取既有安全字段，无时间戳，排序键写入 `coverage.sort` | `toolDeclaredAndDispatchable`、`invalidRequestsAreRejectedBeforeAnyTask` |
| W2 一致受限快照 | `OperationLog::snapshotScope`（薄缝） | G4 严格 loader：`readRecord`、重复身份隔离、受管路径检查、`persistOperations` 写保护 | 单锁单次加载的当前作用域快照 + 作用域内摘要；相对基线纯新增，不改既有读写路径 | `v1QuarantinedAndForeignNeighborsStayOut`、`emptyAndCorruptJournalsAreDistinguished`、`scopeAndDocumentIsolation` |
| W3 预算与生命周期 | `CapabilityRecovery::listOperations` → `PaleoTaskService` worker | 既有任务服务、取消模型、128 MiB journal 上限 | 磁盘扫描出互斥（不阻塞 owner 的 invalidate/续页）；投影每 64 条自查取消；快照/游标/字节上限；缓存满结构化失败 | `bulkJournalPagesWithoutRescan`、`budgetCursorAndCacheDiscipline`、`cancelPreservesJournalAndBusinessState` |
| W4 作用域绑定游标 | `OperationDiscovery` 游标（HMAC） | ProjectQuery 的游标/queryDigest 口径（借鉴，未迁移协议） | `opdisc1.<instanceTag>.<snapshotKey>.<pos>.<expiry>.<HMAC>`；绑定工程/document/caller/查询摘要；TTL 10 min；同快照游标有界回收 | `budgetCursorAndCacheDiscipline`、`bulkJournalPagesWithoutRescan`、`scopeAndDocumentIsolation` |
| W5 直接关系 | `relationRows` 投影 | 记录里既有的 `frozenRequest.sourceOperationId`、`artifactManifest.childOperations`、`stagePlan.parents` | 四类关系行：持久可读目标 / 未解析 / 计划内步骤（标注不可独立 get/resume）/ catalog 父版本（非 operationId） | `relationsClassifyDirectTargets` |
| W6 Recovery API 与工具 | `listOperations`；runner `runOperationList`；readiness 精确资格 | 既有 runner 异步 envelope、`callerScope(Agent, session)`、工具分派 | Read 门；**请求与游标准入在建任务之前**（纯函数 + 互斥内纯内存判定）；`operation_list` 恒走 hosted 资格路径（无宿主不广播） | `readGateAndWriteDenialBoundaries`、`invalidRequestsAreRejectedBeforeAnyTask`、`listGetChainThroughRealRunner`、`readinessGates`、`negativeArgumentsFailClosed`、`emptySessionIsRefusedBeforeAnyTask` |
| W7 失效/取消/回收 | 完成回调复核；`invalidateRuntimeHooks → invalidate + reset` | `read()` 的完成复核范式 | generation 防缓存复活；服务代次检查；取消窄窗；重入回调不清新句柄 | `snapshotImmutabilityWhileBusinessAdvances`、`subscriberDestructionKeepsServiceUsable`、`invalidationDropsInFlightResult`、`cancelPreservesJournalAndBusinessState` |
| W8 健康度与零副作用 | 失败回执 | G4 `journalHealth` 事实 | `journalHealth=ok\|incomplete`（不带他人条数）；独立错误码；错误不回显请求/路径 | `v1QuarantinedAndForeignNeighborsStayOut`、`listDoesNotReconcileWhileGetDoes` |
| W9 规模与闭环 | 上述全部 | 既有 accept/start 生成真实操作与工件 | 冷重启后仅凭 journal 重新发现原 ID → get/results/read 读回原字节 | `discoversScopedOperationsAfterRestart`、`bulkJournalPagesWithoutRescan` |

## 设计与边界

- **快照语义（W4，已选定并写明）**：一次受权读取形成不可变摘要快照，分页从快照顺序取出。
  journal 其后变化不使旧页失效、不混入新行；`coverage.snapshotDigest` 声明快照身份，
  `snapshotPolicy="immutable-snapshot@1"`；需要当前状态时重新发现或调 `paleo.operation_get`
  （工具 description 与每页 `note` 均明说）。会话/工程/epoch 变化（Recovery `live()` 身份检测 →
  `invalidateRuntimeHooks` → `OperationDiscovery::invalidate()` 并 reset 服务）整体失效。
  两种新鲜度策略没有混用：页间不会"偷偷更新"。
- **快照身份只覆盖本作用域**：`snapshotDigest` = 同一次加载内、按 operationId 排序的 v2 记录
  规范序列 + 隔离有无，长度前缀后 SHA-256。刻意不取整本 journal 的哈希——journal 是整个工程
  一份，整本哈希会让当前作用域看到"其他会话/document 的操作是否变化"的旁路信号。
- **游标（W4）**：`opdisc1.<instanceTag>.<snapshotKey>.<position>.<expiry>.<HMAC-SHA256>`，
  常时比较；绑定 canonical 工程、document、callerScope、完整查询摘要（视图/筛选/锚点/页预算，
  缺省归一）与快照身份；不编码路径或其他会话标识，不接受模型提供的 offset。
  MAC 篡改→`cursor_invalid`；过期/未登记/属于已重建的服务实例→`cursor_expired`；换作用域→
  `cursor_scope_mismatch`；换条件→`cursor_query_mismatch`——均不静默回第一页。
  `instanceTag` 是每服务实例一个的公开随机标记：会话/宿主失效会重建服务（新密钥），旧游标据此
  报"不再有效、请重新发现"而不是让模型以为传错了。TTL 10 分钟、快照缓存 8 MiB/4 份。
  游标有界：同一快照只保留"刚消费的可重试游标 + 新发的续页游标"，更早位置重放得
  `cursor_expired`（长遍历不受 maxCursors=128 限制；同页重试幂等）。游标与缓存不跨重启
  （无持久化身份）；重启后重新发现仍取得原 operationId。
- **容量与淘汰（W3/W4）**：份数/字节上限不变；到限时按优先级淘汰——① 其他 callerScope 的遗留
  快照（会话切换后；作用域绑定本就禁止续用）；② 本作用域已无未消费续页的快照（已走完/被放弃/
  首页失败）；③ 本作用域最久未使用的在途分页；同档按 LRU。③ 是有意的取舍：典型用法是
  "list 第一页挑到 ID 就去调 get、不再翻页"，每次 list 都留下未消费续页，若把在途分页当作不可
  淘汰的承诺，操作数 >16 的作用域里 10 分钟内第 5 次 list 必然失败（R3-P1-1）。被淘汰的在途
  游标之后得到**明确的** `cursor_expired`（不静默、不回首页、不混页）——对应任务书"不挤掉已承诺
  仍有效的分页**而不说明**"。只有新快照自身放不进字节上限才腾不出：此时不做无谓淘汰，返回
  `discovery_cache_full` + `retryAfterMs`。首页单行超预算在登记之前判定，不占槽、不触发淘汰；
  其他首页失败撤销登记。
- **排序（W1）**：operations 视图 `operationId-lex@1`（UUID 不编码时间，事件环首项也不必是
  创建事件——时间字段一律不出现，不捏造 createdAt/updatedAt）；relations 视图 `relation-kind@1`
  （source_operation → consumer 按 operationId → plan_step 记录序 → parent_version 记录序）。
  清单里的 execution/commitState/presentation 是**记录事实**，不推导可恢复/可发布权限。
- **关系分类（W5）**：直接持久目标必须是快照内同 document/caller 的 v2 记录；解析不到的来源
  只给 `unresolved_in_snapshot`（不回显目标 ID、不猜状态）。计划内 child 步骤保留 nodeId 与
  记录的 operationId（记录事实），标注 `access=plan_step_not_independently_accessible`——
  不打开父私有子 journal、不扩大 v1 授权门。`stagePlan.parents` 是 catalog 版本 ID，以
  `kind=catalog_version` 标注，不伪装成 operationId。不做任意深度遍历。
- **作用域与准入（W6）**：callerScope 一律由 `CapabilityHost::callerScope(Agent, currentSession)`
  在 owner 线程生成；实参、历史 JSON、游标都不参与。列表服务自带 Read 门
  （`host::checkContext(..., Read)`）——写权限不足不阻止只读发现，Read 被拒如实 `effect_denied`。
  空 callerScope 给独立的 `discovery_scope_invalid`（发现没有"默认作用域"）。
  **无效条件在建任务与任何磁盘读取之前拒绝**：实参准入是纯函数（`validateRequest`），游标
  归属（MAC/到期/登记/快照归属/查询摘要）是互斥内纯内存判定（`validateContinuation`）——
  坏条件、伪造或过期的续页都不占任务槽；`query()` 入口再校验一次作纵深防御。
- **资格广播**：`operation_list` 不经 runner 的 legacy 无条件放行，恒走 hosted 资格路径
  （需 Host + Recovery + TaskService）。无宿主的上下文里既不广播也不可分派——与"广播后调用
  才失败"相比更诚实；既有六个 operation 工具的广播口径未改。
- **线程模型（W3）**：owner 冻结 `DiscoveryScan{canonical project（身份）, 宿主原值 projectDir,
  operationDir, documentId, callerScope, writeClaim}` 后交 `PaleoTaskService` worker；服务经
  `shared_ptr` 保活，Recovery 销毁后 worker 安全收尾（结果不再投递）。worker 不引用活
  Host/Catalog/GUI。`OperationLog` 与既有 get/results 一样拿**宿主原值**的工程路径（词法形态
  一致，受管路径检查才成立）；canonical 只用于快照身份与归属比对——混用两种形态会在工程根经
  符号链接/大小写不同访问时误报 `operation_log_unavailable`（R3-P1-2）。发现任务以 quiet 启动：
  照常登记、可取消，但不触发主窗口任务中心自动露出（否则每翻一页都会弹出任务页）。扫描 = 一次
  `OperationLog::snapshotScope`（与既有 get/events 同一把目录互斥、同一次严格加载——每快照
  一次，不逐页重扫）；磁盘读解析在服务互斥**之外**执行，不阻塞 owner 线程的 invalidate 与
  续页；重入互斥后比对 generation，扫描期间发生失效则不把旧快照登记回缓存。Qt JSON 解析
  不可中途抢占，取消延迟受既有 128 MiB 输入上限约束（未提高上限，未提高 4096 容量）。
  快照只保留摘要行与关系事实，不复制完整 Record/frozenRequest/artifactManifest；
  不持有 ProjectWriteClaim 以外的任何工程级锁/租约。
- **完成防线（W7）**：QPointer 守卫先于 `m_tasks->start()` 构造（`taskAdded` 可能同步销毁
  订阅者或 owner）；`finished` 回调（SingleShot）里复核 owner 存活、宿主身份
  （document/epoch/Read/工程 canonical 路径）、服务代次与取消窄窗，任一失败按独立 code 丢弃。
  取消只停本查询，不触任何业务 operation 的 cancelRequested，不调用全局 task cancel。
- **零副作用（W8）**：全链路无 mkpath、accept/save、reconcile、工件扫描、清理。坏邻居在场时
  沿用 G4 的"合法邻居可读 + 全局 journal 不完整"事实，只给 `journalHealth=incomplete`，
  不输出他人 ID/内容/数量/路径。`journalHealth` 刻意是**工程级**事实而非按本作用域判定：
  被隔离行的身份字段本身不可信，无法可靠归属到哪个作用域——其中任何一行都可能属于当前作用域，
  所以只要 journal 里有隔离行，本作用域清单就不能自称完整（任务书 W8："不得把过滤到合法行
  解释成所有业务操作均已发现"）。R3-P2-3 建议改按本作用域判定，未采纳，理由如上。错误不回显 frozenRequest、externalRequest、密钥、全路径或
  无界异常文本。`paleo.operation_get`/`results` 的对账行为不变（纯 list 零 reconcile 写）。

## 实现清单

| 文件 | 角色 |
|---|---|
| `src/workflow/operationdiscovery.{h,cpp}` | 新：发现服务（请求准入、快照、HMAC 游标、预算、关系投影、诊断计数、注入时钟） |
| `src/workflow/capabilityrecovery_discovery.cpp` | 新：`CapabilityRecovery::listOperations`（owner 冻结 → 准入 → worker 扫描 → 完成复核） |
| `src/workflow/capabilitygateway.h` + `capabilityops.cpp` | 薄缝：`OperationLog::ScopedSnapshot snapshotScope()`。相对基线**纯新增**（一个 include、一个结构体、一个方法声明与实现）；`readJson`/`loadOperations` 签名与既有读写路径一行未改 |
| `src/workflow/capabilityrecovery.{h,cpp}` | 薄缝：`listOperations` 声明、`m_discovery` shared_ptr；`invalidateRuntimeHooks` 顺带失效并 reset 发现服务。accept/start/reconcile 状态机零改动 |
| `src/ai/chat/operationdiscoverytools.cpp` | 新：`paleo.operation_list` 工具声明（effects=Read，无必填项） |
| `src/ai/chat/operationtools.cpp` | 薄缝：`appendAgentOperationTools` 末尾追加一次注册调用 |
| `src/workflow/aichattoolrunner_operationlist.cpp` | 新：runner 分支（世代号/callId/activeContext 守卫，与 resume/read 同款） |
| `src/workflow/aichattoolrunner_operations.cpp` | 薄缝：`runPersistentOperation` 在 operation_id 必填校验之前加 list 专用分支 |
| `src/workflow/aichattoolrunner.h` | 薄缝：`runOperationList` 声明 |
| `src/workflow/aichattoolrunner_readiness.cpp` | 薄缝：`operation_list` 跳过 legacy 无条件放行 + 精确资格（recovery + TaskService） |
| `tests/tst_h4_operation_discovery.cpp` | 新：服务/Recovery 层 20 个用例 |
| `tests/tst_h4_operation_list_tool.cpp` | 新：真实 runner 链 6 个用例 |
| `tests/tst_aichat.cpp`、`tests/tst_aichatprojectquery.cpp` | 守卫同步：内置工具总数 24→25、名单加 `paleo.operation_list`；为无必填项的 `operation_list` 加一条**点名**例外（其余工具仍须至少一个必填项） |
| `cmake/extra-h4-agent-operation-discovery.cmake` + 根 manifest 一行 | 构建接线 |
| `translations/paleo_{zh_CN,en}.ts` | 仅追加本任务 9 条文案（+36 行/文件；手工定位插入，未整库重排） |

## 契约（paleo.operation-discovery.v1）

请求（只接受已声明键；未声明键不参与读取决策）：`view`（operations|relations，缺省
operations；键出现即须取精确词表值）、`anchor_operation_id`（relations 必填，规范 op-uuid
形态）、`execution_state`（8 值词表精确筛选，仅 operations 视图）、`capability_id`（≤512B
精确匹配，仅 operations 视图）、`page_size`（1–32，缺省 16）、`max_bytes`（2048–24576，缺省
8192）、`cursor`。请求体 ≤16 KiB。`documentId`/`callerScope`/路径不是可声明参数。

响应：`operations[]`/`relations[]`、`returnedCount`、`hasMore`、`nextCursor`（无续页时省略）、
`coverage{documentId, snapshotDigest, journalHealth(ok|incomplete), snapshotPolicy, sort, matched,
complete}`、`note`。`coverage.complete`（本页即末页）与同族
`paleo.query_project` 同义：工具结果信封协议会把原生 `coverage` 对象原样提升到信封顶层，投影层
读其中的 `complete` 判定覆盖是否已知（R3-P2-1）。失败时可带 `minimumBytes`、`retryAfterMs`。摘要行字段：operationId/capabilityId/contractVersion/execution/
commitState/presentation/revision/computeCount/cancelRequested/requestHash/resultsAvailable/
resultPorts/sourceOperation/consumers/planSteps/parentVersions/external——全部为已存在的记录
字段或其计数。

稳定错误码（按产生位置分组）：

- 请求准入（建任务前）：`discovery_request_budget_exceeded`、`invalid_argument_type`、
  `invalid_discovery_view`、`invalid_execution_state_filter`、`invalid_capability_filter`、
  `invalid_filter_for_view`、`invalid_anchor_operation`、`invalid_page_budget`、
  `cursor_invalid`、`cursor_expired`、`cursor_scope_mismatch`、`cursor_query_mismatch`、
  `discovery_scope_invalid`
- 宿主门（建任务前）：`document_unavailable`、`effect_denied`、`owner_thread_required`、
  `task_service_unavailable`
- worker / 完成：`operation_log_unavailable`、`operation_not_found`、
  `operation_record_quarantined`、`page_budget_too_small`（+`minimumBytes`）、
  `discovery_cache_full`（+`retryAfterMs`）、`cursor_capacity_exceeded`、
  `discovery_response_budget_exceeded`、`cancelled`、`stale_session`、`discovery_unavailable`
  （worker 未产出结果时的占位，正常路径不可达）
- runner 入口（沿用既有口径）：`host_unavailable`、`operation_host_or_session_unavailable`、
  `task_service_unavailable`

## 验证

全部为本地 Linux 运行验证；命令在 H4 worktree 内执行。

```
./paleo-dev build -j4                                   # 全量 + 逐轮增量；exit 0
./paleo-dev test -j1 -R "tst_h4_"                       # 2/2
./paleo-dev test -j1 -R "<受影响族正则，见下>"            # 86/86 通过
ctest -LE 'core|perf' -R "<无标签闸门项正则>" -j1         # 62/63（first_configure 见下）
```

- 受影响族回归正则：
  `tst_(agent|operation|capability|aichat|project_query|g[1-5]_|tile|wellfacies|python|pi_|aiwiring|algorithm_output|query|lineage|chatresult|toolresult|onnx)|^(agent_python|pi_agent)`
  ——覆盖 OperationLog/Recovery/Host/gateway、工具表与 runner、controller、ProjectQuery、
  G1/G2/G5 以及经同一 runner 分派的 python/tile/wellfacies/Pi 族。
- 主机注记：系统 `ctest` 在 deb 前缀的 `LD_LIBRARY_PATH` 下与 vendored libexpat 符号冲突，
  无法启动。本地用 `/tmp/h4-tools/ctest` 垫片（`ld.so --library-path /usr/lib /usr/bin/ctest`，
  不提交）：只改变 ctest **自身**的库搜索路径，测试子进程继承的环境不变，仍经
  `./paleo-dev test` 的完整受控环境（Qt/QGIS/GDAL/PROJ/ORT 同一前缀、offscreen、每测试独立
  XDG 沙箱）。未使用 `LD_PRELOAD` 绕过（会被测试子进程继承）。

### 结果（最终 head）

| 项 | 结果 |
|---|---|
| 构建 `./paleo-dev build -j4` | exit 0（全量 + 每轮增量均 0 FAILED；ccache 加速） |
| `tst_h4_operation_discovery` | Passed（20/20 用例；ctest 计 22 项含 init/cleanup；~12 s） |
| `tst_h4_operation_list_tool` | Passed（6/6 用例；ctest 计 8 项；~2.7 s） |
| 受影响族回归 | 86/86 通过（`ctest -j1`，187 s；同一正则与首轮一致） |
| 无标签闸门项（ctest，共 63 项） | 62/63 通过；唯一失败 `first_configure` 为**本机工具链环境限制**：其内层全新 configure 不传 `CMAKE_LIBRARY_ARCHITECTURE`，在非 Debian 多架构主机（Arch/CachyOS）上该变量为空 → 资源路径 `usr/lib//qt6/...` 缺多架构段。同一 configure 加 `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu` 即通过（已复现）；护栏脚本相对基线零改动 |
| 源码闸门脚本直跑 14 项（layering --strict/--qgs-audit/--transitive、file_budget --strict、ui_size、ui_invariants、ui_tokens、i18n、l10n、ts_stale、qgis_versions、docs_drift、qgis_spread、cpp_mainline） | 14/14 rc=0（最终 head 复跑；transitive 用受控 `QGIS_PREFIX`） |

### 验收矩阵对照

| 场景 | 可观察行为 | 证据 |
|---|---|---|
| 当前聊天无已知 ID，磁盘已有合法 v2 operation | 重建 Host 后 list 返回同作用域准确 ID；随后 get（committed）、results（1 端口）、read（读回 worker 落盘的 15 字节）均可用；不读聊天历史 | `discoversScopedOperationsAfterRestart`；真实 runner 上的同一闭环 `listGetChainThroughRealRunner`（list→get→results→read，业务 worker 计数保持 1） |
| document A/B、chat A/B、空 session、Read denied | 无跨域记录/其他会话标识/他人 ID；Read 被拒不建任务；空 session 在取 scope 前拒绝、零任务 | `scopeAndDocumentIsolation`、`readGateAndWriteDenialBoundaries`、`emptySessionIsRefusedBeforeAnyTask` |
| v1 与安全 v2 邻居、未知 schema、隔离 ID | v1 不进清单；重复身份把同身份双方隔离（G4 无损纪律）→ 清单空且 `journalHealth=incomplete`，journal 原始字节不变 | `v1QuarantinedAndForeignNeighborsStayOut` |
| 多页遍历接近 4096 操作 | 4000 行一次快照顺序分页：125 页无重复/遗漏；全程 `scanCount==1`（不逐页重扫）；游标 ≤2；保留量 < 源 journal 一半 | `bulkJournalPagesWithoutRescan` |
| 单条摘要超页预算、总缓存满、TTL 到期 | `page_budget_too_small`+`minimumBytes`；`discovery_cache_full` 且旧快照续页不被挤掉；`cursor_expired` 且保留字节归零 | `budgetCursorAndCacheDiscipline` |
| 游标篡改、换条件/视图/会话/工程 | `cursor_invalid` / `cursor_query_mismatch` / `cursor_scope_mismatch`，不返回其他查询页 | `budgetCursorAndCacheDiscipline`、`scopeAndDocumentIsolation` |
| 分页期间业务 operation 正常推进 | 不可变快照：续页 digest 与行状态不混入新值；新查询取得新快照 | `snapshotImmutabilityWhileBusinessAdvances` |
| source operation、同 scope 消费者、legacy child step | 持久可读引用与仅描述步骤分类准确；未解析来源不回显目标 ID；错 scope 锚点 `operation_not_found` | `relationsClassifyDirectTargets` |
| 坏邻居与未知身份污染 | 原 bytes 不改；coverage 不冒充完整；不暴露他人条目 | `v1QuarantinedAndForeignNeighborsStayOut` |
| 准入/TaskAdded/完成回调重绑或销毁 | 无旧结果写入、无悬空；旧回调不清新句柄 | `invalidationDropsInFlightResult`、`subscriberDestructionKeepsServiceUsable` |
| 取消一次发现 | 只撤本查询；业务 journal 字节与业务记录（queued、未 cancelRequested）不变 | `cancelPreservesJournalAndBusinessState` |
| 纯 list 与显式后续 get 的区别 | list 零 reconcile 写（journal SHA 不变）；get 沿旧契约把 publishing 对账回 not_requested | `listDoesNotReconcileWhileGetDoes` |
| 模型工具请求与否定参数 | 工具在工具表可发现、可分派；否定参数全部零任务失败 | `toolDeclaredAndDispatchable`、`negativeArgumentsFailClosed`、`invalidRequestsAreRejectedBeforeAnyTask` |

## 独立复审

| 轮次 | 审查对象 | 结论 | 处置 |
|---|---|---|---|
| R1 | `2f867231`+`26b14943`（独立 reviewer A，全量 diff） | 不可直接合入：5×P0（两个新测试文件里可证为红的断言——当时分支尚未跑过自己的测试）、1×P1（`listOperations` 的 QPointer 守卫晚于 `start()` 构造，`taskAdded` 同步销毁订阅者时是 UAF）、8×P2 | 全部处置：`24ed845f`、`d67a0a13`（守卫顺序、扫描出互斥 + generation 防缓存复活、游标按快照回收、envelope 余量 1 KiB、关系行真实消费者数、view/筛选组合显式拒绝、`m_discovery` reset 使代次检查生效、五处红断言） |
| R2 | `d67a0a13`（reviewer A 复审） | 上轮各项核实已解决；新发现 N-1：末页后 `liveCursors` 断言仍红（2 vs 1）；N-2~N-4 为补充项 | `c4d6d226`：回收循环移出 `hasMore`（末页也只留可重试游标）、新错误码负例、digest 口径与窗口防线文档化 |
| 实跑 | 首次全量构建 + 首次测试运行 | R1/R2 关于"测试未跑过"的判断成立：暴露 1 处编译错误（缺 `QCryptographicHash` include）、4 处夹具错误（`acceptDurable` 不回写入参、状态机须两步、SHA 基线时序）、1 处空指针（`ToolHost` 未分配 `ProjectDirLock`）、1 处字节数笔误、宿主直通校验器缺失 | `b4d4f41b`、`def177ea`、`1fc741cc` |
| 回归 | `1fc741cc` 上 86 个受影响族 | 83/86；3 个失败均为工具表守卫（新工具被枚举） | `e8151242`：产品侧让 `operation_list` 恒走 hosted 资格路径；同步 `tst_aichat`/`tst_aichatprojectquery` 守卫 |
| 自审与实跑自纠 | `e8151242` | 整本 journal 哈希返回给模型 = 他人操作的旁路信号（R1 记为取舍，自审认为应改）；W6"任务前拒绝"未按字面做到；实跑抓到自己两处时序/逻辑缺陷：`makeRoom` 内层循环未在腾够后停止（多淘汰）、LRU 初版用墙钟毫秒（同毫秒打平 + QHash 遍历序不定 → 淘汰顺序随机） | `ba6d8746`（作用域内摘要）、`6cae05dc`（准入前移、单调 useSeq） |
| R4 | `6cae05dc`（独立 reviewer C，未参与前几轮，增量 `e8151242..6cae05dc`） | **可合入**：R3 的 3×P1 与须修 P2 均核实实质修复，未发现新 P0/P1；P2 集中在文档/注释与测试 oracle 表述——头文件 `maxSnapshots` 注释过时、进度文档残留 `pageComplete` 与旧 token 格式、"三条独立 oracle"表述不实、tier0 未与通用 LRU 区分、同步失败载荷缺 `contract`、`retryAfterMs` 含他会话到期（极弱跨会话时序信号）、operations 视图静默忽略锚点 | `6cae05dc` 之后的收尾 commit：修正头文件注释与文档两处契约错误；补只撤 Read 效果、不 reset 服务的 `effect_denied` 变体（单独隔离完成侧上下文/效果复核）；补 tier0-vs-LRU 对照用例（`maxSnapshots=2`，他会话快照让位、同作用域在途存活）；operations 视图下提供锚点改为显式 `invalid_filter_for_view`（不静默忽略，附负例）；同步失败载荷补 `contract`（payloadContract 稳定）；`retryAfterMs` 只取本作用域快照；`queryDigest` 移出锁外。R4 记录的两处与 `read()` 同款的既有残留（task 在 `taskAdded` 内被同步销毁而订阅者仍活时的空错误码；`DiscoveryScan::writeClaim` 注释措辞）记入已知边界，不在本任务改动 |
| R3 | `e8151242`（独立 reviewer B，未参与前两轮，全量 diff，不带前轮结论） | 需修复后复审：无 P0；P1×3——P1-1 快照上限 4 份且只回收过期项，10 分钟内第 5 次无游标查询必失败；P1-2 canonical 工程路径与原始 operationDir 混用，工程根非规范时恒报 `operation_log_unavailable`；P1-3 整本 journal 哈希是跨作用域活动旁路（已在 `ba6d8746` 修）。P2：`coverage` 撞协议保留键缺 `complete`；任务非 quiet 每页弹任务中心；文档错误码缺口；`resultSchemaJson` 偏差；失效后旧游标报 `cursor_invalid` 不准；测试 oracle 缺口（`msleep` 时序依赖、取消用例未执行扫描、`consumers` 无断言、两处修复无回归用例）；`journalHealth` 为工程级位 | `6cae05dc`：P1-1 三级 LRU 淘汰（他会话遗留 → 已走完 → 最久未用在途，单调 useSeq；被淘汰在途游标得明确 `cursor_expired`；`discovery_cache_full`+`retryAfterMs` 仅在新快照自身放不下时；首页失败撤销登记、首行超预算登记前判定）；P1-2 宿主原值工程路径 + 符号链接用例；P2-1 `coverage.complete`；P2-2 quiet 任务；P2-4 文档错误码补齐；P2-5 schema 对齐；P2-8 游标实例标记（`cursor_expired`/`cursor_invalid` 分流）；P2-6 测试补齐（扫描内取消、上下文变化三变体、taskAdded 内销毁订阅者、consumers、去 msleep）；P2-3 不采纳并写明理由 |

## 与同批开放 PR 的接缝（提 PR 时核对）

- `CMakeLists.txt` `_paleo_extra_manifest`：五个 PR 各加自己一行，后合入者有一处平凡冲突。
- `aichattoolrunner.h`：H5（#1073）的 `runPlanPreview` 与本任务的 `runOperationList` 插在同一
  位置（`runPersistentOperation` 声明之后），后合入者需两者都保留；H7（#1072）的插入点不相邻。
- `aichattoolrunner_readiness.cpp`：H5 在文件顶部加提前返回，与本任务改的 legacy 放行条件不在
  同一 hunk，语义不交互。
- `translations/*.ts`：H5/H7 各自追加文案，本任务只追加 9 条。
- **工具表名单守卫**（`tst_aichat`、`tst_aichatprojectquery`）：本任务置 25 并加
  `paleo.operation_list`。H5 同样向内置表加了工具但未改这两处——两者都合入后须为 **26** 且
  同时含两个工具名。本任务不修改 H5 的文件。
- 未依赖任何兄弟分支的新 API/commit/夹具；验证在仅含本分支提交的主干 worktree 上完成。

## 已知边界与未覆盖项

- **依赖前缀寿命**：`vendor/prefix` 指向 h3 worktree 的 deb 闭包。若该 worktree 被删除，本
  worktree 的重建会失败（已构建产物与测试结果的取得时点前缀有效）。复现验证时应按
  BUILDING.md 换成自有前缀并重新 configure。
- **构建树体积**：RelWithDebInfo 全量 DWARF 树约 272 GB（同批公告所述现象）。按公告未中途
  改 `-g1`（会全量重编）；最终 head 验证完成后整目录删除。
- **无确定性夹具的窗口**：① "扫描期间 invalidate → `stale_session`"的 generation 防线需要
  故障注入才能稳定命中，由代码审查与完成侧归属复核兜底；② "读取进行中 journal 被 QSaveFile
  原子替换"只以顺序方式验证（取页 → 替换 → 续页），单次整读的原子性依赖既有 loader（G4 已验），
  未做并发压力测试；③ Qt JSON 整文档解析不可中途抢占。
- **取消的交付语义**：`cancelPreservesJournalAndBusinessState` 用 `taskAdded` 同步置取消，
  确定性走"取消先于首个检查点"分支；"取消晚于 worker 最后一次自查"的窄窗由完成侧
  `cancelRequested()` 复核兜底，与 `read()` 同款，未单独构造。
- **游标并发的 last-wins**：同一刚消费游标被并发双发时先发的会被回收（串行单 agent 工具下
  不会出现）。
- **与 `read()` 同款的既有残留**（非本任务引入，记录在案）：任务若在 `taskAdded` 内被同步
  销毁而订阅者/owner 仍存活，`listOperations` 会以空 `*error` 返回（宿主不可恢复，正常路径
  不可达）；`DiscoveryScan::writeClaim` 注释称"用于受管路径钉住"，实际读路径的钉住由
  projectDir + managedpath 完成，claim 仅为保持与既有 RecoveryHost 结构同构。
- **规模夹具**：4000 行为满足 loader 严格校验的合成 journal 行，未执行数千次真实科学计算；
  解析/投影路径与真实记录是同一条 loader 代码路径。真实 accept/start 生成操作与工件的闭环由
  重启用例覆盖（少量操作）。
- `--transitive` 的 func/ui 两指标只在 CI lint 车道全闸（ctest 项不注入 QGIS_PREFIX）；本地以
  受控 `QGIS_PREFIX` 直跑脚本通过。
- 未做 Windows 平台验证（本机 Linux）；H4 新增代码不触碰平台相关路径。
- 未改 UI：`operation_list` 结果经通用有界卡片呈现（顶层无 `operationId`、契约非
  `paleo.operation.v2`，不会被投影成单条操作卡）。按任务范围未做原生 Qt 视觉验收。
