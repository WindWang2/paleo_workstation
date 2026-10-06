# 方向62 ledger — AI 助手 UX 补全（会话管理 / 图形化配置 / markdown 渲染）

- 分支：`goal/ai-ux-20261007`（worktree `.worktrees/ai-ux`，基线 origin/master `511c38e`）
- 协调：方向61（ai-toolloop）在飞未合——62 按约定独立推进；同触
  `aichatcontroller` / `aiassistdock` / `paleomainwindow_attach_shell`，
  合入时谁先合谁为准、后者 rebase。62 侧新增信号尽量少
  （控制器仅加 `sessionListChanged`），减少 61 的冲突面。

## 无人值守决策记录（宁小勿大）

1. **markdown 方言子集**（`src/ai/chat/markdown.h` 头注释即规范）：
   支持 ATX 标题（#~######）、围栏代码块（``` 忽略语言标注）、单层
   无序/有序列表、管道表格（首行表头 + `|---|` 分隔行 + 数据行）、
   行内 `**粗**/*斜*/`code``、空行分段段内换行保持。
   **不支持**（原样显示不猜）：嵌套列表、引用块、链接/图片、setext
   标题、删除线、任务列表、HTML 直插（安全红线）。
   只有表头没有分隔行的「表格」按普通段落原样显示。
2. **安全管线**：escape-all-first——整段输入先 `toHtmlEscaped` 再做标记
   替换；转换器只生成白名单标签。`<script>`/`<img onerror>` 字面量以
   `&lt;…&gt;` 文本呈现（tst_aimarkdown 有注入断言）。
3. **流式渲染形态**：流式期间 `insertText` 纯文本（不解释 HTML）；
   `streamingChanged(false)` 时把该块整体替换为 markdown 渲染
   （`m_assistantStart`/`m_assistantDraft` 记账）。取消/出错的部分
   回答同样走定型（语义：流结束 = 定型，无论怎么结束的）。
4. **DESIGN.md token 提案**：本方向**不新增 token**——会话行用
   QComboBox + QPushButton 原生控件（QGIS 原生优先），表单对话框用
   QFormLayout 标准形态 + spacingSm/spacingMd，对话气泡不引入
   （DESIGN.md 无此组件，transcript 平铺是方向51 既有形态）。
   状态胶囊/颜色全部沿用 PaleoTheme（check_ui_tokens/invariants 绿）。
5. **会话入口形态**：下拉而非侧栏——dock 在 bottomTabs 内空间受限，
   下拉是最小可达面；当前会话固定首位（未落盘也在），历史按
   updatedAt 倒序接后。
6. **切换未保存语义（钉死）**：`loadSession`/`newSession` 先
   `saveCurrentSession()` 再切换——取消产生的半轮回答不因切换丢失
   （tst_aichatcontroller::switchingSessionsFlushesUnsavedContentToDisk）。
7. **滚动位置**：dock 侧 `QHash<sessionId, scrollbar值>` 记忆，切换回来
   还原；首见会话落底。
8. **重发语义**：`retryLast()` 截掉最后一条用户消息之后的全部助手应答
   （含工具帧），不重复追加用户消息，按既有历史重新发起；历史截断经
   `sessionChanged` 整页重画。
9. **导出**：`ChatSession::toMarkdown()` 单一口径（`## 你/助手` 段落式，
   工具调用只报名字不导 argumentsJson——防实参泄漏）；导出前先落盘。
10. **密钥面**：对话框密钥字段 Password 回显、已存密钥只报「已设置/
    未设置」永不回显；无 QtKeychain（或 PALEO_LLM_NO_KEYCHAIN=1）密钥区
    整体禁用并指路环境变量（先例 llmkeystore 降级口径）。结构校验失败
    阻断保存；密钥缺失允许保存（助手如实保持禁用态）。

## 静态门禁（改动树上实测）

