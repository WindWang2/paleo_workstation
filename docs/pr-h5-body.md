## H5：Agent v1 计算计划的提案、原生审阅与显式执行闭环

### 问题

Agent 已有 v1 计划后端（`validatePlan` 整图预检、`CapabilityHost::preparePlan →
start → CapabilityRecovery` 两步图执行、child_operations 溯源），但没有用户
入口：模型无法提出计划，用户无法在原生界面查看步骤/输入/有效参数/端口连接、
修改后重新预检、再显式点击「计算私有成果」，也无法从已知身份回到原操作。

### 实现后行为

- **纯预检提案工具 `paleo.workflow_plan_preview`**（W1/W2）：模型提交 v1
  nodes/edges，工具做**有界严格解码**（封闭词表——节点 7 字段/边 4 字段/顶层
  4 字段之外一律 `unknown_field` 拒绝，approved/caller/documentId/路径等授权
  语义字段进不来；深度/元素/字符串/非有限数预算先于容器扩张）+ 复用唯一
  业务预检 `validatePlan`，返回 `paleo.plan-preview.v1` 有界契约：规范化图
  （**原始提议值**）、effective_parameters（算法补默认单列）、order/terminal、
  稳定 digest、issues（只含验证器真实返回的首个可定位问题）。状态恒
  `not_executed`——提案/预检通过/UI 勾选都不构成执行授权。不 prepare、不
  start、不建 operation、不读模型路径。
- **原生审阅面板**（W3/W4）：计划卡「审阅此计划」入口打开；步骤列表、参数
  表（提案值/当前值/来源三列——**用户修改/提案/算法默认三态分开标注**）、
  边表、终端成果（compute-only 私有产物语义）；IDW 点集 x/y/z/well_id 有界
  表格编辑 + 必填 subinterval_id；每次有效编辑产生新 draft revision，原始提案
  保留可对照。输入诚实性：`points_ref/constraints_ref` 标注「提案值·来源未
  核验」、CRS/单位「宿主未提供核验事实，如实标注未知」、独立文件源明确不
  支持不回退。失败原因常驻状态行；按钮 enabled 与 handler 共用同一判定。
- **展示/执行适配器 `AiPlanPresentation`**（W5/W6）：草案身份 =
  (documentId, graphDigest) 确定性请求身份；执行门比对**预检采纳时绑定的
  上下文**与 live（checkContext(live, captured, Read)）——切工程/换会话/
  编辑图使旧资格失效，重新预检才恢复；显式「计算私有成果」构造宿主拥有的
  ExecuteRequest（compute-only、Caller::Agent + 当前 chat session）走既有
  `preparePlan → start(handle, 原始 intent)`；重复请求同身份 → 宿主幂等重放
  原 operation（computeCount 不增）；**accept 后调度失败**：作为本次显式执行
  动作的结果处理，沿原 clientRequestId lookup 确认已受理的 queued 原 operation
  （不清 ID、不换 ID、不自动重算；历史渲染/打开面板零查询）。
- **原 operation 与恢复**（W7/W8）：成功/失败保留真实 operationId/
  requestHash/execution/commitState/presentation/child_operations；面板「打开
  操作详情」转既有 `AiOperationDetailWidget`（三轴/事件/端口/取消/恢复，不建
  第二套）；「查看原操作」按稳定请求身份 `lookup(scope, clientRequestId)`
  确定性重建（无需 operation_list），来源不足如实不可恢复；晚结果按
  （代次+草案+execGeneration）归属，不跨绑新修订；关闭面板=隐藏视图不取消
  已受理任务。
- **装配**（W9）：`installPlanWorkbench`（幂等）由既有
  `installConversationPresentation` 末尾追加——main.cpp 零改动，主程序/测试
  同源；`extra-h5-agent-plan-workbench.cmake` + 根 manifest 一行。

### 边界（重要）

**新增的是 v1 计划入口，不是 v2 多成果/正式发布**：不依赖 H3 的版本化导出、
H4 的 operation 发现、H1 上下文投影、H2 会话档案；后端（capabilityplan/
capabilitynativeplan/capabilityhost_plan）零改动只读消费。compute-only 成果
标注私有计算产物，不显示「已加入成果库/地图」。

### 复用

唯一业务预检 = `validatePlan`（UI 不手抄校验器/默认值）；执行链 = 既有
Host/Recovery（opaque ticket、幂等、恢复语义原样）；操作详情/取消/恢复 =
G5 既有组件；工具声明/分派/严格外部档 = G3 既有链（readiness 特判与
read_result 同口径：纯预检零宿主依赖，采纳/执行仍走宿主门）。

### 本地验证（CachyOS x86_64，受控 deb 前缀 Qt6/QGIS 4.2.3）

- 新增测试全绿：tst_h5_plan_preview 13/13（解码矩阵/契约诚实性/digest/
  请求身份/工具表/runner 纯路径）、tst_h5_plan_presentation 10/10（真实
  IDW→contour compute-only 全链：child_operations=2、catalog 0 版本、结果
  端口存在、重复重放 computeCount=1、改图新身份+原操作保留、accept 后调度
  失败 lookup 确认 queued、epoch/read-only 拒绝零 start、adapter 重建按稳定
  身份 lookup、晚结果不覆盖新修订）、tst_h5_plan_workbench 7/7（面板交互/
  三态来源/诚实注记/主题窄窗）、tst_h5_plan_assembly 5/5+截图（真实主窗 +
  loopback 假端点回确定性 plan preview tool_call → controller→runner→卡→
  审阅面板全链、tools[] 上送/role=tool 回灌对账、打开期间执行计数零）。
- 相邻回归全绿：tst_capability_plan/host/recovery_native、tst_aichatcapability、
  tst_aichattoolloop（工具表 16→17 断言随新工具更新）、tst_g5_*×4、
  tst_aiassistdock、tst_aitoolresultview。
- Source gates 全绿：layering --strict/--qgs-audit(134 基线不涨)/--transitive、
  file_budget --strict(51 基线不涨)、ui_size/ui_invariants/ui_tokens、
  i18n/l10n/ts_stale(0/0)、qgis_versions、docs_drift、cpp_mainline、qgis_spread(169 基线不涨)。
- GUI 证据：artifacts/h5-screenshots/（浅/深 1280×800 + 窄 720×600，真实
  主窗走查）。
- 独立评审：两轮（第一轮逐 TU 实编发现 5×P0/1×P1/8×P2 全部修复；第二轮
  绑定最终 head）。

### 剩余限制

- 计划入口仅 compute-only、终端成果单一（v1 支持面）；无工程打开时提案可用
  但重新预检/计算被 Read 门如实拒绝（document_unavailable）。
- 100%/200% 高 DPI 仅静态缩放验证（离屏平台限制，无交互截图）；Windows 侧
  未本地验证（CI 车道覆盖）。
- prepare ticket 5 分钟过期/128 容量错误码由宿主透传（适配器不缓存 handle，
  既有后端测试覆盖）。

进度文档：docs/progress/h5-agent-plan-workbench-20261122.md
