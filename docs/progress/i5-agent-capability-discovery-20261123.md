# I5：Agent 使用的原子能力发现、精确契约解释与纯候选参数预检（2026-11-24）

分支 `codex/paleo-i5-agent-capability-discovery-20261123`（基线 origin/master
63b26e78，即调研快照同提交）。实施记录按任务书要求记载实际命令、commit、
覆盖范围与未覆盖项；验证段落随实际运行结果更新。

## 交付概览

新工具 `paleo.capability_inspect`（`paleo.capability-inspect.v1`）——封闭
list/describe/check 三模式：

- **list**：分页目录（稳定 kind→id→version 序），默认 capability+algorithm+
  tool 三类；UI command 全集仅 `filter=commands` 显式给出（W2：不默认混入
  业务能力列表）。游标绑定 filter+目录全序摘要，注册表演进后 `cursor_stale`。
- **describe**：精确 ID+版本读取合同。版本 0=当前登记最高版；给定版本精确
  匹配、不回落（`onnx:tile_classification` v1 请求 → `unsupported_version`）。
  capability 支持 registry 声明别名（冲突不猜测）；sections 封闭词表
  （identity/contract/params/ports/lifecycle/related/route）+ max_bytes 整节
  预算（超限整节省略，coverage 如实，不截半个 schema）。
- **check**：纯候选参数预检。`{tool,arguments}` 走 AiToolSpec 两档既有验证器
  （validateParameters + validateExternalArguments），`{algorithm,
  contract_version,parameters}` 走 validateAndFillParams（effective 只含
  AlgorithmSpec 单一来源默认值）。分层结论 declaration/route/
  parameterAssessment/policyAssessment/runtimeAssessment 相互独立；
  policy 仅复用纯 checkContext/checkRequiredContext（宿主快照由 runner 注入），
  runtime 恒 `not_checked/needs_execution_preflight`。无执行票据/handle/
  operationId。

红线（实现与测试双向钉住）：不 prepare/start、不 Recovery::get/reconcile、
不读用户路径、不装载模型、不运行算法、不触发 UI、不调用 readiness 动态分支。

## 新增文件

| 文件 | 层 | 职责 |
|---|---|---|
| `src/services/capabilitydescription.{h,cpp}` | 数据 | 三注册表纯值投影 DTO（不复制注册表/默认值表） |
| `src/workflow/agentcapabilityinspection.{h,cpp}` | 功能 | list/describe/check 纯检查服务（纯函数 + 可信宿主上下文入参） |
| `src/ai/chat/capabilityinspectiontool.{h,cpp}` | 功能 | 工具声明/schema/共享常量（kCapabilityInspectTool） |
| `src/workflow/aichattoolrunner_capabilities.cpp` | 功能 | runner 分支：owner 线程宿主快照 → 服务 → finishActive |
| `tests/tst_i5_capabilitydescription.cpp` | 测试壳 | 投影逐字段 oracle、多版本无遗漏/不合并 |
| `tests/tst_i5_capabilityinspection.cpp` | 测试壳 | 服务层反例矩阵（见下） |
| `tests/tst_i5_capabilityinspection_wire.cpp` | 测试壳 | 接线 + 真实 Host 零副作用 + loopback 模型闭环 |

## 共享文件最小接缝

- `src/services/algorithmspec.{h,cpp}`：新增只读全版本枚举 API
  `AlgorithmSpecRegistry::allSpecsProjection()`（声明+定义各一处，直接返回
  既有 allSpecs 纯值投影；knownIds/spec/specAtVersion 语义与注册次序不变）。
- `src/ai/chat/domaintools.cpp`：include + `appendCapabilityInspectionTool`
  追加 + dispatchAiTool 精确名分支（先于任何前缀分派）。
- `src/workflow/aichattoolrunner.h`：私有方法声明。
- `src/workflow/aichattoolrunner.cpp`：include；startNext 精确分支；统一
  Host 门对 `paleo.capability_inspect` 的窄豁免（与 read_result/plan preview
  同口径——纯静态查询，Host 缺失时 policy 部分由服务标 unavailable）。
- `src/workflow/aichattoolrunner_readiness.cpp`：纯路径早退（无宿主/工程/ORT
  依赖），不改其他工具资格语义。
- 根 `CMakeLists.txt`：`_paleo_extra_manifest` 追加一行；
  `cmake/extra-i5-agent-capability-discovery.cmake` 登记 sources + 3 测试。

## 与兄弟任务的边界

- 精确工具名分派：不使用 `startsWith("paleo.map_")` 或其他前缀吞并；
  I3 `paleo.map_layer*` / I4 `paleo.map_locate` 按各自 AiToolSpec 注册即可
  被本目录发现（builtinAiToolSpecs 反射），无需 I5 专用注册。
- I1/I2 新算法版本仍走 allSpecs 工厂追加，不依赖 allSpecsProjection。
- 未触碰：CapabilityHost prepare/start、native adapter capture/freeze、
  OperationLog/Recovery、计划执行/exports、算法核/模型加载、I3 呈现、
  I4 导航/选择/nativeviewpermit、AiChatController/H1/H2/H6/H7 缓存实现、
  AppContext/app wiring、P5 编辑生命周期（分支与文件均未触碰）。

## 验证记录（随实际运行更新）

- [进行中] 构建/测试命令与结果。
- [进行中] 源码闸门（layering 族/qgs-audit/spread/file_budget/i18n/docs_drift
  纯 Python 项已预跑通过；transitive 全闸待 QGIS_PREFIX 构建后执行）。
- [进行中] 独立审查记录。

## 已知边界与未覆盖项

- [待运行后填写]