- `check_layering.py --strict`：通过（2026-10-07，0 违规）
- `check_i18n.py`：通过（0 裸字面量；新文案全部 tr()）
- `check_ui_tokens.py` / `check_ui_invariants.py`：通过（0 违规）
- 翻译 .ts：仓库当前 `translations/paleo_zh_CN.ts` 无 AiAssistDock
  context（方向51 串亦未入册），方向62 新串沿用同口径不单方入册，
  归 l10n 批量工具收割（update_translations.sh）。

## 轮次记录

| 轮 | 改动 | 验证 | 结论 |
|----|------|------|------|
| 1 | worktree/配置/基线构建 + 勘察 | build exit 0（1755/1755） | ✅ |
| 2 | 批1-4 代码全量 + 3 测试文件 | 静态门禁（layering strict/i18n/ui_tokens/ui_invariants）4 项绿 | ✅ |
| 3 | R0 基线（先 stash 还原 master → 重建 → 串行 ctest） | 首采 PATH 顺序错误（229 红=0xc0000139 全域）；修正顺序（conda deps 前置）后 paleo.exe 起活 | ⚠️→✅ |
| 4 | build-ux 独立编译验证（避开 R0 采集中的 build/） | tst_aimarkdown 12/12、tst_aichatcontroller 12/12、tst_aiassistdock 11/11+1 设计跳过、tst_llmconfigdialog 8/8 | ✅ |
| 5 | 修复：markdown 标题级别（`<h#>` bug，captured(1) 是井号串非数字）；dock 测试会话清理/假服务器答案口径；对话框测试「已设置」断言与无钥匙串分支矛盾 | 四套全绿（证据见上） | ✅ |
| 6 | ledger 截图采集（PALEO_AI_SHOT_DIR 门控） | ai-ux-dock-markdown.png（520x420，584 色，token 色在）；ai-ux-llmconfig.png（补 pinRenderEnvironment 后 35KB 主题化） | ✅ |
| 7 | R0c 正确 PATH 基线采集（build/ = 纯 master，串行 325 项） | 41% 过/192 红（多为宿主 temp/夹具/DLL 类；AI 家族绿：aichatcontroller/aiassistdock/aiwiring Passed，tst_aichat 红属 QTemporaryDir 宿主问题） | ✅ |
| 8 | 独立子代理复审（五维） | 0 High / 3 Medium / 多 Low（详见下） | ✅ |
| 9 | 修 3 Medium + 低成本 Low | 三套测试复跑全绿（controller 14/14、dialog 7/7+skip、dock 11/11+skip） | ✅ |
| 10 | 全量 ctest（build-ux = 方向62 全量，327 项，串行）对照 R0c diff | **diff 为空**：R0 红 192 = final 红 192，无新增红、无愈合；AI 家族全绿（aichatcontroller/aiassistdock/aiwiring/aimarkdown/llmconfigdialog Passed；tst_aichat 红为宿主稳定红，基线同红） | ✅ |
| 11 | 关键验证第二遍（ctest -R chat/wiring/markdown/llmconfig） | 同第一遍：除 tst_aichat（宿主稳定红）外全过 | ✅ |

## Oracle 验收证据（逐条）

1. **会话管理** ✅ — tst_aiassistdock::sessionSwitchRoundTripsContentAndScroll：
   多会话创建→切换→内容回放一致（含滚动位置还原）；
   openSessionRequested 真实 emit（QSignalSpy 断言）+ 测试内与生产同构的
   loadSession 接线（生产接线 paleomainwindow_attach_shell）；删除后列表同步
   （renameAndDeleteKeepComboInSync）；切换未保存语义钉死并有
   tst_aichatcontroller::switchingSessionsFlushesUnsavedContentToDisk。
2. **配置对话框** ✅ — tst_aichatcontroller::applyConfig*（表单→LlmConfig
   文件读 round-trip；结构非法不落盘）；密钥永不回显（tst_llmconfigdialog::
   keyNeverEchoedInPlaintext——全 label/输入框不含明文断言）；无 keychain
   密钥区禁用 + 指路环境变量（keyAreaDisabledWithoutKeychain）。
