# H5：Agent v1 计算计划的提案、原生审阅与显式执行闭环

- 日期：2026-11-22/23（批次 2026-11-22）
- 分支：`codex/h5-agent-plan-workbench`（worktree `../paleo-h5-agent-plan-workbench`）
- 基线：origin/master = `8eca4644db31b4dcfd5ff11bebf39f0bbcfaf3a5`（#1068 后）
- 构建清单：`cmake/extra-h5-agent-plan-workbench.cmake`（根 manifest +1 行）
- 环境与验证记录见文末「本地验证」；截图见 artifacts 说明。

## 1. 交付边界（一句话）

打通「模型提出 v1 计划 → 原生面板审阅/修改/重新预检 → 用户显式点击
计算私有成果 → 既有 CapabilityHost::preparePlan → start →
CapabilityRecovery 执行 → 原 operation 事实与已知身份恢复」的闭环；
**新增的是 v1 计划入口，不是 v2 多成果/正式发布**（H3 的能力不在本分支）。

## 2. 生产实现

### 2.1 纯预检提案工具（W1/W2）

- `src/ai/chat/planpreviewtool.{h,cpp}`：`paleo.workflow_plan_preview`
  声明（title/note/nodes/edges 四参数、resultSchema= paleo.plan-preview.v1、
  effects=Read）；共享常量（32 节点/64 边/深度 8/字符串 4096/对象键 64/
  数组元素 512）与 runner/UI 编辑器同源。
- `src/workflow/aichattoolrunner_plan.cpp`（`runPlanPreview`）：decode +
  validatePlan + 有界契约。纯预检零业务执行：不 prepare、不 start、不建
  operation、不读模型路径；聊天消息/结果缓存按既有协议保存。
- 严格解码器（`aiplanpresentation.cpp` 内 `decodePlanPreviewArguments/
  decodePlanPreviewGraph`）：封闭词表（节点 7 字段/边 4 字段/顶层 4 字段，
  一切未知字段——含 approved/caller/documentId/publishIntent/路径——在
  解码层即拒）；预算先于容器扩张；数值必须有限；null/undefined 拒绝；
  contract_version 必须原生整数。
- 提案契约：`{contract, tool, ok, status:"not_executed", title?, note?,
  graph{nodes,edges}, effective_parameters?, order?, terminal?, digest,
  issues?}`。issues 只含 validatePlan 真实返回（首个可定位问题），不伪造
  完整问题集合；预检通过时 graph 回显验证器归一结果（含 effective 参数），
  失败时回显解码归一值。
- digest：规范化图按节点 id/边四元组排序 + Qt JSON 对象键序（字典序）
  做 SHA-256——与提交顺序无关；改图/换参即换身份。
- 工具表接线：`domaintools.cpp` append + 专属 dispatch 分支（Routed）；
  **不挂 isAiCapabilityTool**（该族经 runChatTool 自动执行能力调用）。
- readiness：与 `paleo.read_result` 同口径的独立分支（纯预检零宿主依赖，
  decode+validatePlan 无 I/O）；采纳/执行仍由 aiplanpresentation 走宿主
  Read/effects 门，不因该分支放宽。`startNext` 的宿主 checkContext 豁免
  同步扩展到本工具（与 read_result 同一清单）。

### 2.2 展示/执行适配器（W5/W6）

`src/workflow/aiplanpresentation.{h,cpp}`（paleo::chat::AiPlanPresentation，
G5 AiOperationPresentation 同族纪律）：

- 草案（Draft）：`openProposal(payload, toolCallId)`——校验契约/digest/
  完整图（截断 envelope 拒绝为 `plan_graph_unavailable`）；同 toolCallId
  重开复用草案（保留用户修订与已知操作事实）；打开即预检态但
  **执行资格恒为 preflight_required**（提案/预检/勾选都不是执行授权）。
- 编辑：`updateDraftGraph`（同一严格解码器 + 16 KiB 编辑预算）→ 新修订；
  编辑使执行资格失效（graph_edited），原始提案图保留可对照。
- 重新预检：`requestPreflight`——owner 线程 + QPointer + 非空当前 chat
  session + `host::checkContext(live, captured, Read)` 门后纯校验；成功即
  显式绑定当前上下文（documentId/epoch/session）。
- 显式计算：`requestCompute`——门比对**预检绑定上下文**与 live（不是动作
  时刻现值自比）；重建 ExecuteRequest（publishIntent=compute-only、
  clientRequestId = SHA-256("paleo.plan-exec.v1|document|graphDigest")，
  确定性可重建）；`preparePlan → start(handle, 原始 intent)`（ticket 拥有
  整图，执行不可替换 graph；重复请求同身份 → 宿主幂等重放，computeCount
  不增）。
- accept 后调度失败（空 task/启动错误）：作为本次显式执行动作的结果处理，
  沿原 clientRequestId `lookup(scope, id)` 确认已受理的 queued 原 operation
  （acceptedNote「已受理（调度暂不可用）」）；查询无权/失败 →「受理状态待
  确认」（保留请求身份与已知 ID，不换 ID、不自动重算）。**不是历史卡自动
  查询的例外**——历史渲染/open 的 prepare/start/lookup 计数恒零（内省计数
  prepareAttempts/startAttempts 为测试 oracle）。
