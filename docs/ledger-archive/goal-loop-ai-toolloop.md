# Ledger — 方向 61：AI 工具调用闭环（goal/ai-toolloop-20261007）

基线：origin/master `f130fb2`。worktree `.worktrees/ai-toolloop`，构建绿（R0）。

## 设计定案（勘察后钉死，勿再摇摆）

- **批1 协议**：`LlmClient::send(messages, tools = {})` 可选参；tools 非空时
  请求体带 `tools[]`（`AiToolSpec::toChatFunction()` + `type:"function"` 包壳）
  与 `tool_choice:"auto"`（口径：模型自决，含不调工具直接作答；不强制）。
  三工具恒上送（契约稳定；无 ORT 构建由执行面诚实报 Disabled，不在请求面
  隐瞒——Oracle 5 的 Disabled 状态卡才有触发路径）。
- **批2 执行器**：新 `src/workflow/aichattoolrunner.{h,cpp}`。绑定
  `AiAssistWorkflow*`（tile/层位执行面）+ 可选 `WellFaciesService*`（自配
  config，测试注假端点）+ `AiChatToolContext`（horizon / gridFetch /
  traceFetch / faciesInput 提供者，由装配根或测试注入）。
  - tile：走 `startClassification`（PaleoTaskService 异步 + 协作取消）；
    AiAssistWorkflow 增 `lastProductStats()`（类直方图/无数据数/均值置信/
    tilesDone/inferenceMs/layerIds——writeTileProducts 处顺手算，不回读栅格）。
  - 层位：AiAssistWorkflow 增 `startSuggestion(...)`（同构：池线程跑纯引擎
    `suggestHorizonTracking`，主线程收尾入裁决队列）；结果摘要 JSON +「建议
    已入待裁决队列，不自动落库」。
  - 测井相：runner 直连 WellFaciesService（completed/failed 信号），摘要
    JSON 取 intervals 统计。app 装配暂不绑曲线提供者（需井缓存上下文，
  递延）——未绑时每调用如实报错回灌（role=tool error），不冒充成功。
  - domaintools 分发态更新：ORT 构建下 tile/层位 Routed（闭环已接线）；
    facies Routed + note（须端点与井数据上下文）。既有 tst_aichat 断言
    随语义更新（「诚实说明状态」语义不变，状态本身升级）。
- **批3 闭环**：controller 持 runner（组合）。finished 且末条 assistant 带
  toolCalls → 工具相（streaming 保持 true，状态「正在执行领域工具…」）→
  逐帧 run → 每结果追加 role=Tool 消息（toolCallId）→ 全部到齐后新
  assistant 占位 + 带 tools 再发。工具轮上限 kMaxToolRounds=5（防乒乓，
  超限诚实报错）。**取消语义（定案）＝作废**：cancel() 先 runner.cancel()
  （协作取消 tile/层位任务→不写产品；中止 facies HTTP），未应答的
  tool_call_id 合成 role=tool 取消回复（否则下轮请求违反协议——assistant
  带 calls 必须每个 id 有应答），循环停止。
- **批4 预算**：历史按「轮」（User 起切）开窗：近 kMaxTurns=12 轮且字符
  预算 24000（口径：ASCII≈4字符/token、CJK≈1字符/token 的字符近似，非精确
  tokenizer，注释钉死）；system prompt 常驻首位；发生截断时插第二条 system
  「更早对话已按上下文预算截断」（请求体可见）。工具结果超 4000 字符截断
  + 如实后缀标记（原始长度）。
- **批5 UI**：卡片两态——toolCallDispatched 呈「执行中」（Neutral 胶囊），
  新增 controller 信号 toolResultReady(call, ok, summary) 更新同卡为
  Success/Error 胶囊 + 摘要行；transcript 里 role=Tool 消息渲染为弱化一行
  （不灌 JSON 进正文）。

## 取消/守卫语义

- runner 世代号 `m_generation`：cancel()/析构 ++；在途任务 QPointer 守卫；
  迟到结果一律丢弃不 emit（先例 seismictaskservice/llmclient）。
