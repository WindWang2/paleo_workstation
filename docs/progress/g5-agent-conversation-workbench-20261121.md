# G5：Agent 会话工作台 — 上下文归属、结果回放与原操作控制

- 分支：`codex/g5-agent-conversation-workbench`；worktree `../paleo-g5-agent-conversation-workbench`
- Base：`origin/master@dd73a322da570e5ad9ce1ddc6b3c1042c5c24788`（2026-11-21 快照）
- 构建前缀：`QGIS_PREFIX=/home/kevin/projects/paleo-deb-prefix-20261119/usr`（共享只读）

## 一、scope-ledger（源码核对 2026-11-22，均为源码事实、未运行复现）

| WP | 主干现状（dd73a322） | 待验证缺口 | 本任务增量 | 验收 oracle |
|----|----------------------|-----------|-----------|------------|
| W1 上下文摘要 | `projectBrief` 已由 aiwiring.cpp:235 / appcontext.cpp:292 计算注入；`hostContextIdentity()` 有 documentId/epoch/generation/taskSession；`host::CallContext` 含 horizon/unitId/selectionIds/readOnly/open | dock 完全不展示简报与归属；无「当前宿主上下文 vs 回合冻结归属」区分 | `src/ui/ai/aicontextbar.*` + controller `projectBriefChanged` 信号与 const getter（薄缝） | tst：简报/只读/回合归属展示与刷新；切工程/重挂后取当前值 |
| W2 历史工具卡 | `renderSession()` 对 role=Tool 只放「结果已回传模型（N 字符）」（aiassistdock.cpp:376-383）；实时卡直连 runner `toolFinished` | 历史结果卡不恢复；归属不标注；raw vs envelope 差异未呈现 | `src/workflow/aiconversationpresentation.*`（只读投影）+ dock 历史卡恢复（同一 renderer） | tst：loadSession 恢复卡（envelope 投影、工具名/参数/时间），模型请求数=0，旧记录「归属未验证」 |
| W3 envelope 统一呈现 | AiToolResultView 只认 native payload；解析失败→空 object（呈现为成功空结果）；无 transport/coverage 事实 | envelope `paleo.tool-result.v1` 不可解读；坏 JSON/标量/数组无降级 | `projectToolResult` 归一化 + AiToolResultView envelope/降级分派 | tst：envelope→data 正常表格+事实行；坏 JSON 非成功；transport incomplete ≠ 业务失败 |
| W4 operation 三轴卡 | `chatToolResponse`（capabilitychatprotocol.cpp:117）已把 contract/revision/computeCount/cancelRequested/commitReceiptRef/resultRef/execution/commitState/presentation 带进工具出参；operation 工具结果走通用 Fallback | 无三轴状态卡；查询 ok=true 会被当业务完成 | AiToolResultView 新增 Operation 分派 + 卡内三轴胶囊与事实 | tst：三轴显示；committed+取消不抹已提交；cancel_after_commit/commit_uncertain 可解释 |
| W5 操作控制意图 | `runPersistentOperation`（aichattoolrunner_operations.cpp）示范 scope=`host->callerScope(Agent, session)` 后 get/events/results/cancel/start(resume)/read | UI 无显式查询/取消/恢复入口；无捕获-校验-代次纪律 | `src/workflow/aioperationpresentation.*`（QObject adapter，捕获 live context、`checkContext(Read)`、非空当前会话、忙碌去重、代次丢弃） | tst：真实 host/recovery 走通六路动作；拒绝路径不触服务；双击恢复去重 |
| W6 事件/端口检视 | `events` 有 afterRevision/nextRevision 十进制游标、event_history_expired；`results` 出 portId/kind/unit/axis/byteSize/sha256/diagnostic；`read` 24KiB 有界 base64 页 | 无原生详情区；游标/翻页/内容检视缺失 | `src/ui/ai/aioperationdetailwidget.*`（分页事件表+端口表+有界内容预览） | tst：游标翻页不静默重置；过期显窗口；跨页 UTF-8 边界；代次丢弃 |
| W7 草稿/键盘生命周期 | 输入框 placeholder 称「Enter 发送」但无 keyPressEvent（实际 Enter 换行，文案与行为不符）；`sendCurrentText` 先 `m_input->clear()` 再 `sendUserText`（拒绝时文本丢失） | Enter/Shift+Enter/IME/重复键；草稿按会话保存恢复；拒绝不吞草稿 | dock 键盘事件过滤 + 每会话草稿 + 接受后清理 | tst：未配置发送草稿保留；切换 round-trip；autoRepeat 不重发 |
| W8 有界呈现/可达性 | m_cards 直接加入布局，卡全量即建；固定 token 样式不随主题刷新 | 500 结果历史全量建卡；主题切换不刷旧卡；窄 dock/键盘可达 | `src/ui/ai/aiconversationcard.*`（惰性展开+展开上限+palette change 刷新） | tst：500 历史结果时活跃 widget 有界、展开可达；浅暗切换刷新既有卡 |
| W9 真实装配 | `attachAiAssistant`（attach_shell.cpp:619）幂等接底栏；main.cpp:393 接线；tst_commandshell 有真实主窗测试先例 | adapter 未装配；回归/GUI 证据未做 | `src/app/aiconversationwiring.*` + main.cpp 一行 + tst_g5 装配测试 + 截图 | 真实 AppContext+主窗走 AI 页签闭环；回归 tst_aiassistdock 等 |

