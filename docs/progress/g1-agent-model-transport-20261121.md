# G1 模型通信（有界传输、流式协议与配置一致性）——进度账本

- 分支：`codex/g1-agent-model-transport`（worktree `../paleo-g1-agent-model-transport`）
- Base：`origin/master@dd73a322da570e5ad9ce1ddc6b3c1042c5c24788`（2026-11-21 快照，与任务书一致）
- Head（提交 review 时点）：`135ec48f`
- OS：Linux x86_64（cachyos，glibc ≥ 2.43）；依赖前缀：`/home/kevin/projects/paleo-deb-prefix-20261119/usr`（共享只读，QGIS 4.2.3，带 `.paleo-deb-closure.sha256` 收据）；`vendor/Data_service` 钉 `905b5c1b`（分支 gitlink）；`vendor/ranuts-document` symlink 主仓，configure 期 `office-editor 已就绪`（无警告）。
- 专属挂点：`cmake/extra-g1-agent-model-transport.cmake`；根 manifest 仅新增一行 include（保留全部并行条目）。

## Scope ledger（开工静态核对，2026-11-21）

| 工作包 | 生产入口（当前） | 已具备 | 待验证/缺口 | 实施增量 | 验收 oracle |
|---|---|---|---|---|---|
| W1 attempt 冻结 | `LlmClient::send/issueRequest/readyRead/finished` 全读 `m_config` | 世代号作废迟到回调、QPointer reply | 在途请求无独立配置快照；`setConfig` 可改变上一轮回调行为 | 私有 `Attempt`（冻结 endpoint/model/stream/key/idle/total）；全部信号/计时器/解析/清理绑 attempt；readyRead 先查 HTTP 状态 | A 在途中改 stream/endpoint 再发 B：请求体 URL/Accept/解析方式按各自快照归属 |
| W2 双时限 | `m_timer` 单一 idle | 收字节重置 idle、世代失效 | 无绝对总时限；心跳可无限续命 | `totalTimeoutMs`（默认 300000，域 [1000,3600000]，旧 JSON 缺省→默认）+ 单发总时限计时器（不重置） | 无数据→idle；心跳<idle 但永不结束→total 有界终止；文案含「总时限/空闲超时」区分，enum 不变 |
| W3 SSE 分帧 | `LlmStreamParser::feed` 整体 replace CRLF→LF | 多段 arguments、空行分隔、注释忽略 | 跨块 CRLF 拆字节、裸 CR、BOM、字段前导空格精确剥离、[DONE] 后同块内容 | 真·增量行/事件解析（WHATWG event-stream 口径），EOF 终态 | 同一响应按每字节切分 == 一次喂入（表驱动全边界等价） |
| W4 工具调用完整性 | `takeCompleteCalls`：临时合法 JSON 即产出 | index 排序、空 arguments 不提前产出、flush 兜底 | 临时合法后追加会推翻已发调用；choices 混拼；同 index 身份冲突无声覆盖 | 封印（sealed）状态机 + 封印序产出；只认 choice 0；身份冲突报错；终局校验（非空残缺不得改 {}） | 临时合法后追加/同 index 换 id/缺 name/异常大 index/多 choice 同 index → 无工具执行+如实错误 |
| W5 统一裁决 | readyRead([DONE]→finishUp) 与 finished 各自收尾 | HTTP 分类优先、非 SSE JSON fallback、tail 并入 | 非 2xx 带 SSE 外形在 readyRead 已派发内容；200 error 信封/空事件流成功；finish_reason 恒 stop | readyRead 先查 HTTP 状态；所有终路径汇入 attempt 单一终态；finish_reason/usage 透传（缺省退化 stop） | 401 带 [DONE]→零增量+Unauthorized；尾坏 JSON→Malformed 无 finished；无 [DONE] 正常关闭→容错 flush；length 不改写 |
| W6 预算 | readyRead 累计 8 MiB | 到货计数、TooLarge | finished tail 未计数；溢出加法；body/缓冲双份保留 | tail 并入计数；sawStreamEvent 后释放 m_body；差值比较防溢出 | 刚好 8 MiB 成功；+1 字节（任意 readyRead/tail 分布）→TooLarge；无换行 8MiB 单行→TooLarge |
| W7 配置提交 | `applyConfig`：先写文件→异步 key→回调 | 结构校验、QPointer 双守卫、newKey 优先/clear/沿用语义 | key 失败后文件已更新（重启=新端点+旧 key 误启用）；并发提交回调竞态 | `LlmConfigCommitter`（ai/chat 私有）：串行队列（实际存储次序=提交次序）+文件快照补偿+双重失败降级清 key；可注入 hooks 测试 | 文件/key 任意一步失败、恢复失败、并发两次保存、重启组合全部一致；旧回调不覆盖新配置 |
| W8 凭据边界 | `runFaciesModels` 成功 payload `size()>=8 && bytes.contains(key)`；失败 reason 直传 | 有 ≥8 长度门槛的字节包含检查 | 短 key（<8）回显漏检；JSON 转义可绕过 bytes.contains；reason/诊断/Location 无统一脱敏 | `llmcredentialguard`（结构化值等值+≥8 包含双防线，任意非空长度）；LlmClient 诊断按 attempt key 脱敏；runner 两出口按请求时 key 捕获 | 短/长/转义/非 ASCII key 回显→拒/脱敏；正常非 ASCII 文本不受损；失败原因仍可辨识 |

