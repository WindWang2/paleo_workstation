# Goal-Loop 方向 61：AI 工具调用闭环——tools[] 上送 + 领域工具执行回路

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

方向 51（PR #245）交付了 LLM 对话骨架，但工具面只到「描述+
分发占位」。TODOS.md:1-15 唯一新条目正是这项（P3，
from goal/ai-assist）：

1. **模型侧工具声明未上送**：`src/workflow/aichatcontroller.cpp`
   `requestMessages()`（定义于 `:135`）只拼 messages；`LlmClient::send` 签名只收
   messages（`src/ai/chat/llmclient.h:117`）——`AiToolSpec::
   toChatFunction()`（`domaintools.cpp:57` 已实现）从不进请求体
   `tools[]`，**模型根本不知道有工具可挑**。
2. **执行回路 NotImplemented**：三个工具（tile 分类/层位建议/
   测井相）在 `src/ai/chat/domaintools.cpp` 的 `dispatchAiTool`
  （定义于 `:182`；`:174-212` 区间含状态文案 helper）全部
   返回 `NotImplemented`（有 ORT 时）或 `Disabled`（无 ORT）；
   控制器只把 dispatch 结论转卡片文案
  （`aichatcontroller.cpp:32-44`）。
3. **执行面现成可复用**：`src/workflow/aiassistworkflow.cpp` 有
   完整编排——`setCatalog(DataCatalog*, projectDir)`（h:35）+
   `DerivedAssetRegistrar`，tile 分类执行面 `:228-231` 带 catalog
   登记与裁决语义；装配根开工程时刷新绑定
  （`src/app/appcontext.cpp:448-449`）。
4. **SSE 解析面已备**：`LlmStreamParser`（`llmclient.h:86`）按
   index 分段拼接 tool_calls、`takeCompleteCalls()`（`:90`）
   取出即移除——闭环只差「把 tool_calls 转发执行 + 结果回灌
   下轮请求」。
5. **上下文预算**：全量历史拼接无 token 截断（`requestMessages`
   全量，aichatcontroller.cpp:135-139），`maxTokens = 1024`
  （`llmclient.h:34`）——工具结果回灌后长会话更易撞顶。

- **协调面**：与方向 62（ai-ux）同触 aiassistdock/aichatcontroller
  ——**61 先行**（62 的会话管理在工具闭环后的消息形态上更稳）；
  若 62 已在飞按「谁先合谁为准，后者 rebase」。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\ai-toolloop -b goal/ai-toolloop-20261007 origin/master
cd .worktrees\ai-toolloop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。LLM 侧测试用假端点（先例：方向 51 的
socketpair/本地 HTTP 测试），不依赖真实远端。

## 目标形态（建议按序）

1. **tools[] 上送**：`LlmClient::send` 扩展可选 tools 参数；
   请求体带 `tools[]`（toChatFunction 数组）；`tool_choice`
   口径（auto）注释钉死。假端点断言请求体含 tools。
2. **tool_calls 执行回路**：`takeCompleteCalls()` 的调用转
   `AiAssistWorkflow` 现成执行面（tile 分类起步；层位建议/
   测井相同构接入）；执行异步（QPointer 守卫 + 世代号，先例：
   seismictaskservice 范式）。
3. **结果回灌**：工具结果按 OpenAI 协议 role=tool 消息 +
   tool_call_id 回灌下轮请求；对话 UI 呈现「工具执行中→结果
   卡片」两态；取消路径（在途工具执行的处理语义：等完成 vs
   作废——勘察后钉死并注释）。
4. **上下文预算**：历史窗口管理（近 N 轮 + system prompt 常驻；
   token 估算口径注释钉死——字符近似即可，标注非精确 tokenizer）；
   工具结果超长截断（截断标记如实显示）。
5. **安全红线沿用**：建议不自动落库（工具产物默认草稿态进
   catalog 的 DERIVED 草稿通道——与 aiassistworkflow 裁决语义
   一致）；计算注明方法假设。
6. **测试**：假端点全链路（tools 上送→tool_calls 回→执行→
   结果回灌→终答）；取消语义；超长截断；无 ORT 时工具诚实
   Disabled。

## 通用纪律（方向内全程有效）

- **分层**：协议扩展归 src/ai/chat（数据层），执行编排归
  workflow（复用 aiassistworkflow），UI 两态归 src/ui/ai；
  `check_layering.py --strict` 绿。
- **诚实面**：工具执行失败如实呈现（错误卡片不冒充成功）；
  Disabled/NotImplemented 状态语义保留；token 估算口径标注。
- **安全**：密钥只在钥匙串（先例）；工具结果不落盘除非显式
   裁决；无遥测。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：协议细节按 OpenAI 官方公开文档口径；取消语义
  自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-ai-toolloop.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/协议正确性/取消与守卫语义/安全红线/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. tools 上送：假端点捕获请求体含 `tools[]`（三工具 schema
   校验）；tool_choice 口径注释与实现一致。
2. 执行回路：假端点返回 tool_calls → 实际执行 tile 分类
  （夹具小模型）→ 结果 role=tool 回灌 → 终答生成——全链路
   测试绿。
3. 取消：在途工具执行时用户取消，无悬垂回调、无 UAF（守卫
   测试）；UI 状态如实。
4. 预算：超长历史截断生效（请求体长度断言）；system prompt
   常驻；截断标记可见。
5. 诚实面：工具执行失败→错误卡片；无 ORT→Disabled 状态卡；
   两者不冒充成功。
6. 回归：方向 51 既有测试（llmclient/chatsession/domaintools）
   全绿；全量 ctest 对照 R0 红集合 diff 为空。
