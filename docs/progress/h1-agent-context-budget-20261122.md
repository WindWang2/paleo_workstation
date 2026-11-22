# H1｜长会话上下文投影与请求预算（2026-11-22）

分支 `codex/h1-agent-context-budget`，worktree `../paleo-h1-agent-context-budget`，
base `origin/master@8eca4644`（G1–G5 + #1068 整合复审已合入后的主干）。
进度记录 + scope ledger；结论以本文件末尾的验证记录为准。

## Scope ledger（开工静态核对，来源=源码阅读；运行证据见「验证记录」）

生产入口：`AiChatController::sendRound`（aichatcontroller.cpp）→ 追加空
assistant 占位 → `m_client.send(requestMessages(), m_turnTools)` →
`LlmClient::issueRequest` 组 POST body。原生与 Pi 续轮（`modelRequested`）
同经 sendRound。

| 项 | 现状（已核对源码） | 处置 |
|---|---|---|
| 已有能力（保留） | 12 轮/6000 估算 token 历史开窗、按轮丢弃；legacy tool 结果转 envelope（确定性：encode allowCache=false / resultError）；estimateTokens 字符近似；G2 回合冻结（m_turnConfig/m_turnTools/m_turnPrompt）与 m_toolTokens 准入账本；G1 传输冻结 + 8MiB 应答上限 | 不重做；投影只叠加最终装配检查 |
| 缺口 1（W3） | 预算按原 content/arguments 计价，legacy 结果转 envelope 后再膨胀——按短字符串收费、按大 envelope 发送 | 投影先适配成最终形态再计价 |
| 缺口 2（W4） | aichatcontroller.cpp 旧选择循环：某近轮超预算被丢弃后循环继续向更旧轮捞小轮——非连续窗口 | 改为连续后缀选择，遇不可纳入即停 |
| 缺口 3（W2） | 存储内 System 消息原样进请求（旧档案的 System 变成事实高位指令）；截断标记是第二条 System | 请求只保留唯一宿主 System；标记合并进该块 |
| 缺口 4（W1） | sendRound 追加的空 assistant 占位被当作 `{"role":"assistant","content":""}` 发送 | 投影剔除占位（存储不动） |
| 缺口 5（W2） | 仅按 User 边界分组，无 tool_call/应答配对校验——孤儿/重复/跨轮 tool 消息可产出协议非法请求 | 逐轮协议闭合校验；损坏旧轮整轮排除，当前轮损坏即拒绝发送 |
| 缺口 6（W5） | 最新轮无条件保留，单轮超预算也照发 | 当前必需轮超限→明确不可发送原因，零 HTTP，走既有停轮路径 |
| 缺口 7（W6） | 无请求体字节保护；固定 prompt/工具声明/输出预留与历史预算不分账 | 投影诊断分账 + 独立字节闸（256KiB 请求体 / 64KiB 工具声明，依据见组件头注） |
| 缺口 8（W7） | requestMessages 每轮全量复制整段历史、int 累加 | 尾部有界扫描（≤N+1 轮）；qint64 饱和计数；不建缓存（失效面>收益） |

边界（必须不破坏）：G5 tst_g5_conversation_workbench「loadSession 零模型请求、
不改写消息」；tst_aichatcontroller legacyBrokenResults（wire 上
`legacy_result_unavailable`、存储不变）；tst_aichattoolloop 历史窗（仅按
H1-W4 指示把「截断标记=第二条 system」断言精确改为单一 system 内可见说明）；
G2 m_toolTokens 准入合同不动。

重复实现排除：#1067 传输/#1063 checkpoint/#1064 工具准入/#1066 展示均为现状
能力，本任务不重做；H2 档案、H3 plan、H4 发现、H5 工作台不在范围。

## 实现

- 新组件 `src/workflow/aichatcontextprojection.{h,cpp}`（层：功能）：
  纯函数 `projectChatRequest(session, systemPrompt, tools, config, limits)`
  → `{messages, diagnostics}`；`serializeChatRequestBody` 与
  `LlmClient::issueRequest` 同形的纯序列化（供字节闸与 byte oracle；不触碰
  G1 网络代码，等价性由 tst_h1_requestassembly 用真实 POST body 钉住）。
- `aichatcontroller`：删除私有 `requestMessages`；`sendRound` 装配接缝改为
  「投影 sendable 才 `m_client.send(projection.messages, m_turnTools)`」；
  拒绝时 `failTurn(...)` 复用既有停轮路径（stopCurrentRound 撤空占位、保存、
  单次错误上报），零新增 HTTP/工具执行。
