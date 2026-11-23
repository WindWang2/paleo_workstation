# H2｜私有会话档案的有界读写与轻量最近列表（2026-11-22）

- 分支：`codex/h2-agent-session-archive`（base = `origin/master` `8eca4644`）
- Worktree：`../paleo-h2-agent-session-archive`（独立 build）
- 范围：W1–W8（档案合同/有界读取/容量准入/索引/查询/一致性/Combo/验收）；
  不动 G2 checkpoint/欠账状态机、G1 传输、H1 sendRound 投影、H5 计划面。

## Scope ledger（开工核对，8eca4644 源码实证）

| 工作包 | 开工时生产入口现状（源码观察） | 本任务增量 |
|---|---|---|
| W1 身份/结构合同 | `ChatSession::fromJson` 无 schema/形状/身份检查，`toObject/toArray` 默认值把坏结构映射成空历史 | `chatsessionarchive.*`：`validateChatArchiveObject`（schema=1、id==文件名、字段类型/长度、messages/tool_calls/bindings 形状、消息数护栏）；错误分类 `ChatArchiveReject`；load 失败不部分覆盖 out |
| W2 有界读取 | `load` 直接 `readAll()`，无文件类型/大小/累计上限检查；无符号链接边界 | 常规文件 + 叶子 `isSymLink()` 拒绝 + 父目录 canonical == 会话根（目录伪装/叶子链接别名/junction 同拒）；打开前 size 与读取中累计双上限（`readChatArchiveBytes`）；超限旧档案保留字节、只给元信息状态 |
| W3 容量准入 | 无任何发送/重试/改名前置容量概念；`save` 无上限 | `ChatArchiveBudget`（生产上限由 `turnReserve()+titleReserve()+32MiB` 结构式推导 = 338,203,072 字节）；`sendUserText` append 前、`retryLast` resize 前、`renameSession` 赋值前三道纯门；`save` 写侧同尺（字节 + 消息数 + 头字段长度，对称）；G2 欠账语义原样 |
| W4 可重建索引 | 无 | `chatsessionindex.*`：per-session sidecar（`<root>/index/<id>.meta.json`，≤2 KiB，schema=1，id 绑文件名）；只存列表元信息 + size/mtime 弱指纹；缺失/损坏/过期丢弃重建，永不反向改正文 |
| W5 有界最近列表 | `loadRecent` 全量解析所有正文再排序裁剪；每次 checkpoint 的 `sessionListChanged` → `recentSessions(20)` → 全库正文载入 | `aichatrecentsummaries.*` 协调器：热路径 stat + sidecar/内存缓存（正文解析计数零增长）；超限档案按当前预算只看文件大小；冷修复经既有 PaleoTaskService 的 quiet 低优先应用任务（分文件协作取消、单飞、#159 连接次序、用户取消不自动重启、写失败按候选指纹抑制重试）；无服务 = 只发布可信缓存的部分结果，不退回 GUI 线程全库解析；partial/complete 如实发布 |
| W6 一致性 | save/remove 与列表无缓存协同 | 权威 save 成功才写 sidecar（sidecar 失败不影响 save=true、不产生假欠账）；remove 同步清缓存；外部删除/替换/坏 sidecar 经弱指纹失配定向修复；孤儿 sidecar 在未截断的扫描后清理 |
| W7 Combo 接入 | `refreshSessions` 同步 `recentSessions(20)`（全量正文） | `refreshSessions` 只发查询；`rebuildSessionCombo`（当前会话恒首位、QSignalBlocker、超限/异常行诊断文案、`complete=false` 时末尾一条禁用的「列表可能不完整」说明行）；草稿/工具卡/选择不受刷新影响 |
| W8 验收 | — | `tst_h2_sessionarchive`（W1/W2/纯准入数学）、`tst_h2_capacitygates`（W3 控制器闭环）、`tst_h2_recentarchives`（W4/W5/W6，含 1000 小档案）、`tst_h2_sessioncombo`（W7 真实信号链） |

