# G2｜原生与 Pi 回合执行的持久边界和终态一致性（2026-11-21）

- 分支：`codex/g2-agent-turn-execution`（base = `origin/master` `dd73a322`，PR #1062 后主干）
- Worktree：`../paleo-g2-agent-turn-execution`（独立 build；`vendor/superbuild/prefix`、`onnxruntime`、`ranuts-document`、`prefix` symlink 主仓；`vendor/Data_service` 按本分支 gitlink `905b5c1b` 就位）
- 依赖前缀：`/home/kevin/projects/paleo-deb-prefix-20261119/usr`（deb 闭包路线，与 20261120 批 worktree 同源；`CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu` 显式给入）
- 范围：W1–W8 回合执行闭环；不动 G1 `applyConfig`/llmclient、G3 runner 准入、G4 journal、G5 UI。

## Scope ledger（开工核对）

| 工作包 | 开工时生产入口现状（dd73a322 源码观察） | 本任务增量 |
|---|---|---|
| W1 冻结回合归属 | `startEngineTurn` 已冻结 hostTurn/turnContext；续轮 `sendRound` 每轮重取 `tools()`/`m_config`，中途 setConfig 会改在途回合与记录 model 字段 | `m_turnConfig`/`m_turnTools`/`m_turnFrozen`；`sendRound` 用冻结快照；`setConfig` 不再直写 `session.model`，落盘经 `currentSessionModel()` |
| W2 结果先落盘再续轮 | `onToolFinished` 仅 append 后即回送 Pi/发下一请求，无结果落盘保证 | 内存完整状态 append 后、任何对外通知/续轮前 `saveCurrentSession` 提交点；失败→`failTurnForSaveArrears` 停队列停续轮，结果保留内存不改判 |
| W3 存储/终答闭环 | 各停轮路径 `saveCurrentSession()` 返回值被忽略；`newSession/loadSession` 无条件换会话；`removeSession(currentId)` 在 `newSession()` 后无条件删文件 | `m_saveArrears` 记账（saveCurrentSession 单点）；终答/Pi 收尾/工具结果三处保存失败均诚实上报；`newSession/loadSession` 欠账薄门（拒绝切换+恢复 Combo、不发 sessionChanged）；`removeSession` 切换被拒即拒绝删除；`retryLast` 先清欠账；statusText 呈欠账状态 |
| W4 单次结算 | 取消/错误配对、世代屏障已存在；新增保存窗口未接入 | 保存失败统一走 `stopCurrentRound(false)` 既有单次结算；取消在保存窗口内先到则取消为唯一结算（测试覆盖） |
| W5 Pi 根因/清理 | `finished` 处 stopReason 优先于 `m_failure`，fail 的根因被降级 interrupted；controlTick 任务缺失路径 finished 固定 interrupted | 优先级=首个宿主失败>停止原因>worker 结果；`fail()` 停止后失败不再抢占；任务缺失终态=根因；新增 `cleanupNotice`（worker 回收失败不隐藏，不改主因） |
| W6 Pi 协议/背压 | controller 两处 `modelFinished` 返回值未检查；`frame()` 任意帧刷新控制时限（重复帧可无限续命） | 返回值检查+`pi_model_completion_rejected` 兜底；controlAge 只在真实进展（model_request/派发/idle 受理）刷新；`setWorkerDriver` 测试缝（受控 peer 顶替 Node，桥路径全真） |
| W7 恢复与重试 | `validateInvocationRetry/bindInvocations` 已有；retry 不处理保存欠账 | `retryLast` settle 欠账后截断重发；「已提交→保存失败→恢复→retry」链新增 `committedOperationSurvivesSessionSaveFailureAndRetry`（tst_pigeneratedoperations，受控 Python 环境执行） |
| W8 诊断矩阵 | — | `tst_g2_turncheckpoint`（原生+受控 Pi 两链一致性、W1–W4 oracle）、`tst_g2_piterminal`（W5/W6 桥协议矩阵：正常闭环/违约根因/取消+回收补充/任务销毁/满环背压/重复帧不刷新/缺 idle 有界超时/错 round/迟到拒绝） |

## 行为语义（PR 描述要点）

- **四个结果分别结算**：transport（LlmClient/Pi 通道）、执行（runner toolFinished ok）、提交（operation receipt/Catalog）、会话存储（ChatSessionStore.save）。其一失败不冒充其它成功；已提交事实不被会话写失败撤销。
- **保存欠账恢复**：内存保留 + statusText 如实 + newSession/loadSession/removeSession 薄门拒绝无声丢弃；`retryLast`（或任何下一次成功保存，含下轮保存）先清欠账再续轮。**崩溃边界**：进程突然终止时未落盘内存内容不可恢复——重启只信磁盘 admitted bindings 与 operation 记录；本任务不为此引入第二套日志。
- **retry 何时允许**：不在流式、Pi worker 已收尾（busy 解除）、绑定校验通过、保存欠账可清。legacy 纯读照旧可重试；effectful 缺绑定仍走原操作恢复。
- **Pi 终态规则**：首个宿主侧失败为主因；其后的用户取消/清理失败为补充（cleanupNotice），不互相冒充；busy 直到 worker 真结束或任务销毁兜底。