- 查看原操作：`requestOriginalOperation`——已知 operationId 直接 `get`，
  否则按稳定请求身份 `lookup`（确定性重建，无需 operation_list）；来源不足/
  不存在 → 如实不可恢复，不自动发起新计算冒充恢复。
- 失效：document/session 信号 + 代次；epoch 变化由门在动作时裁决
  （stale_session，计数不增）；草案保留可读（失效≠删除用户输入）。
- 晚结果：按（代次 + 草案 + execGeneration）接收；编辑后的新修订不被旧
  请求的迟到结果覆盖。

### 2.3 原生审阅面板（W3/W4）

`src/ui/ai/aiplanreviewwidget.{h,cpp}`：

- 结构呈现：步骤列表（id·capability→output_port）、选中节点事实行、
  参数表（参数/提案值/当前值/来源四列——**用户修改/提案/算法默认三态
  来源分开标注**，默认值来自 AlgorithmSpec 不手抄）、边表（from.port→
  to.port + 输入键）、终端成果（compute-only 私有产物语义）。
- 有界编辑：参数当前值列（按提案/生效值类型回写，非有限数值拒绝且原因
  常驻状态行）；IDW 点集 x/y/z/well_id 表格（≤256 行，加/删行）；必填
  subinterval_id 行内编辑。每次有效编辑 → 新 draft revision。
- 输入诚实性：points_ref/constraints_ref 标注「提案值·来源未核验：字符串
  引用不构成宿主核验的 Catalogue 版本证明」；CRS/单位「宿主未提供核验
  事实，如实标注未知」；contours 栅格输入「由图内上游边供给；本入口不
  支持独立文件源，不提供任意文件打开回退」。
- 状态与动作：状态胶囊（待预检/预检未通过/通过·尚未执行/通过·执行资格
  失效(原因)/计算中）+ 受理注记；按钮 enabled 与 handler 共用
  `computeAllowed` 同一判定；「重新预检 / 计算私有成果 / 查看原操作 /
  打开操作详情（转 dock 既有 AiOperationDetailWidget）/ 关闭」。
- 主题：PaleoTheme tokens/capsule；ApplicationPaletteChange 重建。
  关闭面板 = 隐藏视图，不取消已受理 operation（取消走原服务）。
- 拒绝态：截断/缺图载荷呈只读「无法审阅 + 拒绝码」，不拼凑可执行草案。

### 2.4 卡片入口与装配（W3.1/W9）

- `AiConversationCard`：plan-preview 结果且 payload 带完整图时呈现
  「审阅此计划」→ `planReviewRequested(toolCallId, payload)`；历史 envelope
  缺图不给入口（面板如实拒绝）。
- `AiAssistDock`：`attachPlanPresenter`（幂等/null 解绑）+ m_planReview 挂
  主布局（默认隐藏）；`handleCardPlanIntents` 薄接线；clearToolCards 隐藏
  面板。**refreshSessions/发送/草稿/会话存储未动（H2 领地）**。
- `src/app/aiplanwiring.{h,cpp}`：`installPlanWorkbench`（findChild
  objectName 幂等；依赖全部来自既有组装根）；由
  `installConversationPresentation` 末尾追加调用——生产入口 main.cpp:397
  零改动，主程序/测试同源。

## 3. 测试（tst_h5_*，extra fragment 登记）

| 测试 | 覆盖 |
|---|---|
| tst_h5_plan_preview | 解码矩阵（未知字段/授权字段/深度/非有限数/类型/预算）、契约诚实性（issues 只含真实返回、无效图仍回显、状态恒 not_executed）、digest 顺序无关/改图敏感、请求身份确定性、工具表声明/路由/非自动执行、runner 纯路径（无宿主）成功与解码失败两态、16 KiB 级参数回显 ≤64 KiB |
| tst_h5_plan_presentation | 真实宿主链：打开/预检零执行计数；真实 IDW→contour compute-only（child_operations=2、commitState=not_requested、catalog 0 版本、结果端口存在、computeCount=1）；重复请求重放原 operation；改图新身份+原操作保留；**accept 后调度失败 → lookup 确认 queued + 显式查看原操作**；epoch 失效拒绝且计数不增+重预检恢复；read-only 拒绝且 start=0；adapter 重建后按稳定身份 lookup；晚结果不覆盖新修订 |
| tst_h5_plan_workbench | 面板：完整渲染（步骤/参数三态来源/点表/诚实注记/终端）、预检→计算可用、编辑→失效→重预检恢复、非法数值编辑拒绝且原因常驻、截断载荷只读拒绝态、深浅主题/窄窗、adapter 解绑/重挂 |
| tst_h5_plan_assembly | 真实主窗：installConversationPresentation 追加计划装配、双 presenter 幂等重装、**loopback 假端点回确定性 plan preview tool_call → 真实 controller→runner→卡→审阅按钮→面板打开**（tools[] 上送/role=tool 回灌对账、打开期间执行计数零）、PALEO_H5_SHOT_DIR 截图走查 |