- 测试 `tests/tst_h1_contextprojection.cpp`（组件矩阵）、
  `tests/tst_h1_requestassembly.cpp`（真实 QTcpServer HTTP + Pi 受控 peer 链）；
  fragment `cmake/extra-h1-agent-context-budget.cmake`。

## 验证记录（2026-11-22/23，本机 Linux；base=8eca4644，head 见 PR）

环境：Arch 宿主 + 共享 deb 闭包前缀（H3 worktree 的
`vendor/prefix/usr`，stamp `90a7bf…` 与本分支 `deb-closure.lock` 匹配；
configure 显式 `QGIS_PREFIX` + `CMAKE_LIBRARY_ARCHITECTURE`；Arch 宿主
直跑测试需补 `LD_LIBRARY_PATH`/Qt 插件路径——Linux CI 的 Debian13 宿主走
默认路径无此需求）。`vendor/Data_service` 初始化到本分支钉住 gitlink
`905b5c1b`；`vendor/ranuts-document` symlink 接线，configure 无 Office 警告。

源码闸门（全部在最终代码态跑）：layering/--strict ✅、qgs-audit 134=基线 ✅、
qgis_spread 169=基线 ✅、file_budget --strict 51=基线 ✅、i18n ✅、ui_tokens ✅、
docs_drift ✅、cpp_mainline ✅、`--transitive`（deb 头目录全闸）黄牌与基线
同数、功能层 QtWidgets 0 ✅。

受影响测试（本地真实执行，qtest Totals）：

| 套件 | 结果 | 说明 |
|---|---|---|
| tst_h1_contextprojection（新） | 22/22 | 组件矩阵：协议闭合 7 类损坏、连续后缀、envelope 终形计价、唯一 System、占位剔除、字节闸、有界扫描（300 轮史 scanned≤27）、饱和计数（200k CJK 即时拒绝）、分账、确定性逐字节 |
| tst_h1_requestassembly（新） | 10/10 | 真实 HTTP 捕获：单一 System/无空占位、旧档案净化 + 原档不改写、损坏轮线上排除、转换计价整轮截断、当前轮超限零 HTTP + 停轮路径、非均匀轮不跳捞、retry 净后缀、受控 Pi peer 链与纯投影序列化逐字节一致 |
| tst_aichatcontroller | 24/24 | 含 legacyBrokenResults（wire 上 legacy_result_unavailable、存储不变） |
| tst_aichattoolloop | 14/14 | 历史窗断言按单一 System 精确更新，其余（12 轮/估算器/envelope 契约）原样 |
| tst_aichat / tst_aichatprojectquery / tst_aichatcapability | 17/16/9 全绿 | 工具表与查询闭环不受影响 |
| tst_g1_configcommit / tst_g1_credentialboundary | 19/11 全绿 | 传输/配置面回归 |
| tst_g2_turncheckpoint / tst_g2_piterminal | 12/11 全绿 | 回合冻结/欠账/Pi 终态 |
| tst_pichatbridge | 29/29 | legacy 档案 retry 授权矩阵不受影响 |
| tst_g5_conversation_projection / workbench / operation_presentation / assembly | 17/10/14/5 全绿 | loadSession 零请求、历史卡片归属不受影响（assembly 1 项按需截图 skip 为设计内） |
| tst_aiassistdock / tst_aitoolresultview | 11+1skip / 14+1skip | skip 为 PALEO_AI_*_SHOT_DIR 未设的按需截图 |
| tst_aiwiring / tst_agent_readiness / tst_wellfaciesagent | 14/10/20 全绿 | 装配与执行面回归 |

未覆盖/限制：

- 真实 Node Pi runtime 进程链未在本地跑（受控 peer 覆盖同一 sendRound 装配
  路径，模型 HTTP 仍为真实 LlmClient 请求）；
- Windows 平台未本地验证（NativeFormat 注册表路径无变更，风险面为投影纯
  函数与 sendRound 接缝）；
- 8 MiB 应答上限/双时限等 G1 传输行为未重测（未触碰，由 tst_g1_* 回归）；
- 估算器仍为字符近似（ASCII≈4/字符、CJK≈1/字符），不宣称对齐任意 tokenizer。

## 复盘要点（实现中发现并修正）

- 饱和计数初版在「恰好等于预算」处失去区分度（cost==budget 被判可纳入）——
  改为 ceiling=budget+1：等于可纳入、超过即拒，计数仍有界；
- 工具声明字节闸初版 64 KiB 低于当前内置表实测 ~72 KiB（会拒掉所有带工具
  请求）——实测校准为 128 KiB（测试钉住余量）；
- 空工具表的声明字节按「不上送=0」报告（序列化 "[]" 不计成本）。