## 本地验证

环境：CachyOS Linux x86_64；Qt 6.10.2（deb 闭包前缀）；RelWithDebInfo；`-j4`。
构建/测试环境：`QGIS_PREFIX=/home/kevin/projects/paleo-deb-prefix-20261119/usr`
（+`CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`），测试直跑二进制（deb 前缀
expat 1.11 与系统 ctest 不兼容，ctest 闸门脚本改为直接运行同一脚本）。

### 专项新增（19 例全绿）

- `tst_g2_turncheckpoint`：8/8——首项结果保存失败停队列停续轮（内存/磁盘
  oracle + 恢复后 retry 收口）、终答保存失败诚实上报 + newSession/loadSession/
  removeSession/retryLast 薄门、中途 setConfig 不劫持在途回合（请求体断言
  冻结模型）、**受控 Pi 链**（真 controller + 真 CapabilityHost 查询 +
  FakeNode peer；第二项工具派发前 gate 在磁盘观察到第一项结果）、Pi 链保存
  失败与原生同口径结算、保存失败窗口内取消单次结算。
- `tst_g2_piterminal`：11/11——正常两轮闭环 ok（failed/cleanup 零次）、握手
  违约根因不被降级（finished=pi_conversation_identity）、用户取消保持 cancelled
  且回收失败走 cleanupNotice、任务销毁终态=根因、满 16 帧环背压可恢复（17 片
  不丢、busy 不假失败）、重复帧风暴 15s 控制时限内有界失败（wall<24s，
  toolRequested 恒 1、已完成结果合法重发）、缺 idle 有界超时（无凭空工具派发）、
  错 round 立即 pi_tool_state、关闭后迟到 modelFinished/modelDelta/toolFinished
  拒绝且零新信号。

### 回归（既有受影响入口，全部绿）

| 测试 | 结果 |
|---|---|
| tst_aichatcontroller | 24/24 |
| tst_pichatbridge | 29/29 |
| tst_chatinvocationbinding | 8/8 |
| tst_piconversation / tst_piprotocol | 15/15、21/21 |
| tst_pigeneratedoperations | 6/6（受控 Python 环境缺失，真实发布链按门跳过） |
| tst_aichattoolloop（ORT 门内） | 14/14 |
| tst_aichat / projectquery / capability / aiwiring / chatresults | 17、16、9、14、16 全绿 |

### Source gates（脚本直跑，含 strict 变体）

layering（含 --strict / --qgs-audit --strict / --transitive with
QGIS_PREFIX）、file_budget --strict（51 基线 0 新增）、i18n、ui_tokens、
qgis_versions、docs_drift、ui_invariants、ui_size、qgis_spread（169 基线
0 新增）、cpp_mainline（cmake -P）全部通过；check_ts_stale 由 8→0
（翻译补 8 条，仅本任务文案增量 +80 行）；check_l10n 的 27 处 unfinished
为主干存量（stash 验证与本次改动无关）。

## 未覆盖/限制

- **真实 Pi 进程链**（PALEO_TEST_PI_NATIVE_ROOT）与**受控 Python 发布链**
  （PALEO_TEST_AGENT_PYTHON_ROOT）本机未提供 fixture：`realPi*` 既有测试与
  `committedOperationSurvivesSessionSaveFailureAndRetry`（新增，挂
  tst_pigeneratedoperations）按环境门诚实跳过，未在本机运行。引擎一致性由
  受控 peer 全链（宿主工具/模型 HTTP 全真）覆盖；发布一次性由既有
  tst_pigeneratedoperations 主链在具备环境的跑道上验证。
- 进程突然终止时未落盘的内存内容不可恢复（重启只信磁盘 admitted bindings
  与 operation 记录）——设计边界，非缺陷；未为此新增第二套日志。
- 保存欠账的呈现复用既有 errorOccurred/statusText 信号，未要求 G5 新 API。

## 独立复审记录

- R1（未参与实现的独立 reviewer，对 440dd54f 全量 diff + 调用链 + 自行复跑）：
  **APPROVE**，0×P0/P1，5×P2（rename 绕过欠账记账、冻结跨会话残留、
  loadSession 无出参时提示不一致、进度文档占位、interrupted 触发清理补充）。
  reviewer 独立复跑：tst_g2_turncheckpoint 8/8、tst_g2_piterminal 11/11、
  tst_aichatcontroller 24/24、tst_pichatbridge 29/29 及 touched gates 绿。
- 修复：98e4ab5b（冻结跨会话失效）、dc91e594（文档证据）、25271ac3
  （rename 走统一落盘/拒绝提示一致/interrupted 不发清理补充）。
- R2（对 440dd54f..25271ac3 复审）：**re-verify clean**——五项 P2 全闭环，
  无回归；最终立场 APPROVE。残留说明：重入监听器在 sequence 守卫处中断切换
  会提前失效冻结，后果仅是后续落盘的 model 字段取值（startEngineTurn 恒重冻结，
  无请求身份影响）。
