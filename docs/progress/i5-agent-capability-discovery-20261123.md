# I5：Agent 使用的原子能力发现、精确契约解释与纯候选参数预检（2026-11-24）

分支 `codex/paleo-i5-agent-capability-discovery-20261123`（基线 origin/master
63b26e78，即调研快照同提交）。本文按任务书记载实际命令、commit、覆盖范围
与未覆盖项。

## 交付概览

新工具 `paleo.capability_inspect`（`paleo.capability-inspect.v1`）——封闭
list/describe/check 三模式：

- **list**：分页目录（稳定 kind→id→version 序），默认 capability+algorithm+
  tool 三类；UI command 全集仅 `filter=commands` 显式给出（W2：不默认混入
  业务能力列表）。游标绑定 filter+目录全序摘要，注册表演进后 `cursor_stale`；
  首条即超预算报 `page_budget_exceeded`（不返回空页+原游标）。
- **describe**：精确 ID+版本读取合同。版本 0=当前登记最高版；给定版本精确
  匹配、不回落（`onnx:tile_classification` v1 请求 → `unsupported_version`，
  field 归因 contract_version）。capability 支持 registry 声明别名（冲突报
  `alias_collision` 不猜测）；sections 按目标类型校验适用子集（不适用明确
  `invalid_section`）+ max_bytes 整节预算（单节超限同样省略，coverage 如实，
  绝不超限交付或截半个 schema）。
- **check**：纯候选参数预检。`{tool,arguments}` 走 AiToolSpec 两档既有验证器
  （validateParameters + validateExternalArguments），`{algorithm,
  contract_version,parameters}` 走 validateAndFillParams（effective 只含
  AlgorithmSpec 单一来源默认值）。分层结论 declaration/route/
  parameterAssessment/policyAssessment/runtimeAssessment 相互独立；
  policy 仅复用纯 checkContext（宿主快照由 runner 在 owner 线程注入，
  期待身份来自 runner 绑定），runtime 恒 `not_checked/
  needs_execution_preflight`。无执行票据/handle/operationId。

红线（实现与测试双向钉住）：不 prepare/start、不 Recovery::get/reconcile、
不读用户路径、不装载模型、不运行算法、不触发 UI、不调用 readiness 动态分支。

## 新增文件

| 文件 | 层 | 职责 |
|---|---|---|
| `src/services/capabilitydescription.{h,cpp}` | 数据 | 三注册表纯值投影 DTO（不复制注册表/默认值表；hostEffectNames 单源） |
| `src/workflow/agentcapabilityinspection.{h,cpp}` | 功能 | list/describe/check 纯检查服务（纯函数 + 可信宿主上下文入参） |
| `src/ai/chat/capabilityinspectiontool.{h,cpp}` | 功能 | 工具声明/schema/共享常量（kCapabilityInspectTool） |
| `src/workflow/aichattoolrunner_capabilities.cpp` | 功能 | runner 分支：owner 线程宿主快照 → 服务 → finishActive |
| `tests/tst_i5_capabilitydescription.cpp` | 测试壳 | 投影逐字段 oracle、多版本无遗漏/不合并 |
| `tests/tst_i5_capabilityinspection.cpp` | 测试壳 | 服务层反例矩阵（21 测试函数） |
| `tests/tst_i5_capabilityinspection_wire.cpp` | 测试壳 | 接线 + 真实 Host 零副作用 + loopback 模型闭环（10 测试函数） |

## 共享文件最小接缝

- `src/services/algorithmspec.{h,cpp}`：只读全版本枚举 API
  `AlgorithmSpecRegistry::allSpecsProjection()`（声明+定义各一处；knownIds/
  spec/specAtVersion 语义与注册次序不变）。
- `src/ai/chat/domaintools.cpp`：include + `appendCapabilityInspectionTool`
  追加 + dispatchAiTool 精确名分支（先于任何前缀分派）。
