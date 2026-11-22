# G4：持久 operation 记录、恢复目录与结果生命周期（2026-11-21/22）

- 分支：`codex/g4-agent-operation-recovery`（base `dd73a322` = origin/master 2026-11-22 核实时点）
- worktree：`../paleo-g4-agent-operation-recovery`；构建前缀 `paleo-deb-prefix-20261119/usr`（共享只读）
- 范围：operation 私有命名空间契约、journal 无损读取与隔离、accept/save/CAS 完整性、
  私有目录可信解析、worker/远端附件目录边界、publication/retention 命名空间事实、
  错误可区分性。不重建恢复状态机/提交协议/沙箱/GC。

## 逐工作包记录

### W1 记录与标识契约（src/metadata/operationnamespace.{h,cpp}，数据层）

扫描结果（全部生成器/消费者核对于 dd73a322）：
- operationId 生成器只有两处，恒为 `op-` + `QUuid::WithoutBraces`：
  capabilityops.cpp `accept()`（v1）、capabilityoperationcontrol.cpp `acceptDurable()`（v2）。
  `capabilityseismicjob.cpp:96` 已有同口径 `op-<uuid>` 校验先例。
- ownerAttempt 生成器两处均为裸 UUID（capabilityrecovery_worker.cpp:196、
  capabilityrecovery_external.cpp:25）；已发布 fixture `tests/tst_operation_control.cpp:33`
  使用 `attempt-<uuid>`。两种形态都保留，不强制归一。
- 路径消费点：worker `privateDir=operationDir/<opId>/<attempt>`、external attach
  追加 `result-<uuid>`、publication `.paleo/capability-ops/<opId>/` 前缀、
  retention 私有 path 判定。v1 遗留 `results/<opId>-*` 也消费 operationId，
  由 load 期统一校验覆盖（见 W2）。
- remoteJobId/callerScope/documentId 不参与路径组成，不套用本契约。

契约实现：`isCanonicalOperationId`（op-+36 位小写 hex uuid，共 39）、
`isCanonicalAttemptId`（裸 UUID 或 attempt-+UUID）、`isCanonicalResultLeaf`、
`privateOperationPrefix`、`isInPrivateOperationNamespace`（段精确：`op-a` 不认领
`op-ab`）。拒绝点段/分隔符/绝对形式/控制字符/大小写花括号变体，不 trim、
不归一化、不改写后继续。纯数据，无 I/O。

### W2 无损读取与隔离（capabilityops.cpp、capabilitygateway.h 薄接缝）

- `recordFromJson` → 严格类型解析：与本仓写入口（recordJson）形状不一致的
  字段（非字符串/非数字/非对象/非数组）即整条拒绝，不再经 `toString()/
  toObject()` 默认空值伪装成合法空记录。未知 schemaVersion 只隔离该条
  （不猜测含义、不清空整本）。operationId/ownerAttempt 在此按 W1 契约校验。
- `loadOperations` 两遍式：坏记录进隔离区，邻居照常入表；重复 key/
  operationId 涉事各条全部隔离，不任取赢家。整本 JSON 损坏/截断/symlink/
  超限仍是整本不可用（`operation_log_unavailable`）。
- 写入口无损纪律：隔离非空时 `persistOperations` 拒绝覆写、`accept/
  acceptDurable/saveDurable/cancel` 返回 `operation_log_quarantined`——
  原始 operations.json 字节保持不变；绝不"过滤坏行后写回其余行"。
- 新增兼容 API：`lookupRecordById`（found/operation_not_found/
  operation_log_unavailable/operation_record_quarantined 四态）、
  `journalHealth`。安全 ID 形态不授予任何读取/执行权限；v1 记录继续被
  `authorized()` 的 schemaVersion==2 门槛排除在 Agent get/results/read/resume 之外。

### W3 accept/save/CAS 入口完整性（capabilityoperationcontrol.cpp）

- `lookup`：身份不明的 key 呈现 `operation_record_quarantined`，不借 not_found
  二次立项。