既有能力复用（不重做）：G2 同步 checkpoint/欠账/删除保护/重试授权
（`settleSaveArrears`、`validateInvocationRetry` 原序保留）；G3 工具表；G4 隔离；
`QSaveFile` 原子写；`displayTitle` 语义；`pathFor` 单段安全名；`loadRecent`/`listIds`
契约保留（dock 不再调用 `loadRecent`）。轮次形状常量
（kMaxToolRounds/kMaxCallsPerTurn/kMaxToolArgumentBytes/8 MiB 响应上限）单点
登记到 `chatsessionarchive.h`，controller/llmclient 改为引用（值不变）。

## 容量推导（W3，工程容量选择）

每项都是「已准入回合内可能落盘的最大增量」，全部取自既有业务上限，不提高
任何 LLM/工具预算：

```
escapeWorstFactor = 6                 // 每码元最坏 \uXXXX；UTF-8 网络字节 ≥ UTF-16 码元
assistant   = 6 × (6×8 MiB + 1 KiB)                 // 开场 + 5 次工具续轮；响应 ≤ 8 MiB
toolResults = 16 × (6×(8 KiB + 1 KiB) + 2400)      // 每条已准入调用恰一条 role=tool
toolFrames  = 16 × (6×16 KiB + 2560)                // arguments ≤ 16 KiB；id ≤256、name ≤128
bindings    = 16 × 3072                             // documentId ≤256、toolName ≤128（含最坏转义）
messageMeta = 26 × 2400                             // timestamp + tool_call_id + host_turn_id
turnReserve = 304,644,544（290.53 MiB）
titleReserve= 6 × (256 + 256) + 1 KiB = 4,096       // 轮中 rename 的标题 + 换模型后的 model 名
maxArchiveBytes = turnReserve + titleReserve + 32 MiB = 338,203,072（322.54 MiB）
```

- 空会话一整轮最大合法工作恒可容纳；已载入历史 + 用户消息 ≤ 32 MiB 的会话仍可
  开新的一轮；超过后被拒并建议新建会话（原 newSession / 导出 / G2 欠账出口不受
  影响）。
- 消息数护栏按「现有 + 一轮可能追加的最大条数（25）」评估，轮内 append 不再过门，
  门放行的轮次结束后仍在结构上限（65536）内。
- Pi 引擎每轮文本上限为 16×4096 码元（`PiChatBridge::modelDelta`），小于原生
  8 MiB 响应上限，同一预留对两种引擎都成立。
- 读写对称：save 先序列化再开文件，用同一上限；成功写出的档案自身 load 恒可接受。

## 旧超限档案边界

- > 上限的旧档案：load 明确 `SizeExceeded` 拒绝完整载入（字节保留），最近列表给
  `OverLimit` 元信息状态（只用扫描已 stat 的文件大小对照当前预算，不读正文，
  不信缓存里的旧结论）；仍可经 UI 删除。
- 消息数 > 65536：`MessageBudget` 拒绝。model > 256 / 标题 > 4096 码元：结构非法
  拒绝；save 侧同尺拒绝写出（否则会写出自身永远载不回的档案）。
- 无 bindings / 无标题 / 对象形态 arguments / 无时间戳的 schema=1 档案照常可读；
  长标题（≤4096 码元）容忍，新写入标题上限 256。`schema` 自 writer 首个提交
  （b99e8d5f）起恒写，缺 `schema` 的文件不是 Paleo 写出的档案，按 `SchemaMissing`
  拒绝；未来版本号按 `SchemaUnsupported` 拒绝。

## 环境

- CachyOS Linux x86_64，Qt 6.10.2 / QGIS 4.2.3（deb 闭包前缀），RelWithDebInfo
  **`-O2 -g1 -DNDEBUG`**（同批「构建磁盘占用公告」与 CI Linux 必过车道口径；完整 DWARF
  下每棵全量构建树约 270 GB，本构建树 5.6 GB），`-j4`（主机级构建互斥，同批多 Agent 同一
  时刻只有一个重型构建）。