3. **markdown** ✅ — tst_aimarkdown 12 用例覆盖标题/列表/表格/代码块/
   粗斜体 + `<script>`/`<img onerror>`/表格与代码内注入全部转义断言；
   流式末轮定型（tst_aiassistdock::markdownAnswerIsFormattedAfterStreamEnds
   ——QTextTable 帧结构证据 + docs/progress/ai-ux-shots/ 截图）。
4. **消息操作** ✅ — 复制（剪贴板断言：选区/整段两路）；重发（假端点
   requestCount 1→2、历史无重复、旧回答被截）；导出 .md 内容 ==
   ChatSession::toMarkdown（单一口径断言）。
5. **回归** ✅ — 方向 51 既有测试全绿（tst_aiassistdock 旧 4 用例 +
   tst_aichatcontroller 旧 8 用例 + tst_aiwiring 6/6）；check_i18n 绿
   （新文案 tr() 全覆盖）；layering --strict / ui_tokens / ui_invariants 绿。
6. **全量 ctest 对照** ✅ — R0（纯 master，同 env 串行 325 项）红集合 192
   vs 方向62 全量（327 项）红集合 192：diff 为空（无新增红；愈合亦无）。

## 复盘：R0 基线的坑

R0b（首采）PATH 顺序错误（C:/deps/Qt 在前）→ qgis 链二进制全域
0xc0000139，229 红无效作废。R0c 修正顺序（conda deps 前置）采集于
build/——该树经多轮 stash/pop 后存在陈旧对象，红集合（192）含少量
污染性假红（如 tst_aichat 在 build-ux 干净树上转绿过一轮）；最终对照
两边同为 192 且逐名一致，方向62 改动未引入任何新红（含 AI 家族全绿）
这一结论由「final-only 为空」直接保证，不受基线纯度影响。

## 交付物

- 提交：54f5644（markdown 转换器）→ 4591570（会话管理与消息操作）→
  57f2734（图形化配置对话框 + 壳接线 + TODOS）→ docs 提交（本 ledger
  与截图）。
- 分支：goal/ai-ux-20261007（worktree .worktrees/ai-ux）。
- 与方向61（ai-toolloop，在飞）同触 aichatcontroller/aiassistdock/
  attach_shell——合入按「谁先合谁为准，后者 rebase」。

## 独立复审（轮8）与修复（轮9）

复审结论：0 High、3 Medium；Medium 全修：

1. **配置持久化在视图层**（llmconfigdialog 直接 save/keystore）→ 重构：
   对话框只发 `applyRequested(表单值, 新密钥, 清除意图)`；新增
   `AiChatController::applyConfig(...)`（功能层编排：结构校验 → JSON 落盘
   → 钥匙串写/清（异步，QPointer 双守卫）→ setConfig）；结果回灌
   `dialog.onApplyResult`。测试契约同步：落盘 round-trip 移到
   tst_aichatcontroller（applyConfig 两用例）。
2. **loadSession 失败后下拉脱同步** → 失败路径也发 `sessionListChanged`
   （刷新回真实当前会话，显示态不许与事实脱钩）。
3. **对话框异步窗口期无防护** → busy 态（OK/Cancel 禁用 + 「正在应用…」，
   防重复提交；失败停留可再试，成功才关窗）——测试断言二次点击不再发意图、
   失败回灌后可重交。

顺手修的 Low：clear+新密钥同时给出时新密钥优先（清除视为冗余）；
renameSession 空会话不落盘（守「空会话不出文件」纪律）；exportCurrentSession
落盘失败如实中断；appendError 防御性 finalize（防未来错误流写入被整段替换
吃掉）；attachController 滚动记账真复位；工具卡 QLabel setTextFormat
(PlainText)（模型可控文本不做富文本探测——方向51 顺手加固）。

复审确认无问题的重点项：retryLast 截断时序、finalize 记账在 clear 后失效
处理、「先取消在飞再落盘」次序、refreshSessions 的 QSignalBlocker 覆盖面、
attachController 幂等与宿主侧 dock->controller() 取新。

## 关键修复记录（轮5 细节）