- `src/workflow/aichattoolrunner.h`：私有方法声明。
- `src/workflow/aichattoolrunner.cpp`：include；startNext 精确分支；统一
  Host 门对 `paleo.capability_inspect` 的窄豁免（与 read_result/plan preview
  同口径——纯静态查询；stale_session/host_unavailable 仍生效）。
- `src/workflow/aichattoolrunner_readiness.cpp`：纯路径早退（无宿主/工程/ORT
  依赖），不改其他工具资格语义。
- `tests/tst_aichat.cpp`：工具数钉值 26→27（新工具合法注册）+ 期望名清单
  增补 `paleo.capability_inspect`。
- 根 `CMakeLists.txt`：`_paleo_extra_manifest` 追加一行；
  `cmake/extra-i5-agent-capability-discovery.cmake` 登记 sources + 3 测试。

## 与兄弟任务的边界

- 精确工具名分派：不使用 `startsWith("paleo.map_")` 或其他前缀吞并；
  I3 `paleo.map_layer*` / I4 `paleo.map_locate` 按各自 AiToolSpec 注册即被
  本目录发现（builtinAiToolSpecs 反射），无需 I5 专用注册。
- I1/I2 新算法版本仍走 allSpecs 工厂追加，不依赖 allSpecsProjection。
- 未触碰：CapabilityHost prepare/start、native adapter capture/freeze、
  OperationLog/Recovery、计划执行/exports、算法核/模型加载、I3 呈现、
  I4 导航/选择/nativeviewpermit、AiChatController/H1/H2/H6/H7 缓存实现、
  AppContext/app wiring、P5 编辑生命周期（`codex/paleo-p5-edit-lifecycle-20261119`
  分支与文件均未触碰）。

## 验证记录（实际命令与结果）

环境：独立 worktree `../paleo-i5-agent-capability-discovery`，独立 `build/`；
deb 闭包前缀（h6 维护的 `vendor/prefix`，与 I1/I3 同源接线），显式
`-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`（Arch 工具链不吐 multiarch），
`QGIS_PREFIX=$PWD/vendor/prefix/usr`；`vendor/ranuts-document` symlink 主仓
（configure 报告 `office-editor 已就绪`，无缺失警告）。构建/测试经
`./paleo-dev build|test`（闭包 LD_LIBRARY_PATH/QT_PLUGIN_PATH 注入）；ctest
需 `LD_PRELOAD=/usr/lib/libexpat.so.1`（系统 ctest 与闭包 expat 冲突，I1 同款
处置）。构建并行度 -j4，ctest -j1。

- **I5 专项**：`tst_i5_capabilitydescription` 9 passed / 0 failed；
  `tst_i5_capabilityinspection` 21 passed / 0 failed（含反例矩阵：contours
  Callable+command surface+无独立工具、wellfacies LegacyAdapter+专用工具、
  AiToolSpec-only（read_result）、View 命令仅声明事实、伪造 authority/路径
  零授予、policy/runtime 分离、无 Host policy unavailable、单节超限省略、
  伪造文本 offset 游标、跨类型 section 拒绝）；`tst_i5_capabilityinspection_wire`
  10 passed / 0 failed（工具表/dispatch/readiness/availableTools 真实登记、
  无宿主无工程可查询、顶层伪造授权字段 dispatch 档即拒、严格外部档对本工具
  生效、换绑 stale_session、真实 CapabilityHost 下 freeze/validateInputs/
  taskAdded/operation 目录/catalogRevision 六项零副作用计数、Read-only 翻转后
  原工具仍被 project_read_only 拒（检查不豁免门禁）、loopback 模型五帧闭环
  list→describe→无效 check→修正 check→显式 query_project——末帧以仅真实
  执行才有的 2 口井目录事实区分检查与执行）。
- **回归**（触及面）：`tst_algorithm_contract`、`tst_commandregistry`、
  `tst_agent_tool_readiness`、`tst_agent_readiness`、`tst_aichatcapability`、
  `tst_agent_tool_contracts`、`tst_h5_plan_preview`、`tst_h7_resultwire`、
  `tst_aichat`、`tst_i5_*` 三项——`ctest -R <12 项>` 100% passed（exit 0）。
