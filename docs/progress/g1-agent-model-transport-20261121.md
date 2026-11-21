# G1 模型通信（有界传输、流式协议与配置一致性）——进度账本

- 分支：`codex/g1-agent-model-transport`（worktree `../paleo-g1-agent-model-transport`）
- Base：`origin/master@dd73a322da570e5ad9ce1ddc6b3c1042c5c24788`（2026-11-21 快照，与任务书一致）
- OS：Linux x86_64（cachyos，glibc ≥ 2.43）；依赖前缀：`/home/kevin/projects/paleo-deb-prefix-20261119/usr`（共享只读，QGIS 4.2.3，带 `.paleo-deb-closure.sha256` 收据）
- 专属挂点：`cmake/extra-g1-agent-model-transport.cmake`；根 manifest 仅新增一行 include。

## Scope ledger（开工静态核对，2026-11-21）

| 工作包 | 生产入口（当前） | 已具备 | 待验证/缺口 | 实施增量 | 验收 oracle |
|---|---|---|---|---|---|
| W1 attempt 冻结 | `LlmClient::send/issueRequest/readyRead/finished` 全读 `m_config` | 世代号作废迟到回调、QPointer reply | 在途请求无独立配置快照；`setConfig` 可改变上一轮回调行为 | 私有 `Attempt`（冻结 endpoint/model/stream/key/idle/total）；全部信号/计时器/解析/清理绑 attempt | A 在途中改 stream/endpoint 再发 B：请求体 URL/Accept/解析方式按各自快照归属 |
| W2 双时限 | `m_timer` 单一 idle | 收字节重置 idle、世代失效 | 无绝对总时限；心跳可无限续命 | `totalTimeoutMs`（默认 300000，域 [1000,3600000]，旧 JSON 缺省→默认）+ 单发总时限计时器（不重置） | 无数据→idle；心跳<idle 但永不结束→total 有界终止；文案含「总时限/空闲超时」区分，enum 不变 |
| W3 SSE 分帧 | `LlmStreamParser::feed` 整体 replace CRLF→LF | 多段 arguments、空行分隔、注释忽略 | 跨块 CRLF 拆字节、裸 CR、BOM、字段前导空格精确剥离、[DONE] 后同块内容 | 真·增量行/事件解析（WHATWG event-stream 口径），EOF 终态 | 同一响应按每字节切分 == 一次喂入（表驱动全边界等价） |
| W4 工具调用完整性 | `takeCompleteCalls`：临时合法 JSON 即产出 | index 排序、空 arguments 不提前产出、flush 兜底 | 临时合法后追加会推翻已发调用；choices 混拼；同 index 身份冲突无声覆盖 | 已产出 index 再到分片→Malformed；只认 choice 0；身份冲突报错；终局校验（非空残缺不得改 {}） | 临时合法后追加/同 index 换 id/缺 name/异常大 index/多 choice 同 index → 无工具执行+如实错误 |
| W5 统一裁决 | readyRead([DONE]→finishUp) 与 finished 各自收尾 | HTTP 分类优先、非 SSE JSON fallback、tail 并入 | 非 2xx 带 SSE 外形在 readyRead 已派发内容；200 error 信封/空事件流成功；finish_reason 恒 stop；tail parseError 未检 | readyRead 先查 HTTP 状态；所有终路径汇入 attempt 单一终态；finish_reason/usage 透传（缺省退化 stop 并文档化） | 401 带 [DONE]→零增量+Unauthorized；尾坏 JSON→Malformed 无 finished；无 [DONE] 正常关闭→容错 flush；length 不改写 |
| W6 预算 | readyRead 累计 8 MiB | 到货计数、TooLarge | finished tail 未计数；溢出加法；body/缓冲双份保留 | tail 并入计数；sawStreamEvent 后释放 m_body；单次计数不重复 | 刚好 8 MiB 成功；+1 字节（任意 readyRead/tail 分布）→TooLarge；无换行 8MiB 单行→TooLarge |
| W7 配置提交 | `applyConfig`：先写文件→异步 key→回调 | 结构校验、QPointer 双守卫、newKey 优先/clear/沿用语义 | key 失败后文件已更新（重启=新端点+旧 key 误启用）；并发提交回调竞态 | `LlmConfigCommitter`（ai/chat 私有）：串行队列（实际存储次序=提交次序）+文件快照补偿+双重失败降级清 key；可注入 hooks 测试 | 文件/key 任意一步失败、恢复失败、并发两次保存、重启组合全部一致；旧回调不覆盖新配置 |
| W8 凭据边界 | `runFaciesModels` 成功 payload `size()>=8 && bytes.contains(key)`；失败 reason 直传 | 有 ≥8 长度门槛的字节包含检查 | 短 key（<8）回显漏检；JSON 转义可绕过 bytes.contains；reason/诊断/Location 无统一脱敏 | `llmcredentialguard`（结构化值等值+≥8 包含双防线，任意非空长度）；LlmClient 诊断按 attempt key 脱敏；runner 两出口按请求时 key 捕获 | 短/长/转义/非 ASCII key 回显→拒/脱敏；正常非 ASCII 文本不受损；失败原因仍可辨识 |

## 相关 PR/分支核对（开工时）

- 开放 PR：0。`codex/swarm-review-fixes` 已 squash 合入 #1057 并清理。
- #966：仅作总时限与凭据出口线索（operation/路径部分属 G4，本任务不动）。
- #1038 已修 Pi 小片段预算/Windows handle——不重复。
- 主 checkout 未提交改动（chatsession/llmclient/appcontext/aiassistdock/…）属其他会话，保留不动。

## 记录

- 2026-11-21：开工；账本建立；实现进行中。