- `acceptDurable`：replay/conflict 事实在隔离邻居在场时仍可读；新根被
  `operation_log_quarantined` 阻断（在 mkpath 之前）。ProjectWriteClaim、
  purge lease、容量、validSeed 预算全部保留。
- `saveDurable`：目标被隔离 → `operation_record_quarantined`；邻居隔离 →
  `operation_log_quarantined`（current 仍回传磁盘现状，不污染内存缓存）；
  proposal 的 operationId/ownerAttempt 按 W1 契约校验 →
  `invalid_operation_owner`。CAS（revision+attempt）、immutable、转换表、
  事件上限语义不变。

### W4 私有目录可信解析/创建（workflow/operationprivatedir.{h,cpp}，功能层）

`createPrivateAttemptDir(projectDir, opId, attempt[, result-leaf])`：
从磁盘规范化工程根出发（不信任调用方 operationDir 字符串别名），逐段
（.paleo/capability-ops/op/attempt/leaf）检查已存在段必须是无链接目录且
规范路径在工程根内；缺失段才由 mkpath 创建；创建后复核（真实目录、无
链接别名、规范路径仍在根内）。错误分层：`invalid_operation_identity` /
`operation_root_unavailable` / `unsafe_operation_directory` / `stage_failed`。
已知限制：复核点之后的并发外部篡改（TOCTOU）无法消灭，窗口收敛到
"复核后、worker 使用前"，由工程写锁边界兜底；Windows junction/保留名
场景未在本机实测（Linux symlink 已实测）。

### W5 接入 worker/恢复/远端附件

- `recovery_detail::run()`：`privateDir` 由直接拼接+mkpath 改为 W4 helper，
  失败在任何 worker/校验之前以分层 code 拒绝。
- `attachExternalResults()`：`result-<uuid>` 叶子同走 helper；身份不合规或
  根被链接别名时不建目录、不调 worker、不落 manifest。
- `start()`/`authorized()`：日志不可读/目标被隔离与"确实不存在"分流。
- 保留：原 operation 与冻结计划恢复、新 attempt 状态机分配、旧 attempt
  回调 CAS、external_unknown 只对原 job、取消晚于提交保留成果、
  工程切换后附件归原工程。

### W6 manifest/读取/retention 命名空间对齐

- capabilitypublication.cpp：`artifactManifest`/`outputsFromManifest` 改用
  `operation_namespace::privateOperationPrefix`（非规范 id 直接不成立），
  不再各自手拼 `.paleo/capability-ops/<opId>/` 前缀；SHA 分块认证、
  allowedManaged（stagePlan 冻结目标）语义不变。
- operationretention.cpp：operationId/ownerAttempt 单独按路径身份契约严格化
  （`operation_retention_unsafe_identifier` → incomplete），其余字段继续用
  通用 `identifier()`；私有工件必须属于本记录的 operation（段精确）；
  共享内容寻址输入束精确限定 `inputs/<sha64>/manifest.json`。保持只读、
  有界、可取消；identity 无法证明 → incomplete → 阻断删除。

### W7 错误可诊断且保留提交事实

- get/events/results/cancel/read/start/noteRemoteAccepted/reportExternal/
  attachExternalResults 全部经 `AuthorizedRecord{code,record}` 传播；
  envelope 只含 machine code（不泄绝对路径/原始请求/坏 JSON）。
- 提交事实：真 receipt + catalog proof 对账不变；journal 写失败/隔离阻断
  时 catalog 已提交事实继续呈现 committed（`committed_log_repair_pending`
  路径保留）；publishing 证据不足维持 commit_uncertain。G5 兼容：现有
  `CapabilityRecovery::get/events/results/read/start/cancel` 签名与
  OperationContract envelope 不变，新增 code 均为兼容扩展。

### W8 验证（tests/tst_agent_operation_namespace.cpp、tst_agent_operation_integrity.cpp）

真实文件 + 生产入口（acceptDurable/saveDurable/start/attach/get/results/
read/operationRetention），目录副作用用文件树快照对比：
- 命名空间契约正反例（含 op-a/op-ab 段边界、变体不归一）。
- 私有目录边界：快乐路径（两种 attempt 形态+result 叶子+幂等）、点段/
  绝对/分隔符拒绝、文件占位、symlink 别名不进入他人空间、只读父目录
  stage_failed、根不可用分层。