## 二、执行记录

### 新增组件（生产入口）

| 文件 | 层 | 职责 |
|---|---|---|
| `src/workflow/aiconversationpresentation.{h,cpp}` | 功能 | W2/W3 归一化投影：`projectToolResult`（raw/envelope/坏 JSON/数组/operation 包裹 → ToolResultFacts）、`projectSessionTools`（role=Tool ↔ assistant.toolCalls/hostToolBindings 只读关联）、`decodeUtf8Display`（跨页 UTF-8 边界）。零 QtWidgets、零执行授权 |
| `src/workflow/aioperationpresentation.{h,cpp}` | 功能 | W5 adapter：显式 refresh/events/results/read/cancel/resume；每动作捕获当前 context+非空会话 id，调用前 QPointer 存活校验 + `checkContext(live, captured, Read)` 外层门 + `callerScope(Agent, session)`；同步可重入后复验；异步按请求代次丢弃；per-(op,action) 忙碌去重 |
| `src/ui/ai/aiconversationcard.{h,cpp}` | 视图 | W2/W8 统一卡：实时三态+历史 envelope 同 renderer；归属标注（turn id/绑定验证/旧记录未验证）；body 惰性构建；palette change 重建 token 样式；operation 控制行（意图外发）；无 adapter 时给原因 |
| `src/ui/ai/aicontextbar.{h,cpp}` | 视图 | W1 上下文条：简报一行式+可展开详情（层位/单元/只读/回合归属/冻结身份说明）；显示值是快照非权限 |
| `src/ui/ai/aioperationdetailwidget.{h,cpp}` | 视图 | W6 详情面：三轴+事实表、事件分页（十进制游标/过期显窗口/显式重读才复位）、端口表、有界内容预览（文本限已知 encoding+UTF-8 边界；二进制 hex 有界）；控制按钮忙碌反馈 |
| `src/app/aiconversationwiring.{h,cpp}` | 组装根 | W9 装配：`installConversationPresentation`（依赖全来自既有 ctx accessor；幂等，objectName 查找复用） |

### 修改接缝（按所有权边界）