- controller 取消见批3；析构顺序 controller 先于 runner（组合成员序）。

## 轮账

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|---|------|------|------|--------|
| 0 | 勘察 + 基线构建 | （构建声明有误，见轮1 更正） | — | 批1 |
| 1 | 批1-5 全部落码 | 首次 cmake 调用实际失败（PATH 无 cmake/cl，管道吞错报假绿）；定位工具链（cmake=C:/Qt/Tools/CMake_64，ninja=vcpkg tools，MSVC=VS2022 14.38 vcvars64） | 环境就绪 | 真编 |
| 2 | 修 aiassistworkflow.h 缺 QJsonObject include；接 ORT junction（主仓无 vendor/onnxruntime，真身在 .worktrees/ai-assist/vendor，mklink /J 指过去） | 构建全绿（ORT=ON） | 通过 | 跑测试 |
| 3 | — | 测试 exe 0xC0000135/0xC0000139：三重环境坑逐一定位（见下） | 定位完成 | 绕道跑测试 |
| 4 | 修 tst 两处期望错（异步送达、开窗计数）+ 多调用应答序改集合比较 | tst_aichattoolloop 10/10 | 通过 | 家族回归 |
| 5 | review 轮一修复：cancel 合成应答 JSON 拼接改 QJsonDocument（模型可控 name 转义）；aiwiring 缩进噪音还原 | 重建绿 + 家族 6/6 + 三静态门绿 | 通过 | 全量对照 |
| 6 | ledger/提交 | 全量 ctest 后台跑（清干净构建后重启，防重建污染） | 进行中 | R0 对照 |

### 轮3 环境坑清单（Windows 本机，后续方向直接抄）

1. **PATH 无工具链**：bash 里 cmake/ctest 不在 PATH；`| tail`/`| grep` 会吞
   "command not found" 报假绿——验证命令必须检查真实退出码（`${PIPESTATUS[0]}`）。
2. **System32 假 ORT**：`C:\Windows\System32\onnxruntime.dll`（1.17.2，2026-09-09
   出现）优先于一切 PATH——标准 DLL 搜索序里 System32 赢。解法：把 vendored
   onnxruntime.dll 拷进 `build/`（exe 目录第一优先）。
3. **Qt 双版本错配**：qgis_*.dll 按 **Qt 6.11.2**（paleo-qgis-deps/Library/bin
   自带整套 Qt）构建；PATH 若把 C:/deps/Qt/6.8.0 放前面 → qgis 缺导出 →
   0xC0000139。运行 PATH 必须 qgis-deps 在 Qt 目录**前**（6.8 编的代码跑 6.11
   运行库，Qt 次版本向前兼容）。
4. **宿主 Temp 写入被拒（今日回归，宿主红项）**：QGIS 初始化后的进程在
   QTest 执行段对 `%TEMP%` 与盘根 mkdir/写文件一律 ERROR_ACCESS_DENIED
   （main() 阶段同路径成功——用 _wmkdir/QTemporaryDir 双向探针证实）。今晨
   （08:44-09:22）方向 51 的同二进制还是绿的；现在方向 51 自己的 worktree +
   ctest 也复现（tst_projectsvc/tst_aiassistworkflow 全部 `dir.isValid()` 红）。
   与本方向代码无关（未触碰的测试同样红）。**绕道**：`TMP/TEMP` 指到
   worktree 内目录（如 `<worktree>/tl-tmp`）即全部恢复——项目树路径不受影响。
   根因（AV/minifilter/ACL）未深究，方向内不修宿主。
5. 批处理必须 CRLF + 单行（LF/`^` 续行会让 cmd 解析炸）；printf 写批处理会被
   `\U`/`\2` 转义坑，用 python 写。

### 测试证据

