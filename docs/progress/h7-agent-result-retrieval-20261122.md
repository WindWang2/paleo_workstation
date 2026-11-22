# H7｜临时工具结果的端到端有界检索与完整性（2026-11-22）

- 分支：`codex/h7-agent-result-retrieval`
- worktree：`../paleo-h7-agent-result-retrieval`
- base：`origin/master = 8eca4644db31b4dcfd5ff11bebf39f0bbcfaf3a5`
  （2026-11-22 fetch；0 个开放 PR，与任务书研究快照一致）
- 依赖前缀：`vendor/prefix`（项目 deb 闭包，只读复用；Qt 6.10.2 / QGIS 4.2.3 /
  GDAL / PROJ / GEOS）+ `vendor/onnxruntime`；`vendor/ranuts-document` 已接线
  （configure 报 office-editor 就绪）。

## 问题与目标

模型请求的 `max_bytes`（默认 8192，可到 65536）与宿主本次调用实际可发送的
回复配额是两层预算。此前读取端只按前者减去固定 384 字节生成内层页，最终
`aichattoolrunner_results.cpp::modelResult` 再按控制器更小的配额包装；一旦
内层页超过最终配额，`transportResult` 会删掉 `data`，页内 `nextCursor` 也
随之丢失——模型无法继续读取，且原业务不能重跑。H7 收口这两层预算之间的
接口与生命周期。

## 生产实现

| 工作包 | 改动 | 关键点 |
|---|---|---|
| W1 双层契约 | 新增 `src/workflow/toolreplybudget.h::replyQuotaBytes`（纯值，与 `onToolFinished` 原公式同源）；`ChatResultStore::read(request, hostQuotaBytes=0)` + `lastReadEnvelopeBytes()` | 有效上限 = min(请求 max_bytes, 宿主 quota, 64 KiB)，只向下收紧；最终化按**实际生效值**复核（不是只按宿主配额）；旧 `read(request)` 兼容 |
| W2 派发冻结 | `dispatchTurnTool` 计算并随 `run(call, quota)` 传入；`QueuedCall.quotaBytes`；`modelResult` 以生效值复核 | native 首次/后继与 Pi `toolRequested` 同一点；读取端不读 args/历史推导配额；最终化额度小于页预算 → `tool_reply_budget_changed`（`retry_read_same_cursor`），不重选页/不重跑；其他工具保留原降级语义 |
| W3 最终 envelope 计价 | `toolresultprotocol`：`resultEnvelopeForBudget` / `resultEnvelopeBytes` / `boundedResultError`；`encode` 首段复用 | 计价对象含 `transport.maxBytes` / `coverage` / `data` / `nextCursor`；删除固定 384 预留；成功页 data 与 nextCursor 同达，不再被最终包装降级 |
| W4 容器分页 | `containerPage` 以 `fitsEnvelope` 试算 | nextCursor 指向实际发出的最后一条之后；child selector 仍用同一引用；无进展页显式失败 |
| W5 Unicode 片段 | `stringPage` 以 `fitsEnvelope` 二分 | 片段按 UTF-16 偏移续读、代理对不拆；`offset/returned/totalCodepoints` 与最终字节一起计价；最小片段放不下时稳定 `value_exceeds_page_budget` |
| W6 生命周期 | `read` 先 `expire()`，到期值当场释放字节并保留有界 tombstone | 读取时点 TTL 语义；scope 变化仍由 `setScope` 清空；捕获页只在本次调用内存活 |
| W7 失败与 no-recache | `modelResult` 对已交付读页只复核一次（`m_deliveredReadCallId/Quota`，后者记录生效预算），`allowCache=false` 保持 | 读取失败/过期/配额不足均是最小合法 envelope；`ok`/`transport`/Pi 外层 ok 同源；读页不签发 result_ref；读页预算失败的建议是重试同一 cursor，不是 do_not_repeat_execution |
| 文案 | `domaintools.cpp` read_result `max_bytes` 说明改为「请求上限，宿主可能更严」；`translations/*.ts` 同步该条 | 只改本任务文案，未重排全库 |