### 相关 PR/分支核对（开工时）

- 开放 PR：0。`codex/swarm-review-fixes` 已 squash 合入 #1057 并清理。
- #966：仅作总时限与凭据出口线索（operation/路径部分属 G4，本任务不动）。
- #1038 已修 Pi 小片段预算/Windows handle——不重复。
- 主 checkout 未提交改动（chatsession/llmclient/appcontext/aiassistdock/…）属其他会话，保留不动。

## 实现记录（2026-11-21）

### 提交历史

| commit | 内容 |
|---|---|
| `c85c0515` | WIP：八个工作包生产实现 + 三套测试初版 |
| `135ec48f` | 收口：attempt 双时限、SSE 封印序分帧、统一终态、串行配置提交、结构化凭据防线；三套 G1 测试全绿 |

### 生产改动摘要

1. **`llmclient.h/.cpp`（独占）**
   - 新增 `LlmConfig::totalTimeoutMs`（默认 300000；`load` 钳 [1000,3600000]；`save` 持久化；旧 JSON 缺字段按默认值——向后兼容）。
   - `LlmStreamParser` 实现拆到 `llmstreamparser.cpp`（函数体规模闸友好）。
   - 私有 `Attempt{generation, config}` 冻结快照；`m_attempt` 是唯一配置来源（URL/model/stream/key/双时限）；`setConfig` 只写 `m_config`。
   - 双计时器：`m_timer`（idle，readyRead 重置）+ `m_totalTimer`（单发绝对时限，任何到货不重置）。同报 `LlmErrorKind::Timeout`，文案「空闲超时」/「总时限」区分；`request.setTransferTimeout(0)` 保持（避免双重超时语义）。
   - 全部终态经 `fail(gen,…)`/`finishUp(gen,…)`：`alive(gen)` 闸 + 世代自增（单终态）+ 双计时器停止 + `releaseReply()`（abort+deleteLater+clear）。
   - readyRead：先查 HTTP 状态（≥300 直接返回，不派发内容、不解析）→ 字节计数（`countBytes` 差值比较防溢出）→ 流式 feed → 增量/工具 emit 前后重查 `QPointer self` 与世代 → `done` → `finalizeStream`。
   - finished：HTTP 分类优先（3xx 报 Network 且 Location 按 attempt key 脱敏；401/403→Unauthorized；429；5xx；4xx）→ 传输错误 → 非流式解析 → `sawStreamEvent` 分支（tail + `"\n\n"` 补 feed + `finalizeStream`）→ 非 SSE 兼容层（内容类型不匹配或 chat completions 形状）→ 200 error 信封拒绝 → 零事件流拒绝。
   - `finalizeStream`：`parser.flush(&err)`（终局校验）→ 工具 emit → `finishUp(reason/prompt/completion)`；`finish_reason` 透传，缺省退化 `"stop"`。
   - W6：`m_body` 一旦确认真流式即释放；tail 与 readyRead 同一 `m_received` 口径。
   - W8：`fail` 出网前 `redactCredential(message, m_attempt.config.apiKey)`。
   - 测试注入口：`LlmClient(QNetworkAccessManager*, QObject*)`（NAM 归调用方所有，防栈注入双重析构）。
