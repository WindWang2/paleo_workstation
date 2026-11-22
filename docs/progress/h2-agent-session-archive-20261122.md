# H2｜私有会话档案的有界读写与轻量最近列表（2026-11-22）

- 分支：`codex/h2-agent-session-archive`（base = `origin/master` `8eca4644`）
- Worktree：`../paleo-h2-agent-session-archive`（独立 build；vendor 链接共享主仓
  superbuild/onnxruntime/ranuts-document，deb 闭包前缀共享 H3 的 `vendor/prefix`
  ——只读引用，不在他人 prefix 原地升级；`vendor/Data_service` 按本分支 gitlink
  `905b5c1b` 就位）
- 范围：W1–W8（档案合同/有界读取/容量准入/索引/查询/一致性/Combo/验收）；
  不动 G2 checkpoint/欠账状态机、G1 传输、H1 sendRound 投影、H5 计划面。

## Scope ledger（开工核对，8eca4644 源码实证）

| 工作包 | 开工时生产入口现状（源码观察） | 本任务增量 |
|---|---|---|
| W1 身份/结构合同 | `ChatSession::fromJson` 无 schema/形状/身份检查，`toObject/toArray` 默认值把坏结构映射成空历史 | `chatsessionarchive.*`：`validateChatArchiveObject`（schema=1、id==文件名、字段类型/长度、messages/tool_calls/bindings 形状、消息数护栏）；错误分类 `ChatArchiveReject`；load 失败不部分覆盖 out |
| W2 有界读取 | `load` 直接 `readAll()`，无文件类型/大小/累计上限检查；无符号链接边界 | 常规文件+根/叶一致 canonical 边界（目录/FIFO/叶子链接/中间穿越同拒）；打开前 size 与读取中累计双上限（`readChatArchiveBytes`）；超限旧档案保留字节、只给元信息状态 |
| W3 容量准入 | 无任何发送/重试/改名前置容量概念；`save` 无上限 | `ChatArchiveBudget`（生产上限由 `turnReserve()+titleReserve()+32MiB` 结构式推导，≈322.5 MiB）；`sendUserText` append 前、`retryLast` resize 前、`renameSession` 赋值前三道纯门；`save` 写侧同尺（对称）；G2 欠账语义原样 |
| W4 可重建索引 | 无 | `chatsessionindex.*`：per-session sidecar（`<root>/index/<id>.meta.json`，≤2 KiB，schema=1，id 绑文件名）；sidecar 只存列表元信息+size/mtime 弱指纹；缺失/损坏/过期丢弃重建，永不反向改正文 |
| W5 有界最近列表 | `loadRecent` 全量解析所有正文再排序裁剪；每次 checkpoint 的 `sessionListChanged` → `recentSessions(20)` → 全库正文载入 | `aichatrecentsummaries.*` 协调器：热路径 stat+sidecar/内存缓存（正文解析计数零增长）；冷修复经既有 PaleoTaskService quiet 低优先池任务（分文件协作取消、单飞、#159 连接次序）；无服务=可信缓存 partial，不退回 GUI 线程全库解析；partial/complete 如实发布 |
| W6 一致性 | save/remove 与列表无缓存协同 | 权威 save 成功才写 sidecar（失败只留待修复，不产生假欠账）；remove 同步清缓存；外部删除/替换/坏 sidecar 经弱指纹失配定向修复；索引持久写失败抑制自动重试（候选指纹变化才恢复），权威保存事实不受缓存影响 |
| W7 Combo 接入 | `refreshSessions` 同步 `recentSessions(20)`（全量正文） | `refreshSessions` 只发查询；`rebuildSessionCombo`（当前会话恒首位、QSignalBlocker、超限/损坏有界诊断文案、选择后权威裁决）；草稿/工具卡/选择不受刷新影响 |
| W8 验收 | — | `tst_h2_sessionarchive`（W1/W2/纯准入数学）、`tst_h2_capacitygates`（W3 控制器闭环）、`tst_h2_recentarchives`（W4/W5/W6，含 1000 小档案热列表零正文解析）、`tst_h2_sessioncombo`（W7 真实信号链） |