- `src/workflow/aichatcontroller.h`：`setProjectBrief` 变更时发 `projectBriefChanged`；新增 const 快照 getter `hostTurnId()`/`turnContext()`（按值返回，不赋权限）。**仅此两类增量**。
- `src/ui/ai/aiassistdock.{h,cpp}`（主所有者）：ToolCard struct → AiConversationCard；`renderSession` 恢复历史工具卡（projectSessionTools + 有界窗口 64 + 惰性卡）；Enter/Shift+Enter/IME/autoRepeat 键盘过滤；每会话草稿（接受判据=本条 user 消息已入历史）；上下文条+详情面+adapter 注入（幂等）；主题 changeEvent 刷新；展开上限 8（LRU 收敛）；流式增量不夺滚动。
- `src/ui/ai/aitoolresultview.{h,cpp}`（主所有者）：facts 构造器（实时 raw 与历史 envelope 同分派）；新增 `Operation` 分派（三轴胶囊+事实表+查询 ok 独立陈述+完整 ID 复制）；`buildDegraded`（transport incomplete/坏 JSON/标量原文，不引导重执行）；legacy 构造器委托投影保持兼容。
- `src/app/main.cpp`：`attachAiAssistant` 后一行 `installConversationPresentation`（+include）。
- `CMakeLists.txt`：仅 fragment manifest 一行。
- `DESIGN.md`：AI 结果卡片节追加「Agent 会话工作台（G5）」局部小节（沿用既有 token/原生控件）。
- `translations/paleo_{zh_CN,en}.ts`：仅新增 G5 文案条目（113 条；zh_CN identity 完成；en unfinished 骨架）——未用全量 lupdate（会吞 free-tr 译文并重排全库，违反共享翻译文件纪律）。

### 测试

| 测试 | 链接 | 覆盖 |
|---|---|---|
| `tests/tst_g5_conversation_projection.cpp` | paleo_workflow | W3 归一化 oracle（envelope/坏 JSON 非空成功/数组/operation 包裹/status unknown）、W2 关联（配对/绑定/旧记录/歧义/未应答）、UTF-8 边界 |
| `tests/tst_g5_operation_presentation.cpp` | paleo_workflow+paleo_services+paleo_store | W5/W6：真实 host/recovery 六路动作、cancel_after_commit 不抹已提交、游标翻页、端口读页 base64、resume 双击去重+computeCount 合同、capture-live race 拒绝、Read 收回拒绝、忙碌翻转 |
| `tests/tst_g5_conversation_workbench.cpp` | paleo_ui | W2 历史卡恢复（零模型请求+消息不重写）、归属（含旧记录）、W8 有界窗口+翻页、W7 草稿（拒绝保留/round-trip/接受后清理）+Enter/Shift+Enter、W1 上下文条、adapter 幂等/解绑 |
| `tests/tst_g5_assembly.cpp` | paleo_core（伞式） | W9 真实 AppContext+PaleoMainWindow+attachAiAssistant+install 全链、幂等重装、无工程上下文事实；PALEO_G5_SHOT_DIR 截图（浅/暗/窄） |
| `tests/tst_aitoolresultview.cpp`（扩展） | paleo_ui | envelope 分派、坏 JSON 降级非成功、operation 三轴/查询 ok 分离、committed+pending 组合、完整 ID 复制 |
| `tests/tst_aiassistdock.cpp`（回归） | paleo_ui | 既有 10 用例（键盘/卡行为兼容由其承载） |



## 三、验证与证据

### 本地验证命令与结果（2026-11-22，Linux；QGIS_PREFIX=/home/kevin/projects/paleo-deb-prefix-20261119/usr）