2. **`llmstreamparser.cpp`（新）**：字节级增量行扫描（LF / CRLF / 裸 CR；末尾孤立 CR 留待下一块）；流首 BOM 一次性剥离（EF 开头但不足三字节留待）；字段名后单前导空格；注释行/无关字段忽略；data 多行按 `'\n'` 连接；空行分发；`[DONE]` 置 `m_terminal`（终局后输入与产出全停）；choice 隔离（`index` 缺省按 0，非 0 一律丢弃）；工具草稿封印：非空 arguments 拼成预算内合法 JSON 对象即 `argumentsComplete`+封印序；`takeCompleteCalls` 流中按封印序产出（切分不变）、终局裁决剩余草稿（空实参=无参调用；非空未封印=残缺/非对象/超预算 → 错误；缺 name → 错误）；同 index 已交付后再到分片 → 「在完整交付后再次收到分片」；id/name 前后冲突 → 报错不静默覆盖；`finishReason()` 取 choice 0 最后一个非空 finish_reason；`promptTokens/completionTokens` 取最后一次 usage。
3. **`llmcredentialguard.{h,cpp}`（新）**：`valueDisclosesCredential`（任意非空长度等值 + ≥8 字节包含式纵深）；`jsonDisclosesCredential`（递归对象/数组/键名的结构化字符串值判定，绕不开 JSON 转义）；`redactCredential`（空 key 先行跳过；全部出现替换为 `[已隐藏]`）。
4. **`llmconfigcommit.{h,cpp}`（新，功能层私有）**：`LlmConfigCommitter` 串行提交协议。结构校验 → 快照旧文件原文 → 写新配置（`LlmConfig::save`，原子，无密钥）→ 密钥操作（newKey 写 / clearKey 删 / 沿用不动）串行链（`m_inFlight` 闸；实际存储次序=提交次序）→ 失败补偿（还原旧原文；无旧文件或还原失败 → 清钥匙串；清也失败 → 报错点名须手动重新配置）。`completeHead`：出队 → apply（无条件，协议链不依赖 UI 生命周期）→ done（仅 callbackContext 存活时）。生产 hooks = `defaultLlmConfigCommitHooks()`（LlmConfig::save + QSaveFile 原文还原 + LlmKeyStore）。
5. **`aichatcontroller.{h,cpp}`（精确接缝）**：新增 `LlmConfigCommitter m_configCommitter` 成员 + `setLlmConfigCommitHooksForTesting`；`applyConfig` 改为委托 submit（签名/语义不变：结构失败同步拒绝；newKey 优先/clear/沿用；成功经现有 `setConfig` 生效；done(ok) 时已生效）。未动 G2 的 `setConfig`/发送/停止/重试/回合接线。
6. **`aichattoolrunner_facies.cpp`（精确接缝二）**：`runFaciesModels` 捕获 `requestApiKey`（本次请求实际凭据，配置切换后仍按旧 key 检）；成功 payload 改 `jsonDisclosesCredential(QJsonValue(payload), requestApiKey)`（任意非空长度、结构化、绕不开转义）→ 拒绝 `model_catalog_contains_credential`；`failed` 回调 reason 按 `requestApiKey` 脱敏后仍回灌（原因可辨识）。未动预测/目录查询业务、inputContract/schema。
7. **`cmake/extra-g1-agent-model-transport.cmake`（新）**：三个新 TU 挂 `paleo_ai`；三个 `add_paleo_test`（`tst_g1_modeltransport` LIBS paleo_ai；`tst_g1_configcommit` LIBS paleo_workflow+paleo_ai；`tst_g1_credentialboundary` LIBS paleo_app+Paleo::paleo_data_service）。`ctest -N` 核实注册（Test #502 等三项）。

### 验证证据（本地实际运行）

环境：`LD_LIBRARY_PATH=$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu:$PWD/vendor/prefix/usr/lib`、`QT_PLUGIN_PATH=$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu/qt6/plugins`（ctest 宿主本身与 deb 前缀 expat 冲突，直接跑二进制等价口径）。