## 保持不变

- G1 模型网络/SSE/配置提交、G2 checkpoint/保存欠账/身份/派发顺序、G3 工具表
  与外部严格校验、G4 operation 日志/隔离、H1 历史选择/`sendRound`、H2 档案、
  H3 plan、H4 operation_*、H5 plan preview、H6 诊断、Pi bridge 与 controller
  构造均未改；runner 仅新增 per-call quota 携带、read_result 分支与
  `modelResult` 复核。
- 1 MiB/8 MiB/128 句柄/30 分钟 TTL、8 KiB 单页上限、768 下限、5 次工具往返、
  每回复/每轮调用上限、历史 token 预算全部原值。

## 验证

环境：Linux x86_64、GCC 16.2.1、CMake/Ninja、RelWithDebInfo；依赖前缀为项目 deb
闭包（只读复用：Qt 6.10.2 / QGIS 4.2.3 / GDAL / PROJ / GEOS）。本机 `/tmp`
是 10 MB tmpfs，编译临时文件必须 `TMPDIR=<worktree>/build/tmp`；deb 前缀的
`rcc`/`lrelease` 需要 `LD_LIBRARY_PATH=<prefix>/usr/lib/x86_64-linux-gnu:<prefix>/usr/lib`
才能加载同源 Qt（否则命中系统 libQt6Core 的符号冲突）。

构建：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`；
`cmake --build build --target tst_h7_resultbudget tst_h7_resultwire -j4`。
`ctest -N -R h7` 注册为 #513 / #514。

运行（直接执行二进制；本机 ctest 自身在 deb 前缀 `LD_LIBRARY_PATH` 下会命中
libexpat 符号冲突，故本地以二进制为 oracle，CI 以 ctest 为准）：

```
QT_PLUGIN_PATH=<prefix>/usr/lib/x86_64-linux-gnu/qt6/plugins \
QT_QPA_PLATFORM=offscreen ./build/tst_h7_resultbudget
# 10 passed, 0 failed