- **依赖前缀**：`vendor/prefix` 是一份带闭包收据的 deb 闭包的**私有只读副本**
  （`cp -a`，96,979 项 / 3.47 GB；收据 `90a7bf14…fb02` == 本分支
  `vendor/deb-closure.lock` 的 sha256；configure 期 `paleo-deb-prefix.cmake` 复核
  收据与关键资源）。缘由：最初只读引用的兄弟 worktree 前缀在该 worktree 被删除后
  悬空；本机无 `dpkg-deb`，仓库的离线解包入口（`vendor/fetch-deps.sh`）在本机
  不可用。`CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`；
  `vendor/onnxruntime`、`vendor/ranuts-document` 链接主仓；`office-editor` 就绪
  （configure 无缺失警告）；`vendor/Data_service` 按本分支 gitlink `905b5c1b`。
- **测试执行方式**：deb 前缀进入 `LD_LIBRARY_PATH` 后系统 `ctest` 无法加载
  （`XML_GetCurrentColumnNumber64` 未定义，同 G2 批次记录）。测试二进制按
  `CTestTestfile.cmake` 生成的原始参数、每测试沙箱 `ENVIRONMENT`、
  `WORKING_DIRECTORY` 与 `TIMEOUT`（300 s）直接运行，串行（相当于 ctest -j1）。

## 本地验证

最终 head `552d1191`（base = `origin/master` `8eca4644`，主干在开发期间未移动，无需
rebase）。15:29 构建之后的提交只改测试文件与本文档；产品代码与产品二进制自那次构建
后未变（因此 27 个回归测试的二进制仍是跑过的那批）。

### 构建

`./paleo-dev build -j4 --target paleo <31 个受影响测试>`：1371 步，**exit 0**，1348 s，
0 error；H2 改动的 TU 无新增警告。构建树 5.6 GB。

### 专项新增（4 个测试，44 例全绿，0 skipped）

| 测试 | 结果 | 覆盖 |
|---|---|---|
| `tst_h2_sessionarchive` | 15 passed | schema/身份/结构拒绝、目录伪装与符号链接别名（叶子链接在本机**真实创建并拒绝**，非 SKIP）、超限档案保留字节、读取中增长（无限设备）、恰好到限/差一字节、消息数上限、纯准入数学边界、save 对称拒绝、sidecar 生命周期 |
| `tst_h2_capacitygates` | 8 passed | 发送拒绝零 append/零 HTTP/磁盘不变 + `newSession` 逃生、近上限旧档案 load→retry 在 resize 前拒绝→原回答/bindings 不变、重试余量充足时照常、rename 三类拒绝（标题字段/候选档案成本/无法载入）与合法改名、空会话不落盘、真实重转义一轮的落盘字节落在预留内 |
| `tst_h2_recentarchives` | 13 passed | 1000 小档案冷修复恰 N 次正文解析、热刷新 5 次零解析、定向修复恰 1 次、排序/同秒/无时间、外部改写与坏缓存重建、超限/损坏摘要、外部删除清孤儿、无服务 partial、修复原语与任务页取消、控制器销毁安全、索引目录只读时保存事实独立 |
| `tst_h2_sessioncombo` | 8 passed | 下拉由元信息构建、超限选择经权威拒绝、**真实 checkpoint 信号链后零旧正文解析**、草稿与选择不受刷新影响、删除不复活、未校验完全部候选时的禁用说明行 |

### 回归（既有受影响入口，27 个测试）

388 passed / 0 failed / 4 skipped（此前误记为 402，独立复审 R6 逐文件核对后更正）：