| 套件 | 命令 | 结果 |
|---|---|---|
| 传输回归 | `./build/tst_g1_modeltransport` | **51 passed / 0 failed**（4.8s） |
| 配置提交 | `./build/tst_g1_configcommit` | **18 passed / 0 failed** |
| 凭据边界 | `./build/tst_g1_credentialboundary` | **11 passed / 0 failed**（490ms） |
| 既有 parser | `./build/tst_aichat` | 17 passed / 0 failed（含 SSE 拆包、工具草稿、配置校验用例——契约变更点见下） |
| 原生 controller | `./build/tst_aichatcontroller` | **24 passed / 0 failed**（含 streaming/工具帧/取消/401/plainJson/idle 超时/applyConfig 既有用例） |
| Pi 兼容 | `./build/tst_pichatbridge` | 29 passed / 0 failed |
| 工具循环 | `./build/tst_aichattoolloop` | 14 passed / 0 failed |
| 配置对话框 | `./build/tst_llmconfigdialog` | 9 passed / 1 skipped |
| readiness | `./build/tst_agent_readiness` | 10 passed |
| project query | `./build/tst_aichatprojectquery` | 16 passed |
| 结果视图 | `./build/tst_aitoolresultview` | 10 passed / 1 skipped |
| 装配 | `./build/tst_aiwiring` | 14 passed |
| dock | `./build/tst_aiassistdock` | 11 passed / 1 skipped |
| capability host | `./build/tst_capability_host` | 12 passed |
| Pi 生成操作 | `./build/tst_pigeneratedoperations` | 5 passed |
| 测井相 agent | `./build/tst_wellfaciesagent` | 20 passed（含 `modelCatalogQueryDisclosesReadinessWithoutWellRead`——目录查询路径回归） |

编译：`ninja -C build -j2 paleo_ai paleo_workflow <上述测试目标>` 零 error（-j2：主机多 Agent 并行构建配额）。

Source gates（`tools/` Python 检查器 + `cmake -P`）：
`check_layering.py --strict` 通过（无层违规）；`check_file_budget.py --strict` 51 项全在 baseline（新增 0/回升 0/可收紧 0）；`check_i18n.py` 通过；`check_ui_tokens.py` violations 0；`check_docs_drift.py` 三项绿；`check_qgis_spread.py` 169=baseline 新增 0；`check_layering.py --qgs-audit` 134=baseline 新增 0；`verify-cpp-mainline.cmake` OK。
（`--transitive` 的 func/ui 两指标需 QGIS 头目录，属 CI lint 车道权威闸；本地以 `--qgs-audit` 现状冻结口径核对，未注入 QGIS_PREFIX 跑 full-audit。）

### 契约变更说明（既有测试/调用面）

- `LlmStreamParser` 头文件声明从「实现内联」改为拆 TU；公开方法签名保持（`feed`/`takeCompleteCalls`/`flush`），新增带 error 的重载与 `markTerminal`/`terminal`/`finishReason`；旧调用面（tst_aichat 既有 parser 用例）全部通过，无需改断言。
- `LlmClient` 公开 `send/cancel/busy/config/setConfig` 与四信号签名不变；新增测试注入构造（第二构造，隐式转换面不变）。
- `AiChatController::applyConfig` 签名与 callback 语义不变（done(ok) 时配置已生效）。
- `runFaciesModels` 拒绝码 `model_catalog_contains_credential` 语义收紧：从「≥8 字节 key 的序列化包含」扩到「任意非空长度 key 的结构化回显」。正常目录（无凭据回显）不受影响——`tst_wellfaciesagent` 20 项回归为证。
- 新增 JSON 字段 `total_timeout_ms`（缺省 300000；旧配置文件读入即补默认，不影响既有行为）。

### 未覆盖项 / 限制（如实记录）

- 真实系统钥匙串（libsecret/Windows 凭据管理器）路径未在本地跑：`PALEO_HAVE_QTKEYCHAIN` 已启用，但钥匙串操作经注入式 FakeStore 测试；`defaultLlmConfigCommitHooks` 的真实 LlmKeyStore 路径只做了编译与契约核对，未做真实钥匙串 round-trip（需人工/交互环境）。
- Windows 平台未跑（本任务在 Linux 验证）；NativeFormat QSettings/注册表隔离是 Windows 侧既有机制，本任务未触碰。
- `--transitive` func/ui 两指标未本地全闸（需 QGIS 头目录；权威在 CI lint 车道）。
- SSE `feed` 的行消费用 `QByteArray::remove(0,n)`（每行 O(剩余)）。已评估：m_pending 只保留未成行字节（SSE 行长有界，[DONE]/心跳场景极小），且「一次 feed 内含极大量行」在 chat completions 流式场景不现实（每事件 ≤ 数百字节）；未改为滚动偏移实现，避免为理论风险引入新复杂度。如评审认为需要，可改 index 滚动。
- 未查询/等待托管 CI；未自动合并。

## Review 记录

- 2026-11-21：独立 reviewer（未参与实现）审 `dd73a322..135ec48f` 真实 diff——进行中，结论待补。