既有能力复用（不重做）：G2 同步 checkpoint/欠账/删除保护/重试授权
（`settleSaveArrears`、`validateInvocationRetry` 原序保留）；G3 工具表；G4 隔离；
`QSaveFile` 原子写；`displayTitle` 语义；`pathFor` 单段安全名。轮次形状常量
（kMaxToolRounds/kMaxCallsPerTurn/kMaxToolArgumentBytes/8MiB 响应上限）单点
登记到 `chatsessionarchive.h`，controller/llmclient 改为引用（值不变）。

## 容量推导（W3，工程容量选择）

```
escapeWorstFactor = 6            // QString→JSON 每码元最坏 \uXXXX 6 字节；
                                 // UTF-8 网络字节 ≥ UTF-16 码元（各编码逐项可证）
assistant   = (1+kMaxToolRounds) × (6 × 8 MiB + slack)   // 6 条回答（开场+5 工具续轮+终答）
toolResults = kMaxCallsPerTurn   × (6 × (8 KiB 页 + 信封) + 元字段)
toolFrames  = kMaxCallsPerTurn   × (6 × 16 KiB arguments + 名字/id)
bindings    = kMaxCallsPerTurn   × 行定长上界
titleReserve= 6 × maxTitleChars(256) + slack             // 轮中合法 rename 不耗响应余量
turnReserve ≈ 290.4 MiB；maxArchiveBytes = turnReserve + titleReserve + 32 MiB ≈ 322.5 MiB
```

- 空会话一整轮最大合法工作恒可容纳（fresh ≈ 0 + 290.4 ≤ 322.5）；不提高任何
  LLM/工具业务预算（8 MiB/16 次/8 KiB 页全部沿用既有值）。
- 一轮最大工作落盘后（~290 MiB）再发新轮被拒（建议新建会话）；原 newSession/
  导出/G2 欠账出口不受影响。
- 读写对称：save 用同一上限（先序列化后开文件），成功写出的档案自身 load
  恒可接受；准入用「当前精确序列化 + 用户消息精确字节 + 最坏预留」的上界。

## 旧超限档案边界

- > 上限的旧档案：load 明确 `SizeExceeded` 拒绝完整载入（字节保留），最近
  列表给 `OverLimit` 元信息状态（stat-only，不读正文）；仍可经 UI 删除。
- 消息数 > 65536 的档案：`MessageBudget` 拒绝（内存护栏；正常会话远不可及，
  发送门同尺拒绝继续增长）。
- 无 bindings/无标题/对象形态 arguments/旧时间格式的 schema=1 档案照常可读；
  长标题（≤4096 码元）容忍，新写入标题上限 256。

## 本地验证

环境：CachyOS Linux x86_64；Qt 6.10.2 / QGIS vendored（deb 闭包前缀，共享 H3
worktree 的 `vendor/prefix`，`CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`）；
RelWithDebInfo；`-j4`（主机级重型构建互斥锁，五任务并行纪律）。

（以下结果在最终 head 复核后填写）

## 未覆盖/限制

- Windows 特有名（保留名/junction/大小写不敏感归一）未在本机实测——叶子
  链接/junction 走 `isSymLink()` 同拒，目录伪装同拒；真实 Windows 车道由 CI
  windows leg 覆盖，未在本机验证的部分如实记录。
- 同毫秒同大小替换的弱指纹盲区：size+mtime 一致时缓存判新鲜（打开仍走权威
  校验）；确定性规则见 chatsessionindex.h 头注。
- 真实 Pi 进程链/受控 Python 发布链 fixture 本机未提供（同 G2 批次口径）；
  容量预留对 Pi 轮的结构覆盖来自同形消息面（Pi 回答/工具结果与原生共用
  appendMessage/emit 路径）。

## 独立复审记录

（review 后填写）