| 测试 | 结果 |
|---|---|
| tst_aichat / aichatcontroller / aichatprojectquery / aichatcapability | 17、24、16、9 全绿 |
| tst_aiassistdock / aitoolresultview | 11、14 通过；各 1 skipped（`captureLedgerScreenshots`，需显式截图环境，既有环境门） |
| tst_chatinvocationbinding / chatresults | 8、16 全绿 |
| tst_g1_configcommit / g1_credentialboundary | 19、11 全绿 |
| tst_g2_turncheckpoint / g2_piterminal | 12、11 全绿 |
| tst_g5_assembly / g5_conversation_workbench / g5_conversation_projection / g5_operation_presentation | 5、10、17、14 通过；g5_assembly 1 skipped（`captureAssemblyScreenshots`，既有环境门） |
| tst_pichatbridge / piconversation / piprotocol / pigeneratedoperations | 29、15、21、6 全绿 |
| tst_shell_projectlifecycle | 19 通过；1 skipped（`realProjectBackgroundResponsiveness`，需真实工程夹具，既有环境门） |
| tst_wellfaciesagent / tile_agent / agent_readiness / aichattoolloop / capability_host / aiwiring | 20、14、10、14、12、14 全绿 |

4 个 skipped 全部是既有的环境门（截图采集 / 真实工程夹具），与 H2 无关，**如实记为
未执行，不计入通过**。

### Source gates（最终 head，脚本直跑，全部 exit 0）

layering `--strict` / `--qgs-audit --strict`（134 = 基线 134，新增 0）/ `--transitive`
（`QGIS_PREFIX` = 本 worktree 的受控前缀）、qgis_spread（169 = 基线 169）、
file_budget `--strict`（51 = 基线 51，新增 0）、ui_size、ui_invariants、i18n、ui_tokens、
ts_stale（0）、l10n、qgis_versions、docs_drift、cpp_mainline。**未抬任何基线、未改
任何检查器。**

### 过程如实记录

- 上述构建/测试是本任务**第一次真实运行**（此前会话两次中断 + 等待主机构建互斥锁
  约 7 小时）。在此之前只做过：每个改动 TU 的 `-fsyntax-only`、静态闸门脚本、测试
  用例逐条推演。
- 首跑结果 1 绿 3 红，**产品代码未暴露缺陷**，全部是测试自身问题，修复于 89b57cff：
  (a) `QFile(path).readAll()` 用未打开的临时对象恒返回空，使「原文件字节保留」类断言
  此前是空对空的恒真比较——改为显式打开的 `readFileBytes()` 后这些断言才真正生效；
  (b) 有界列表期望值写错：`requestRecentSessionSummaries(20)` 在回合后是「当前会话
  自身进入最近 20 条之首 → 被 dock 过滤 → 当前 + 19 个存量 = 20 项」，与改造前
  `recentSessions(20)` + 过滤当前会话的条数语义一致。
- 独立复审（R1–R6）在**构建前后**靠静态推演 + 实跑证据发现并修掉了 4×P0 / 2×P1 等问题；
  构建前的逐条推演与自审又消掉了 9 处测试缺陷与 3 处产品侧问题（预算旋钮默认值为 0、
  超限档案沿用缓存旧结论、修复原语的指纹竞窗）。
- **等待约 7 小时的真实原因**（不是"机器慢"）：同批的共享构建锁脚本
  `heavy-build.sh` 用 `git -C <主仓> rev-parse --git-common-dir` 取锁目录，该命令从主
  checkout 调用时返回**相对路径 `.git`**，被解析成**调用者 cwd** 下的 `.git`——从 worktree
  调用时 worktree 的 `.git` 是文件（gitlink），`mkdir` 恒失败，`wait` 轮询永远在 sleep。
  H2 与 h4 的排队进程都因此空转（H2 错过 h4 在 14:24 主动让出的窗口就是这个原因）。
  已把脚本改为 `--path-format=absolute`（备份原文件）并从 worktree cwd 验证。
- R6 指出回归合计误写为 402；逐文件重算确认实际为 **388 passed / 0 failed / 4 skipped**
  （两种独立数法一致），文档与 PR 描述均已更正并保留更正说明。

## 未覆盖/限制

- Windows 特有名（保留名/junction/大小写不敏感归一）未实测；叶子链接别名用例在
  无法创建符号链接的平台显式 SKIP（如实记为跳过，不算通过）。
- 同毫秒同大小替换的弱指纹盲区：size+mtime 一致时缓存判新鲜（打开仍走权威校验）。
- **准入序列化成本**（复审 R1 提示）：sendUserText/retryLast 的容量门在 UI 线程
  全量序列化当前会话——近上限（数百 MB）档案的拒绝判断本身可感知卡顿；正常
  会话（MB 级）无感。未做序列化字节数缓存。