- **源码闸门**：`ctest -R ^(layering|…|cpp_mainline_selftest)$` 18 项
  100% passed；`python3 tools/check_layering.py --transitive --qgis-include
  vendor/prefix/usr/include/qgis` exit 0（QGIS 头注入的全闸口径，功能层
  QtWidgets 黄牌 0）；qgs-audit 134=baseline、qgis-spread 169=baseline
  （新增 0）；file_budget 51=baseline；i18n/docs_drift 绿。
- **独立审查**：未参与实现的评审 Agent 通读全部 diff（commit 249d348f，
  +2944/-3）并对照宿主契约源。结论：无 P0；1×P1 + 8×P2。逐项处置：
  - F1(P1) describe 单节超预算仍交付且 coverage 谎报 complete → 已修
    （去单节逃逸 + 余量 512；新增单节超限 oracle）；
  - F2 list 空页+不变游标 → 已修（page_budget_exceeded）；
  - F3 游标 offset 静默按 0 → 已修（严格整数 + 伪造游标 oracle）；
  - F4 sections 请求/交付名不对齐、不适用节静默缺失 → 已修（按类型校验
    + params/ports 平铺 + oracle）；
  - F5 失败帧缺 mode → 已修（失败帧补 mode；required 收窄 [contract, ok]）；
  - F6 max_bytes 上限与内联传输上限不符 → 已修（8192 对齐 + 描述同步）；
  - F7 效果名映射双份 → 已修（hostEffectNames 单源）；
  - F8 unsupported_version field 归因不一致 → 已修（统一 contract_version）；
  - F9 验证记录未闭合 → 本节闭合（审查后全量复跑：12 项回归 100%、
    18 项闸门 100%、transitive exit 0）。
  审查确认无问题的面：输入/权限均来自可信宿主（顶层封闭字段集 + owner
  线程快照 + expected 身份来自 runner 绑定）；生产入口链条完整无孤立条目；
  Host 门豁免未放宽其他工具；只读红线（新文件无 QSettings/QFile/QNetwork/
  PaleoTask/prepare/Recovery）；候选形状预算先于验证器；异步/世代守卫复用
  既有屏障；版本/别名语义四类反例齐；未侵入兄弟任务所有权。
- **commit**：`249d348f`（主体）→ `945ee9cb`（审查修复）；rebase 复核见 PR。

## 已知边界与未覆盖项

- `onnx:tile_classification` 当前仅登记 v2（v1 未登记）：「同 id 双版本不
  合并」以真实 v2-only 反例 + 合成双版本清单喂纯投影函数双保险证明；主干
  出现真实双版本条目后 allSpecsProjection 逐条保留无需改动。
- list 游标无 MAC：目录内容为全局静态注册投影（commands 需显式 filter），
  伪造游标最多换取同一公开投影的另一页，无越权面；offset/摘要/上界校验
  挡住错页与回退（F3 修复后含非整数 offset）。
- check 的 policy 只覆盖「当前静态策略」（checkContext/checkRequiredContext
  纯判定）；provider/模型/选择/冻结输入/view permit 等动态前提恒
  not_checked——按任务书不另造第二套 readiness。构建期静态事实（无 ORT
  构建的 requires_local_ort）如实呈现。
- describe 单节仍超 8192 内联预算时省略该节（continuation 指引改请求更小
  节/经 read_result 引用续读）；不在超限下交付。
- 平台覆盖：仅 Linux x86_64（deb 闭包栈）实跑；Windows 车道未运行（本地
  无该环境，如实记录）。Pi 路由未改（本工具不经 Pi 状态机；H7 wire 测试
  的 loopback 原生端点已覆盖工具往返闭环）。
- 兄弟任务（I1/I2/I4 分支 + I3 开放 PR #1076）均未合入：本分支从基线独立
  构建/测试；domaintools/runner/readiness 的接缝在合入时保留双方增量
  （I3 的 mapdisplay 分支与本分支的 capability_inspect 分支互不吞并）。