- `markdown.cpp headingHtml`：`captured(1)` 是井号串（"###"），直接 arg 进
  `<h%1>` 产出 `<h#>`/`<h###>`——改为 `QString::number(length())`。
  tst_aimarkdown::headings 断言 `<h1>`/`<h3>` 抓获。
- tst_aiassistdock：①sessionSwitch 末尾漏删会话 A → 污染 renameAndDelete
  的 combo 计数断言（补 `ChatSessionStore::remove(idA)`）；②本文件假服务器
  Stream 模式答案 = kChunkA×2（「砂岩砂岩」），重发断言改为「与首轮回答
  相同」（同源同答），不硬编码「砂岩为主」。
- tst_llmconfigdialog：`已设置` 状态行断言与 PALEO_LLM_NO_KEYCHAIN=1 的
  降级文案互斥——改为「两分支都不出现密钥明文」的正交断言。

## 环境口径（本机 Windows）

- cmake：`C:/Qt/Tools/CMake_64/bin/cmake.exe`（Git Bash PATH 没有），
  经 `.zwork/msvc-cmake.bat`（vcvars64 → cmake）调用；CMAKE_PREFIX_PATH
  不能带内引号（会渗进 cache 值）。
- **运行时 PATH 顺序（关键）**：`paleo-qgis-deps/Library/bin`（conda，含
  qgis 所链的 Qt6/gdal/geos/qt6keychain）必须排在 `C:/deps/Qt/6.8.0/bin`
  **之前**——反了则一切 qgis 链接二进制 0xc0000139（入口点缺失：先加载
  6.8 的 Qt6Core，qgis 要 conda Qt 的导出）。方向51 ledger 的「宿主红项」
  即此因；`paleo-dev.ps1` 的 OSGeo4W 路径同理（其 bin 天然在 Qt 前面）。
- Windows 侧 ctest 串行（AGENTS 纪律：QSettings NativeFormat 注册表
  不受沙箱 env 控制）。
- 本机 QTemporaryDir 有宿主级问题（TEMP 被 Git Bash 置成 POSIX `/tmp`，
  tst_las 等 initTestCase 因此红）——本方向测试不依赖 QTemporaryDir，
  导出落 ChatSessionStore 沙箱目录。
- 截图：`PALEO_AI_SHOT_DIR=<dir>` 时 tst_aiassistdock /
  tst_llmconfigdialog 的 capture 用例落 PNG（默认 QSKIP），
  产物提交 `docs/progress/ai-ux-shots/`。

## 遗留（Low，记 PR body）

- 会话列表无分页（loadRecent 上限 20，写死在 refreshSessions）；每轮
  结束的 sessionListChanged 会同步重读 ≤20 个小 JSON（UI 线程盘 I/O，
  量级可忽略，未做节流）。
- 导出文件名非法字符过滤只清 Windows 保留集。
- 对话框 pending 应用期间用户 Cancel：JSON 已落盘而编排器内存态未更新
  （与「下次启动读盘」语义一致，不追补）。
- 首见会话「落到底」在 QTextEdit 布局极端滞后时可能停在顶部（offscreen
  实测未见；如遇可改 singleShot(0)）。
- 方向51 递延复审项（非本方向引入）：tst_aichat 的
  sessionRoundTripAndStoreLivesOutsideProject 在本机红——宿主 QTemporaryDir
  问题（Git Bash 把 TEMP 置成 POSIX /tmp，QTemporaryDir path() 退化为
  tempPath 前缀使 startsWith 误判），R0 基线同红，非回归。

## PR body 素材（待全量 diff 验证后定稿）

方向62：AI 助手 UX 补全——会话管理 / 图形化配置 / markdown 渲染 /
消息操作。四提交：markdown 转换器（escape-all-first 安全管线）→
会话管理与消息操作（切换落盘语义钉死/重发/导出）→ 图形化配置对话框
（意图式分层，密钥不回显）→ ledger 与离屏截图。
证据：四套 AI 测试全绿（12/14/11/7+设计跳过）；静态门禁四项绿；
全量 ctest 对 R0 红集合 diff 为空（见 ledger 轮10）。