QT_PLUGIN_PATH=... QT_QPA_PLATFORM=offscreen ./build/tst_h7_resultwire
# 7 passed, 0 failed
```

- `tst_h7_resultbudget`（10 例）：最终 envelope 配额内重组（quota 8192→1024→
  4096→2048→1280 循环）、请求上限与宿主配额交集不写进 cursor 身份、嵌套大
  成员同引用 child 下钻、Unicode/转义片段拼接与代理对、空容器/标量、最小
  配额显式失败且不自动抬高、到期读即时释放值（retainedResults/Bytes 归零、
  tombstone 仍区分 expired/unknown）、纯值配额公式。
- `tst_h7_resultwire`（7 例）：真实 controller + loopback 模型 + 临时工程/
  catalog：真实 `query_project` 产出 ref → 模型按实际收到的 ref/cursor 读页
  （根对象→child `/wells`→多页）→ 逐页重组 22 口井；每帧 role=tool content
  为合法 envelope 且 ≤ 实际配额；`entityPage` 计数 1 证明读页不重跑查询；
  同会话后续用户轮继续同一临时引用（第二轮回读仍不重跑查询）；受控 Pi peer
  走同一 `dispatchTurnTool` 路径并验证重复 `tool_request` 复用原 content（
  工具执行计数不增）；runner 级验证「请求 1024 / 宿主 4096」最终 envelope ≤
  1024 且 `transport.maxBytes=1024`，额度压小返回
  `tool_reply_budget_changed`+`retry_read_same_cursor`；同帧两个 query 调用
  各自携带配额、各自产出可读引用（entityPage=2，ref 互异）。

关键回归（同一构建、同一运行环境）：`tst_chatresults` 16/16、
`tst_aichatprojectquery` 16/16、`tst_g2_turncheckpoint` 12/12（含受控 Pi 链、
保存失败停队列、取消单次结算）。未运行：`tst_aichattoolloop`（需 ORT 执行面
整链）、`tst_pichatbridge`/`tst_g2_piterminal` 的真实 Pi 进程项（本机无
`PALEO_TEST_PI_NATIVE_ROOT`）；H7 未改 Pi bridge/ORT 执行面。

Source gates（本地，全部通过）：`check_layering.py --strict`、`check_i18n.py`、
`check_ts_stale.py`（0 missing）、`check_file_budget.py --strict`（新增 0）、
`check_docs_drift.py`、`check_qgis_spread.py`（169，与 baseline 一致）、
`check_ui_invariants.py`、`check_ui_tokens.py`、`check_l10n.py`、
`cmake -P cmake/verify-cpp-mainline.cmake`。

## 独立复审与修复

独立 reviewer（未参与实现）逐 hunk 审查了调用链并给出 5 条问题，已全部修复：

1. **P1 有效子预算未回传**：`read` 取 min(请求, 宿主) 选页，但 runner 记录的
   是宿主配额，最终化会用更大值重包（`max_bytes=768` 的请求可被包成 769 字节，
   `transport.maxBytes` 也谎报）。修复：`ChatResultStore::lastReadEnvelopeBytes()`
   暴露实际生效值，runner 记录它，`modelResult` 按它复核/编码；新增
   `runnerFinalisationUsesEffectiveSubBudget` 覆盖。
2. **P1b 漂移分支绕过 runner 清理且把「回升」当漂移**：删除 controller 侧
   `m_toolCallQuotas` 与 preempt 分支，收敛到 `modelResult` 单点判定
   （`maxBytes < 生效值`），槽位在同一处清理；`deliveredOk` 由返回 envelope
   的 transport code 推导，native/Pi/UI 同源。
3. **P2 读预算失败带误导性 `retryAdvice`**：`boundedResultError` 增加 advice
   参数，读页预算失败用 `retry_read_same_cursor`（只读重试，不重跑业务）。
4. **P2 宿主配额小于 768 被抬高**：改为只向下取 min，不再 `qBound` 上抬。
5. **P3 过期分支死代码**：`expire()` 之后的二次到期判断删除，tombstone 仍由
   `expire()` 按 handles 上限裁剪。

第二轮复审（针对修复增量）另指出三点已收口：

- `lastReadEnvelopeBytes()` 在 selector/cursor/page 预算被拒时仍会残留该次
  尝试的预算 → 现在非 ok 返回前置 0；测试断言被拒读取后为 0。
- `onToolFinished` 的两个早退（`pending<=0` / 调用 id 不匹配）会绕过
  `modelResult` 的槽位清理 → 新增 `discardDeliveredReadResult(callId)` 并在
  两处按 id 精确清理。
- 多调用测试的配额独立性 oracle 偏弱 → 改为直接读取两个工具 envelope 的
  `transport.maxBytes`（即各自派发时冻结的配额），断言两者不同且都在
  [768, 8192]；另补「失败后同 id 再最终化走普通编码」证明槽位已清。

复审指出但按边界保留的事项：`run(call)` 旧入口 + `modelResult` 现在也会记录
并按请求预算复核（不再有静默降级窗口）；保存失败/取消窗口沿用 G2 既有夹具
（H7 未改该路径）；`encode` 对 <768 的 maxBytes 仍按协议下限报告（controller
的 `replyQuotaBytes` 保证 ≥768，生产不可达）。

## 剩余限制

- 估算器仍是字符近似，不宣称精确 tokenizer；配额只保证 envelope 字节有界。
- 单用户轮的 token 预算仍是既有 6000/384；大结果需要跨用户轮继续（第二轮回读
  测试已覆盖），H7 不放宽该预算。
- 未把临时引用升级为持久成果，未新增跨会话恢复/检索语言/浏览 UI。
- 本机未运行 Windows 车道；Pi 真实进程验证需要 `PALEO_TEST_PI_NATIVE_ROOT`，
  本次以受控 peer 覆盖同一生产桥路径（真实进程未运行）。
