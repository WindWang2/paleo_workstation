# Goal-Loop 方向 62：AI 助手 UX 补全——会话管理 / 图形化配置 / markdown 渲染

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

方向 51 交付的助手 dock（`src/ui/ai/aiassistdock.cpp:37-108`：
状态行/新会话/配置按钮/工具卡片区/输入框）缺三块用户可达面：

1. **会话列表 UI 整体缺失**：`openSessionRequested` 信号只有
   声明（`src/ui/ai/aiassistdock.h:33`），全仓无 emit 无连接；
   `ChatSessionStore` 的 loadRecent/removeSession（`src/ai/chat/
   chatsession.h:39-43`）编排能力已备但用户够不着——只能
   「新会话」，历史会话无法回到。
2. **图形化配置对话框未做**：「配置…」按钮只弹 message log
   告诉用户配置文件路径（`src/ui/paleomainwindow_attach_shell.cpp:574-585`，
   注释明说「图形化配置对话框递延（TODOS.md）」）。当前配置
   只能手改文件 + 环境变量（端点/模型/开关经 `LlmConfig::path()`，
   密钥经钥匙串或 PALEO_LLM_API_KEY）。
3. **回复无 markdown 渲染**：assistant 增量走 insertText 纯
   文本 + toHtmlEscaped（`aiassistdock.cpp:182-188`）——模型
   输出表格/代码块/列表无排版；地质解释的对比表格是常见输出
   形态。
4. 消息无复制/重发；会话无导出口（chatsession 只进用户目录，
   by design 不进工程——导出为可选轻项）。

DESIGN.md 纪律：视觉决策先读 DESIGN.md；助手 dock 已用
PaleoTheme token（先例 aiassistdock.cpp），新面沿用。

**协调面**：与方向 61（ai-toolloop）同触 aiassistdock/
aichatcontroller——**61 先行**（工具闭环后的消息形态更稳）；
若 61 已在飞按「谁先合谁为准，后者 rebase」。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\ai-ux -b goal/ai-ux-20261007 origin/master
cd .worktrees\ai-ux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。UI 测试 offscreen；markdown 渲染用 QTextDocument
原生（Qt 自带 HTML 子集，不引第三方）。

## 目标形态（建议按序）

1. **会话管理**：dock 加会话侧栏/下拉（loadRecent 列表 + 打开
   + 删除 + 重命名）；`openSessionRequested` 信号接通（emit +
   控制器 load）；切换未保存内容的处理语义钉死（当前会话自动
   落盘先例：aichatcontroller.cpp:49-53 finished 即存）。
2. **配置对话框**：`src/ui/ai/llmconfigdialog.*`——端点/模型/
   maxTokens/超时/开关表单；密钥字段走钥匙串写入（显示掩码，
   不回显明文）；「打开配置文件所在目录」按钮；无 QtKeychain
   构建时密钥区如实禁用（先例：llmkeystore 降级口径）。
3. **markdown 渲染**：流式增量用 QTextDocument 追加 + 末轮
   markdown→HTML 转换（轻量转换器：标题/列表/表格/代码块/
   粗斜体——Qt QTextDocument setHtml 原生支持；手写 200 行级
   转换器，注释钉死支持的方言子集）；转义纪律不放松（用户
   输入与模型输出均按不可信文本转义后再进 HTML）。
4. **消息操作**：复制（单条 + 整会话）；重发最后一条用户
   消息（复用 send 路径）；导出会话为 .md 文件（QStandardPaths
   或用户选路径）。
5. **测试**：会话切换 round-trip（内容/滚动位置）；配置写→
   读 round-trip；markdown 转换器单元测试（表格/代码块/转义
   注入——`<script>` 字面量必须被转义）；offscreen 截图入
   ledger。

## 通用纪律（方向内全程有效）

- **分层**：会话/配置编排归 workflow，UI 归 src/ui/ai，存储
  归 ai/chat（已有）；`check_layering.py --strict` 绿。
- **DESIGN.md**：新面板先提案 token（对话气泡/侧栏/表单）
  再实现；截图前后对照入 ledger。
- **安全**：模型输出按不可信文本处理（转义后再进 HTML，
  markdown 转换器禁裸插 HTML）；密钥不回显不落明文。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：markdown 方言子集自行定案记 ledger（宁小勿大，
  不支持的语法原样显示）。
- **ledger**：`.goal-loop-ledger-ai-ux.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/转义安全/DESIGN 一致/信号接通正确性/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 会话管理：多会话创建→切换→内容一致（round-trip 测试）；
   删除后列表同步；openSessionRequested 有真实 emit/连接
  （rg + 测试）。
2. 配置对话框：表单写→LlmConfig 文件读 round-trip；密钥进
   钥匙串且界面不回显明文（测试断言 label 掩码）；无 keychain
   构建密钥区禁用态。
3. markdown：转换器单测覆盖标题/列表/表格/代码块/粗斜体；
   `<script>`/`<img onerror>` 注入字面量被转义（安全断言）；
   流式渲染末轮转 HTML 后格式正确（offscreen 截图）。
4. 消息操作：复制到剪贴板（QClipboard 测试）；重发路径产生
   新请求（假端点断言）；导出 .md 文件内容与会话一致。
5. 回归：方向 51 既有测试全绿；check_i18n 绿（新文案 tr()
   全覆盖）。
6. 全量 ctest 对照 R0 红集合 diff 为空。