- `tst_aichattoolloop`：**10/10**（两轮 + review 轮二后第三轮，全绿）。
  覆盖 Oracle 1/2（tools[] schema×3 + tool_choice=auto + 真跑 tile 分类 →
  role=tool 回灌对账 → 终答）、3（取消：无第二条请求、无悬垂、合成应答保
  协议、产品零写入）、4（历史开窗 + 截断标记 + system 常驻；工具结果截断
  标记）、5（未配置 facies → 错误回灌 + 失败卡片 + 终答）、往返上限、
  忙态拒绝、多工具帧全部应答（集合比较——应答序协议不要求）。
- AI/ORT 家族 12/12 ×2：tst_aichat / tst_aichatcontroller / tst_aiassistdock /
  tst_aiwiring / tst_aiassistworkflow / tst_aichattoolloop / tst_onnx /
  tst_onnxsessions / tst_tileinference / tst_modelregistry /
  tst_remotepredictrouter / tst_onnxworkflow。
- 静态门：layering --strict / i18n / ui_invariants --strict 全绿（终态复跑）。
- 全量 ctest（-LE perf，串行，清干净构建后跑完）：**305 项 = 262 过 / 43 红**。
  红集合逐项在 R0（.worktrees/highdpi = master f130fb2 同树、同机、同环境
  口径）复跑：**43/43 同红，diff 为空**——本方向零新增失败。构成：
  - 34 × 0xC0000139：宿主 Qt 双版本错配（qgis_deps 6.11 vs C:/deps 6.8 的
    PATH 取舍必伤一面；方向 51 今晨记录的同类宿主红项，本次为救 qgis 面
    取 6.11 优先，纯 Qt 重面转红——两种次序的红集合在 R0 上同样成立）。
  - 9 × 断言红：tst_runtime（paleo-qgis-prefix 缺 resources/srs.db）、
    boot / tst_metastore / tst_pythonenv / tst_seismic_{core,engine,transcode,3dui}
    / tst_wellsection_ui（宿主 Temp/资源面今日回归的同族；R0 同红）。
  注意：中途一次全量 run 因与 review 重建并发而作废（方向 51 补记的同款坑），
  最终结论只采信干净重启后的这一份。

### review 两轮（五维）结论

- 轮一：发现 cancel 合成应答的 JSON 手拼对模型可控 name 不设防（改
  QJsonDocument 序列化）；aiwiring 无关缩进噪音还原。其余四维
  （分层/协议/守卫/安全/i18n）无 High/Medium。
- 轮二：修复后重建 + 家族复跑 6/6 + 静态门绿。零 High/Medium。
- Low（记 PR body）：① runner 执行序 = 到达序（QHash 迭代），协议允许，
  如需确定性可按 tool_calls 数组序排队；② lastProductStats 是 workflow 单例
  摘要面，靠 runner 串行保一致（已注释钉死）；③ tst_aiassistdock 的
  FakeLlmServer 新增 ToolCallsOnce 模式（工具一轮+文字一轮）；
  ④ requestMessages 对「最新一轮永远保留」即使单轮超预算——极端长问题
  可能超模型上下文（截断标记只对更早轮生效）。


## 环境口径（Windows 本机，复现本方向验证用）

```bash
cd C:/Users/wangj.KEVIN/projects/paleo_workstation/.worktrees/ai-toolloop
mkdir -p tl-tmp
export PATH="/c/Users/wangj.KEVIN/paleo-qgis-deps/Library/bin:\
/c/Users/wangj.KEVIN/paleo-qgis-prefix/bin:\
/c/deps/Qt/6.8.0/msvc2022_64/bin:\
/c/Users/wangj.KEVIN/projects/paleo_workstation/.worktrees/ai-toolloop/vendor/onnxruntime/lib:$PATH"
export QT_QPA_PLATFORM=offscreen
export TMP='C:\Users\wangj.KEVIN\projects\paleo_workstation\.worktrees\ai-toolloop\tl-tmp'
export TEMP="$TMP"
C:/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build -R <name>   # Windows 串行
```

注意：`build/onnxruntime.dll`（vendored 1.19 拷贝，压 System32 的 1.17）与
`vendor/onnxruntime`（junction → .worktrees/ai-assist）都是 gitignored 本机
接线，换机器需按 BUILDING.md「独立 worktree 开发」重接。