构建（共享互斥锁内、-j4）：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu -DQGIS_PREFIX=…` → EXIT=0。

测试（直接驱动 build/ 下二进制，env 同 build-env.local.sh——前缀 LD_LIBRARY_PATH 会与宿主 ctest 自身 expat 冲突，故按二进制直跑；ctest 项等价注册）：

| 测试 | 结果 |
|---|---|
| tst_g5_conversation_projection | 15/15 绿 |
| tst_g5_operation_presentation（真实 host/recovery/OperationLog 本地工程） | 13/13 绿 |
| tst_g5_conversation_workbench（FakeLlmServer loopback） | 9/9 绿 |
| tst_g5_assembly（真实 AppContext+PaleoMainWindow 装配；截图档 3/3） | 5/5 绿（1 skip=未设截图目录） |
| tst_aiassistdock（回归） | 11/11 绿（1 skip=既有按需截图） |
| tst_aitoolresultview（回归+G5 扩展） | 14/14 绿（1 skip=既有按需截图） |
| tst_aichatcontroller（回归） | 24/24 绿 |
| tst_chatresults（回归） | 16/16 绿 |
| tst_chatinvocationbinding（回归） | 8/8 绿 |
| tst_operation_control（回归） | 15/15 绿 |
| tst_aiwiring（回归，paleo_app 最小链接集） | 14/14 绿 |

源码闸门（tools/check_*.py + cmake -P，最终代码态全绿）：layering（--strict）、layering --qgs-audit（134=baseline，0 新增）、ui_size（545 文件 7 超限=baseline）、file_budget（51=baseline，0 新增/回升）、i18n、ui_tokens、ts_stale（0 missing）、ui_invariants、qgis_versions、docs_drift、cpp_mainline、check_layering --transitive（功能层 QtWidgets 黄牌 0）。

### 真实主窗 GUI 证据（W9.4）

`PALEO_G5_SHOT_DIR=docs/progress/g5-ai-shots QT_QPA_PLATFORM=offscreen ./build/tst_g5_assembly captureAssemblyScreenshots`：真实 PaleoMainWindow「AI 助手」页签走查——配置（127.0.0.1 loopback）→ 输入 → 发送（实时流式）→ 回答上屏 → 载入含 operation envelope 的历史会话 → 历史卡恢复（归属标注）→ 展开 → 点「操作详情」→ 详情面打开（事件/端口表就位；宿主未开工程，刷新被 adapter 按 document_unavailable 如实拒绝并显示原因）→ 截图 6 张：`docs/progress/g5-ai-shots/g5-assembly-{light,dark,narrow}{,-2x}.png`（1280×800 / 720×600；2× 档 2560×1600 / 1440×1200，QT_SCALE_FACTOR=2）。

### 过程中发现并修复的实现问题（均复现于上述测试）

1. **配置后按钮不翻可用**：dock 的 statusChanged 处理只更新文字不 refresh()——真实主窗里配置完成后发送/输入仍禁用（生产既有缺陷，走查复现），已修。
2. **接受判据误判**：sendRound 同步追加空 assistant 占位，使「最后一条消息是 user」判据失效；改为「数量增长 ∧ 最后一条 user 消息内容一致」。
3. **navigateHistory 方向反号**：更早/更新翻页方向与按钮语义相反（测试复现）。
4. **UTF-8 尾部扫描**：完整 4 字节序列在页尾被误留给下一页（单测复现），hold<3 → hold<=3。
5. **envelope 的 operation 事实**：顶层未透传时（chatToolResponse 形态在 data 根部）未被识别，operation 卡退化为 JSON 折叠（真实主窗走查复现）；顶层缺失时回读 data。
6. **m_cardsByOperation 悬垂**：卡 deleteLater 后映射仍持裸指针（自审发现），以 destroyed 信号清理。
7. **上下文条详情未随快照更新**：仅可见时更新文本，未展开时保持陈旧值（自审发现），改为始终更新。

### 未覆盖/限制（如实）

- event_history_expired 路径在 adapter 测试未注入（服务层由 tst_operation_control 覆盖；UI 呈现分支由 onOperationEvents 的 code 分支承担，未设专测）。
- Windows/macOS 平台行为未在本机验证（Linux 单机）；Windows 侧 QSettings/注册表隔离依赖既有 ctest 沙箱约定。
- 截图为 offscreen 平台渲染（同一 Qt 渲染栈）；未做人工目视走查记录。
- tst_g5_assembly 不打开真实工程文件（工程打开路径的 host 上下文变化由 tst_aiwiring 换工程回归承载）；走查中 adapter 对未开工程的拒绝行为本身是被验证的断言。