- **load 的 TOCTOU 窗口**：类型/链接检查与 open 之间存在竞窗（本地私有用户数据
  目录的威胁模型下可接受；读取中累计上限兜住打开后增长）。
- 无任务服务时未建索引的会话不会出现在列表里（列表标注「可能不完整」）；生产
  装配已接任务服务（appcontext.cpp）。
- 真实 Pi 进程链 / 受控 Python 发布链 fixture 本机未提供（同 G2 批次口径）。

## 独立复审记录

- R1（独立 reviewer，对 990c0282 全量 diff + 调用链，静态推演）：REQUEST CHANGES
  ——4×P0（修复协作取消极性反转、超限用例时序、retry 用例 lastUser 指向、
  `pruneOrphans` 零调用）、2×P1（预留常量低于形式最坏上界、save 只查字节不查
  结构）、若干 P2。修复提交 bdbba6a5 / fd47f3f3。
- R2（对 990c0282..fd47f3f3）：REQUEST CHANGES——残留 1×P0（retry 边界夹具把大
  内容放在会被 resize 丢弃的一侧）+ 3×P2（截断时 prune 误删、未用变量、文档）。
  修复提交 e4f064f1 / dfff0406。
- R3（对 e4f064f1）：APPROVE（代码面）。
- R4（另一位独立 reviewer，对 e4f064f1..871d73fa 增量）：REQUEST CHANGES——产品
  代码无阻塞；1×P1 测试缺陷（`loadSession` 载入失败只发 `sessionListChanged`、原因
  走出参，用例却在等 `errorOccurred`）、2×P2（DESIGN.md 禁用控件 reason tooltip；
  空 id 防御——后者经 reviewer 复核撤回）。修复提交 08b289d6。
- R5（同一 reviewer，对 871d73fa..39cb98a1）：APPROVE——rename 拒绝按原因分报、
  修复原语读前取指纹读后复核均正确；确认后者无环/无饥饿，未配专门测试可接受
  （竞窗无确定性注入缝，失败模式保守）。
- R6（另一位独立 reviewer，对 d771d5a1 及其实跑证据）：**APPROVE（绑定 d771d5a1）**，
  无 P0/P1；2×P2——(a) 测试的文件读取辅助函数仍有恒真缺口（一处 `!=` 断言被空过、
  两侧都读不到时哨兵对哨兵仍相等），(b) 回归合计 402 应为 388。
- R6 修复：d771d5a1（哨兵 + 7 处基线非空）、26e1e75b（requireFileBytes）、
  eaf48afc（REQUIRE_BYTES 宏把断言放回测试槽）——三轮都在收紧同一件事：让「磁盘不变」
  类断言真正可失败。
- R7（对 26e1e75b 的收尾确认）：**未完成**——reviewer 触达所用模型的 5 小时使用上限
  （16:29 中断，18:31 重置）。如实记录，不作为通过。
- R8（对 `d771d5a1..eaf48afc` 末段测试改动，另起一位独立 reviewer）：**APPROVE（绑定
  eaf48afc）**，无 P0/P1。逐项确认：16 处调用点全在返回 void 的测试槽内；失败经
  `QTEST_FAIL_ACTION` 直接从槽返回；无绕过 REQUIRE_BYTES 的磁盘内容比较；`!=` 断言已
  正反两向钉住（改名前无新标题、改名后有新标题）；共享头只被两个 QtTest TU 包含、
  层标记合规；实跑 Totals 与时间戳绑定最终测试源码；388 的更正已落地。
  1×P2（非阻塞）：`REQUIRE_BYTES` 对 `expr` 求值两次，且 `pathFor` 经 `directory()` 会做
  幂等 `mkpath`——已改为宏内只求值一次（552d1191），语义不变、消除重复副作用；改后
  重链两个测试目标 exit 0，tst_h2_* 15/8/13/8 全绿，Source gates 14/14 exit 0。