- 重启幂等：`attempt-<uuid>` fixture 跨实例重启身份不变（operationId/
  requestHash/key/attempt/revision），Resume 换裸 UUID attempt 走 CAS。
- 磁盘篡改：operationId→`../escape`、attempt→`../sneak`、`op-a/../../escape`、
  stateRevision→非数字、重复 key/operationId、截断 JSON——分别呈现隔离/
  不可用，journal 字节无损，目录树无新增，worker 调用计数为 0（好邻居
  场景恰为 1）。
- 合法 committed 邻居在坏记录在场时：get/cancel_after_commit/results/
  receipt 照常；retention incomplete。
- v1 混合：v1 本地诊断可读、Agent 面不可见；v1 accept/save 遗留契约不变。
- 隔离阻断 saveDurable（current 回传磁盘现状）+ 干净 journal 上非法
  attempt 独立拒绝。
- 既有回归复用：tst_operation_control / tst_capability_recovery /
  tst_capability_recovery_native / tst_capability_gateway 全量本地跑。

## 验证与命令

- OS：CachyOS x86_64 Linux（6.3.4）；Qt/QGIS/GDAL/PROJ 来自共享 deb 前缀。
- 本地构建/测试命令与结果：见提交时 PR 描述（Source gates + ctest 清单）。
- 未实测：Windows junction/reparse/保留名、UNC 盘符行为（平台受限，如实
  报告，不宣称覆盖）。

## 独立复审（R1）与修复

独立 reviewer 审查真实 diff/调用链/验收证据后 **APPROVE**（0 P0/P1，
2 P2），两条 P2 当轮修复：

1. `isLowercaseUuid` 接受缺连字符的 36 位 hex 串（连字符位被 hex 占据仍
   通过）→ 改为连字符位强制 '-'、其余位强制小写 hex；补回归断言。
2. `isInPrivateOperationNamespace` 接受裸命名空间前缀（恰 62 字符、尾 '/'
   后零内容）→ 最小长度收紧到 `opId.size()+24`；既有断言覆盖。

全矩阵复跑后另发现并修正一处实现偏差（R1 修复的连带验证暴露）：
- `storedInput` 初版把共享输入束窄化为 `inputs/<manifestHash>`，漏掉
  wellfaciesinputstore 的生产命名空间 `well-inputs/<uuid>`
  （`tst_storagegovernance` 真实 fixture 拦截）。修正为两个生产者命名空间
  并列白名单（`inputs|well-inputs/<安全段>` + manifest.json 存在性），
  不再要求 directory==manifestHash（well-inputs 的目录名是 UUID；
  身份精确校验留在各自 loader）。同时修正 sharedCapabilityFile 的
  分段数（5 段含 manifest.json）。
- 该套件与 `tst_assetops` 的合成 fixture 使用非规范 operationId
  （`op-retained`/`op-lost-input`/`op-input`）——按 W1 契约现代化为
  `op-<uuid>`（精确归属的 fixture 更新，非断言放宽）。

R1 后全矩阵：namespace 6/6、integrity 12/12、operation_control 15/15、
recovery 37/37、recovery_native 10/10、gateway 18/18、host 12/12、
round2 14/14、assetops 15/15、storagegovernance 15/15+1 既有 skip、
authenticatedartifactreader 18/18、pigenerated 5/5、pythonoperations 17/17。


## 复用与避让

- 复用：managedpath（isSafePathSegment/resolveManagedPath）、
  AuthenticatedArtifactReader、catalog publication proof、CAS/事件模型、
  retention 预算、既有测试故障注入设施（armOperationFileWriteFault、
  setCheckpointHook、QProcess crash child）。
- 未动：G1 llmclient、G2 controller/Pi、G3 domaintools/runner、G5 UI；
  Python Windows sandbox/DACL、Data_service gitlink、Catalog receipt 格式、
  ONNX/科学算法、自动 GC、v1 `execute()` 遗留 `results/` 路径（其
  operationId 已被 load 期契约覆盖，非本任务重建对象）。