相邻回归（按触及面）：tst_capability_plan / tst_capability_host /
tst_capability_recovery_native（v1 后端契约不变）、tst_aichatcapability /
tst_aichattoolloop（工具表/回合影响）、tst_g5_*（卡片/详情/装配不回归）、
tst_aiassistdock / tst_aitoolresultview。

## 4. 复用与不重做

- 唯一业务预检 = `validatePlan`（UI 不手抄第二份校验器/默认值）。
- 执行链 = 既有 `CapabilityHost::preparePlan → start → CapabilityRecovery`
  （opaque ticket、幂等 lookup、requestHash、child_operations、恢复语义
  原样）；本任务零后端改动（capabilityplan/capabilitynativeplan/
  capabilityhost_plan 只读消费）。
- 操作详情/取消/恢复 = 既有 `AiOperationPresentation/
  AiOperationDetailWidget`（面板只转发 operationId）。
- 会话/卡片/分页/草稿 = G5 既有组件；工具声明/分派/严格外部档 =
  G3 既有链。

## 5. 明确不做（防重派）

v2 多导出/typed node result（H3）、operation_list 发现（H4）、上下文投影
（H1）、会话档案（H2）、通用 DAG 拖拽编辑平台、独立 contour 文件源、
Python/depth/seismic 能力进计划、正式科学发布/MapVersion、输入法/Enter
行为改动、G5 卡框架/历史归属修复。

## 6. 已知边界

- 计划入口仅 compute-only；终端成果单一（v1 支持面）。
- 无工程打开时：工具可提案（纯预检），面板重新预检/计算被 Read 门如实
  拒绝（document_unavailable）。
- prepare/start 同步衔接，ticket 5 分钟过期/128 容量对本适配器不可达
  （不缓存 handle；错误码透传由宿主既有测试覆盖）。
- 历史 envelope 若被预算降级缺图：只读拒绝态（不补造）。

## 7. 本地验证

- OS：CachyOS x86_64（linux 7.3.4-1）；Qt6/QGIS/GDAL/PROJ/GEOS/ORT 全部
  来自受控 deb 前缀（vendor/prefix → h3 worktree 闭包 symlink，
  deb-closure.lock 收据一致）；QGIS_PREFIX=/home/kevin/projects/paleo-h3-
  agent-plan-outputs/vendor/prefix/usr；office-editor 接线就绪。
- 构建：`PALEO_JOBS=4 ./paleo-dev build`（heavy-build 主机级互斥，
  -j4 上限，ctest -j1）。
- Source gates（本地全绿）：layering --strict / --qgs-audit（134 基线
  不涨）/ --transitive（QGIS_PREFIX 受控）、file_budget --strict（51 基线
  不涨）、ui_size / ui_invariants / ui_tokens、i18n / l10n / ts_stale
  （0/0 基线）、qgis_versions、docs_drift、cpp_mainline、qgis_spread。
- 测试与截图结果（2026-11-23，本地真实运行）：
  - 新增 4 测试全绿：tst_h5_plan_preview 13/13、tst_h5_plan_presentation 11/11、
    tst_h5_plan_workbench 8/8、tst_h5_plan_assembly 5/5（1 skip=截图用例，
    设 PALEO_H5_SHOT_DIR 后实跑并产出截图）。
  - 相邻回归全绿：tst_capability_plan / tst_capability_host /
    tst_capability_recovery_native / tst_aichatcapability /
    tst_aichattoolloop（工具表计数 16→17 随新工具更新断言）/
    tst_g5_conversation_projection / tst_g5_operation_presentation /
    tst_g5_conversation_workbench / tst_g5_assembly / tst_aiassistdock /
    tst_aitoolresultview。
  - 测试执行方式：./run-target-tests.sh（vendored 前缀环境注入；每测试
    独立 mktemp 日志）；构建 PALEO_JOBS=4 ./paleo-dev build（Ninja +
    RelWithDebInfo；重型构建经主机级互斥锁 -j4）。
  - GUI 证据：artifacts/h5-screenshots/{h5-assembly-light,h5-assembly-dark,
    h5-assembly-narrow}.png（1280×800 浅/深 + 720×600 窄；真实主窗 AI 页签
    走查：loopback 工具提案→卡片→审阅面板→用户点值编辑→修订呈现）。
- 评审与迭代：独立 reviewer 全 diff 审查（逐 TU 实编），P0×5/P1×1/P2×8
  全部修复；随后真实构建/测试迭代修复 4 个运行期问题（payload 原始提议值
  回显、单实例状态胶囊、参数当前值=将提交值语义、编辑后请求身份重 derive）。
- 环境事件记录：h3 worktree 收尾清理导致共享 deb 前缀悬空，已按同 lock
  （逐字节一致）以 h6 前缀重建原路径 symlink（协调目录有记录）；本 build
  的二进制 strip -g 以控制磁盘（938G 卷曾满）。

## 8. PR 说明要点

新增的是 **v1 计划入口**（提案/审阅/显式 compute-only 执行/原 operation
恢复），不是 v2 多成果或正式发布；H3/H4/H1/H2 的能力不被依赖。
