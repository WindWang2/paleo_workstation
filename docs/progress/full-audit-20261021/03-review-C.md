# 审查报告 C（src/workflow / src/ai / src/linkage）

只读深审，BASE e6837e14（工作树 paleo_workstation-full-review-20261021）。
范围与方法同 02-agent-briefing.md；所有发现均给出真实执行路径证据，
并与已知库（02a 的 147 项 + 02-excluded-files 活跃分支 + 上轮已修/登记项 +
开放 Issues）逐条比对。

## 发现总览

| ID | 严重度 | 置信 | 一句话 |
|----|--------|------|--------|
| C-1 | P2 | VERIFIED | AI 助手「重试」重发请求时漏带 tools[]，重试轮永久失去工具调用能力 |
| C-2 | P2 | VERIFIED(机制)/HIGH(触发) | 后台一致性报告写在 /tmp 再 rename 进工程目录，跨文件系统必失败 |
| C-3 | P2 | VERIFIED(机制)/HIGH(触发) | 导出发布以 mutationSeq 判陈旧：任一无关 catalog 写入即整批丢弃长导出 |
| C-4 | P2 | VERIFIED | 地震反演临时体（可达数百 MB）从不清理，commitExternal 拷贝后源文件永留 /tmp |
| C-5 | P3 | VERIFIED | 连井剖面岩性「查询路径」写 catalog（无 no-op 去重），反复抬 mutationSeq + 刷 changed |
| C-6 | P3 | SUSPECTED | LlmClient 非流式应答丢弃 finished 时刻读出的尾部字节，可能截断 JSON 误报 Malformed |

---

## C-1 AI 助手「重试」重发请求漏带 tools[]，重试轮静默失去领域工具

- **ID/Severity/Confidence**: C-1 / P2 / VERIFIED
- **Module**: src/workflow（AI 对话编排）
- **File:line / 函数**: `src/workflow/aichatcontroller.cpp:325-356` `AiChatController::retryLast()`；对照 `sendRound()` `aichatcontroller.cpp:445-452`
- **根因**: `retryLast()` 手工复制了 `sendRound()` 的「占位 + 上送」序列，但调用
  `m_client.send(requestMessages())` 时漏了第二实参 `tools()`（`sendRound()` 是
  `m_client.send(requestMessages(), tools())`）。`LlmClient::send` 的 tools 形参缺省
  为空表（`src/ai/chat/llmclient.h:124-125`），空表时请求体不上送 `tools[]`/
  `tool_choice`（llmclient.cpp:336-346）。
- **触发条件**: 用户在 AI 助手里点「重发」（`src/ui/ai/aiassistdock.cpp:167`
  `m_controller->retryLast()`，按钮在非流式且有用户消息时可用 ：279）。
- **Observed vs Expected**:
  - Observed：重试轮发出的 chat completions 请求体不含 `tools`——模型无法调用
    paleo.query_project / paleo.well_facies_prediction 等领域工具，只能凭历史
    （可能含旧 role=tool 结果）直接作答；需要查工程数据的问题在重试后得到
    无工具支撑的回答，且无任何提示。
  - Expected：注释自述「与 sendUserText 同口径重置工具往返状态：否则上一轮
    打满 kMaxToolRounds 后重试，本轮首个工具请求即超限——重试轮被永久禁用
    工具」（aichatcontroller.cpp:348-350）——设计意图明确是让重试轮**恢复**
    工具能力；漏传 `tools()` 使该意图落空，m_toolRound 重置成了空转。
- **影响面（调用链证据）**: aiassistdock.cpp:167（UI 入口）→ retryLast() →
  `LlmClient::send(messages)`（无 tools）→ issueRequest 组包（llmclient.cpp:336
  起不看 tools 即不上送）。现有测试 `retryLastResendsWithoutDuplicatingUserMessage`
  （tests/tst_aichatcontroller.cpp:414-433）只断言请求数与消息数，
  FakeLlmServer 未校验请求体是否带 tools——该缺陷零覆盖。
- **排除比对**: 已知 147 项中 B.14 是 llmclient.cpp:459 stream=true 收到非 SSE
  应答按空成功收尾（同文件不同根因）；C.15 是 appcontext 关工程不重绑 AI 工具
  上下文；L4.2 是 aiassistworkflow 半提交不回滚。本条是控制器重发路径的参数
  缺失，与上述均不同根因。上轮（20261020）清单无此条。非排除。
- **建议修复**: retryLast 尾部两行改为 `sendRound()`（或补 `tools()` 实参），
  同时消掉与 sendRound 的重复代码。
- **回归测试思路**: FakeLlmServer 断言第二次请求体含 `tools` 数组且条数等于
  `builtinAiToolSpecs().size()`；再加一例「首轮工具往返成功后重试，模型再次
  请求工具可被路由」。

## C-2 后台一致性报告 /tmp → 工程目录 rename 跨文件系统必失败

- **ID/Severity/Confidence**: C-2 / P2 / VERIFIED(机制) · HIGH(整体，触发依赖 /tmp 与工程目录不同文件系统)
- **Module**: src/workflow（mappingworkbench_consistency）
- **File:line / 函数**: `src/workflow/mappingworkbench_consistency.cpp:230-285`
  `MappingWorkbench::consistencyImpl(..., background=true)`
- **根因**: 后台路径把报告 JSON 写进**默认临时目录**的 QTemporaryDir
  （`std::make_shared<QTemporaryDir>()`，:232 → `QDir::tempPath()`），任务成功后
  用 `QFile::rename(tempPath, path)`（:278）把它从 /tmp 搬进
  `<projectDir>/artifacts/reports/facies-consistency-<uuid>.json`。
  POSIX rename(2) 在 src/dst 跨文件系统时返回 EXDEV；/tmp 为独立 tmpfs 是
  Arch/systemd 系默认布局（本机即 CachyOS），工程目录通常在 /home 分区——
  rename 恒失败。
- **触发条件**: 工程与 /tmp 不同文件系统时，任何一次「测井相 × 地震相一致性
  比较（后台任务版）」。
- **Observed vs Expected**:
  - Observed: rename 失败 → `task->failPublication("一致性报告发布失败")` +
    `emit errorOccurred`（:278-281）——整个后台一致性功能在常见 Linux 布局上
    100% 无法产出报告（计算本身已完成并被丢弃，report 仍留在 /tmp 随
    QTemporaryDir 析构删除）。
  - Expected: 报告发布成功。仓库自己的惯例恰是「跨不了设备的搬运用 copy 或
    同目录暂存」：mock 预测路径特意把 QTemporaryDir 建在**工程内** staging
    （mappingworkbench_predict.cpp:316-319）；horizonbatchexport 的交付目录也
    建在目标父目录里再 rename（horizonbatchexport.cpp:430）；commitExternal
    对外部产物用 QFile::copy（derivedassets.cpp）。本处是唯一违反该惯例的
    rename。
- **影响面（调用链证据）**: paleomainwindow_workbench_commands.cpp:301
  「consistency」命令 → `startFaciesConsistency` → `consistencyImpl(bg=true)`
  → m_taskSvc->start（worker 写 /tmp）→ finished 处理器 rename 进工程。
  同步路径（faciesConsistency）直接 QSaveFile 写工程目录，不受影响——即
  同一功能同步/后台两条路行为分裂。
- **排除比对**: 已知库无此条（D-09 layoutexportactions 双上报、A-12 mapversionstore
  冻结目录均不同根因）。上轮清单无。非排除。
- **建议修复**: QTemporaryDir 建在 `<projectDir>/artifacts/reports/.consistency-XXXXXX`
  （同文件系统 rename 原子且零拷贝）；或退而用 QFile::copy + 校验 + remove。
- **回归测试思路**: 单测里把 QStandardPaths 临时目录指到独立挂载不便——建议
  直接测「暂存目录与目标目录同前缀」（构造参数化）；集成环境可用
  `TMPDIR=<工程内目录>` 与 `TMPDIR=/dev/shm` 各跑一例对比。

## C-3 导出发布闸门以 mutationSeq 判陈旧：窗口期内任一无关 catalog 写入即整批丢弃

- **ID/Severity/Confidence**: C-3 / P2 / VERIFIED(机制) · HIGH(整体，触发需导出窗口内有 catalog 写)
- **Module**: src/workflow（mapexport / horizonbatchexport / mapbookqueue）
- **File:line / 函数**（三处同型）:
  - `src/workflow/mapexport.cpp:287-290` `startMapProductExport` 的 finished 处理器：
    `stale = task->session() != tasks->session() || !owner || owner->projectDir() != directory || owner->mutationSeq() != sequence;`
    → `task->discardPublication()` + 成果作废（:291-309）。
  - `src/workflow/horizonbatchexport.cpp:527-534`（`startAsync`，sequence 取自
    :404）→ 整批 `outcome.ok=false`、error=「工程数据已变化，出图成果未登记，请重新导出」（:556-572）。
  - `src/workflow/mapbookqueue.cpp:282-287`（`Exporter::start`，sequence :226）→
    同文案，`succeeded` 全数折进 `failed`、manifest 重写（:290-311）。
- **根因**: 三条导出链都把「任务开始时的 live catalog mutationSeq」当作发布前置
  条件。但导出计算全程只写 **staging 副本**（createStagingCopy + journal），live
  catalog 在窗口期内的**任何**写入——包括与本导出毫无关系的：另一个导出的
  journal 发布、一次预测 record() 落版本、一次导入提交、甚至 C-5 记录的岩屑
  落选标注——都会 ++mutationSeq（datacatalog_versions.cpp:310/486 等，id 分配也
  推进），于是导出完成即被判「工程数据已变化」而整批丢弃。**两个重叠导出必然
  触发**：先发布者把 seq 抬到 S+1，后完成者必 stale。
- **项目内部反证（同一团队已认定该模式为缺陷）**:
  `src/workflow/constraintfactorjobs_factor.cpp:1141-1146`（runFactorWork）注释：
  「#480 P3-3：不再捕获 catalog mutationSeq——作业输入在提交时已冻结（约束快照
  + 输入版本不可变），排队/计算期间任意无关 catalog 写入（导入、注册井、出版
  本…）此前都会在收尾闸门**误取消长作业**。真失效由代次/目录/catalog 指针/
  session 与各作业自己的输入指纹校验把关。」——单因素链已删除该判据，三条
  导出链仍保留。
- **触发条件（现实路径）**: ① 用户先点「按层位批量出图」（分钟级）再做任一
  导入/预测/单图导出；② mapbook（N 版渲染）期间 mock 地震预测完成落版本
  （finishPrediction → record() → addVersion）；③ 同时开两个不同名导出
  （startMapProductExport 仅按同名 title 去重，mapexport.cpp:232-237）。
- **Observed vs Expected**:
  - Observed: 导出实际成功渲染并登记进 staging，收尾时因 seq 不匹配被
    discardPublication，用户看到「工程数据已变化，出图成果未登记，请重新导出」；
    mapbook 场景用户输出目录里已写好的 pages 文件留在盘上、manifest 却全标
    failed（产物/清单不一致的另一面）。
  - Expected: journal-apply 本身就是冲突权威闸（applyJournal 按原序重放 mutator，
    datacatalog.h:96），无关写入不应整体作废已完成的工作；至少应区分
    「真冲突（applyJournal 失败）」与「仅 seq 前进」。
- **影响面（调用链证据）**: 生产可达面：`PaleoHorizonBatchExport::startAsync`
  ← paleomainwindow_attach_compose.cpp:444（按层位批量出图）；
  `PaleoMapBookQueue::Exporter::start` ← ui/layout/mapbookcontroller.cpp:104
  （地图册批量导出）；`startMapFrameExport` ← paleomainwindow_attach_mapping.cpp:367
  （演化帧导出）。`startMapLayoutExport` 当前生产无调用（仅 mapexport.cpp 内
  定义），风险面在前三者。
- **排除比对**: 已知 147 项 D-09（上轮登记未修）是 layoutexportactions 的
  「双上报」——回调重复通知，非发布闸门误判；#480 P3-3 的修复只落在
  runFactorWork。本条三处导出链的 mutationSeq 误陈旧未被任何已知项覆盖。
  mapexport.cpp / horizonbatchexport.cpp 虽在 02-excluded-files（活跃分支触碰），
  但按简报规则「清单内文件仍可审查，仅同根因于已知项须排除」——本条非已知
  项同根因，成立。mapbookqueue.cpp 不在排除清单。
- **建议修复**: 与 #480 P3-3 对齐——去掉 `mutationSeq != sequence` 判据（保留
  session/projectDir/catalog 指针守卫），冲突交由 publishMapExportJournal →
  applyJournal 的失败路径如实报错；或把 seq 差异降级为「重放校验」而非整批丢弃。
- **回归测试思路**: 测试里起一个长 produce（sleep 的 MapProducer），期间对 live
  catalog 做一次无关 addVersion，断言导出仍发布成功；再加双导出重叠用例。

## C-4 地震反演临时体从不回收：commitExternal 拷贝后源文件永留系统 /tmp

- **ID/Severity/Confidence**: C-4 / P2 / VERIFIED
- **Module**: src/workflow（inversionworkflow）+ 编排侧 ui/seismicsection
- **File:line / 函数**: `src/workflow/inversionworkflow.cpp:672-683`
  `InversionWorkflow::computeInversionJob()` 尾段；发布
  `publishInversionJob()` :691-766；编排
  `src/ui/seismicsection/seismicsectiondock_inversion.cpp:199-279`
- **根因**: compute 成功路径把整幅阻抗体写成
  `<QDir::temp()>/paleo-inv-<uuid>/impedance.iimp`（体尺寸 = nIl×nXl×nS 个
  float，几百 MB 量级常态），路径记进 `job->tempVolumePath`。发布走
  `registrar.commitExternal(staging, job->tempVolumePath, ...)`，而
  `DerivedAssetRegistrar::commitExternal`（src/workflow/derivedassets.cpp:244 起）
  是 **QFile::copy 进受管目录**（源路径 ≠ 暂存位时先 remove 目标再 copy），
  不删源。全仓 grep `paleo-inv-`/`tempVolumePath`：除 inversionworkflow 自身与
  测试直读外，唯一消费点是 dock 报错文案里 `arg(job->tempVolumePath)`
  （seismicsectiondock_inversion.cpp:249）——没有任何 cleanup 钩子或清扫器。
- **触发条件**: 每次地震反演（成功、发布失败、写盘中途失败、取消晚于落盘
  任意一种结局）各留一份完整/半份体文件 + 空目录在系统 /tmp，直到重启或
  手动清。
- **Observed vs Expected**:
  - Observed: /tmp 按反演次数累积数百 MB 级 `paleo-inv-*` 树；失败/取消路径
    同样不回收。
  - Expected: 与 geostat 的 `paleo-sf-`/`paleo-gs-` 前缀清理、导出链的
    `.map-export-`/`.horizon-export-` 入口清扫（horizonbatchexport.cpp:419-422
    `sweepStaleStagingRoots`）同口径——受管发布成功即回收暂存，失败/取消由
    cleanup 钩子回收。地震反演是三段式作业族里唯一没有这套纪律的成员。
- **影响面（调用链证据）**: seismicsectiondock_inversion.cpp:202-222
  （m_taskService->startBounded 内 compute）→ :254 publishInversionJob →
  commitExternal 拷贝；finished 处理器任何分支都未触碰
  job->tempVolumePath。tests/tst_inversion_workflow.cpp:391-392、
  tst_inversion_realarea.cpp:84-92 直读 tempVolumePath，佐证该文件在发布后
  仍被预期存在（无人删）。
- **排除比对**: 已知 B.10（constraintfactorjobs_geostat.cpp:1541）是「清理钩子
  存在但判错前缀 paleo-sf- vs paleo-gs-」——本条根因不同：地震反演侧**根本
  没有**清理钩子（无判前缀问题可谈）。同为「临时树滞留」问题域但机制与修复
  点均独立，故不归并为 B.10 同根因。其余已知项无涉。非排除。
- **建议修复**: dock 的 finished 处理器终态各分支补
  `QDir(QFileInfo(job->tempVolumePath).absoluteDir()).removeRecursively()`；
  或把临时目录挪进工程 staging（同 C-2 惯例）并接 `paleo::exporting::
  sweepStaleStagingRoots` 风格的入口清扫。
- **回归测试思路**: 测试跑一次 compute+publish 后断言 tempVolumePath 不存在；
  取消/发布失败用例同断言。

## C-5 连井剖面岩性装配（查询路径）写 catalog：无 no-op 去重，反复抬 mutationSeq 并刷 changed

- **ID/Severity/Confidence**: C-5 / P3 / VERIFIED
- **Module**: src/workflow（wellsectionworkflow）× src/catalog（updateAssetExtra map 重载）
- **File:line / 函数**: `src/workflow/wellsectionworkflow.cpp:834-856`
  `CatalogWellLithologyProvider::cuttingsLithologyFor()`；
  写入落点 `src/catalog/datacatalog_versions.cpp:376-414`
  `DataCatalog::updateVersionExtra(versionId, QVariantMap, ...)`（map 重载）
- **根因**: 岩性「落选点名」逻辑在**取数/装配路径**里对每个落选岩屑资产调
  `m_catalog->updateAssetExtra(unselected.assetId, extraPatch)`（:845-854，
  返回值与 error 均丢弃）。该调用走 map 重载 `updateVersionExtra`，而 map 重载
  **没有**单键重载的「真 no-op：同值重写不动 catalog」短路
  （对照 datacatalog_versions.cpp 单键重载 :350-357 的 sameValue 判定）——
  同一 patch 每次都走 save → commitStore → `++m_mutationSeq` +
  `emit changed()`。
- **触发条件**: 任何一口井链接 ≥2 份岩屑录井资产时，每次连井剖面
  request()/lithologiesFor() 刷新都重写一次（内容不变）。
- **Observed vs Expected**:
  - Observed: ① 查询路径产生持久写副作用，违背「数据只问答不管谁来问」的
    分层语义（AGENTS.md 层契约导语）；② 每次刷新 ++mutationSeq——正是 C-3
    三处导出闸门的陈旧判据输入：**打开/刷新一次连井剖面就能把在飞的批量
    出图/地图册整批判废**；③ 每次 `catalog.changed` → MappingWorkbench 的
    changed 处理器（mappingworkbench.cpp:240-256）排队 synchronizeCatalogLayers
    + emit changed()——纯 UI 刷新churn；④ 只读工程（m_lockedReadOnly）下写入
    被拒，返回值被丢，静默无痕（标注缺失无人知晓）。
  - Expected: 落选标注应一次性写入（或由显式用户动作触发）；map 重载应与
    单键重载共享 no-op 短路；查询路径零写副作用。
- **影响面（调用链证据）**: `WellSectionWorkflow::request()` :332-334
  attachLithoSegments → provider.lithologyFor :679-698 → cuttingsLithologyFor
  :833-858 → updateAssetExtra；`lithologiesFor` :916-924 同路。multi-cuttings
  告警分支即写分支（candidates.size() > 1，:834）。
- **排除比对**: 已知 C.2（catalogindex verifyAgainst 吞分歧）是索引健康度
  语义，不同根因；A-13（负 ord）、#556（吞错路径）均不覆盖「查询路径写 +
  无 no-op 去重」。上轮清单无。非排除。
- **建议修复**: ① map 重载补 sameValue 短路（与单键对齐，一并消除其他调用
  方的重复写）；② 落选标注挪到导入/链接变更时一次写入，或至少结果与
  currentVersion.extra 比对后再写。
- **回归测试思路**: 构造两份 cuttings 资产 + 一次 lithologiesFor，断言
  catalogRevision/mutationSeq 只前进一次（第二次调用不再前进）、只读工程下
  有如实告警。

## C-6 LlmClient 非流式应答未并入 finished 时刻读出的尾部字节（SUSPECTED）

- **ID/Severity/Confidence**: C-6 / P3 / SUSPECTED
- **Module**: src/ai/chat
- **File:line / 函数**: `src/ai/chat/llmclient.cpp:416-479` finished 处理器；
  `:427` `const QByteArray tail = reply->readAll();` 的 tail 仅在 stream 分支
  （:461-471）被消费，非流式分支 `:478 handleNonStreamReply(m_body)` 只用
  readyRead 累积的 m_body。
- **根因（假设）**: Qt 语义上 finished 前数据都会经 readyRead 送达，但 Qt 文档
  同时建议在 finished 里读取剩余缓冲；若平台/管线在最后一个 readyRead 与
  finished 之间留有未投递字节，非流式应答尾部缺失 → JSON 解析失败 → 误报
  「应答不是合法 JSON」（Malformed）。流式分支自己都做了 tail 兜底
  （:460-471 注释「[DONE] 缺失…兜底 flush」），非流式分支没有对称处理。
- **验证思路**: 用本地假服务在响应末尾分片发送并控制事件循环时序难以稳定
  复现；更直接的是代码论证——若成立，修法仅一行：非流式分支先
  `m_body.append(tail)` 再 parse。鉴于不确定 Qt 是否会在 finished 前保证
  readyRead 全量送达，标 SUSPECTED；建议有环境者以 QNetworkReply 源码或
  抓包实验定夺。
- **排除比对**: 已知 B.14 是 stream=true 收到非 SSE 应答被当空成功——不同
  分支不同根因。上轮清单无。非排除（若验证不成立则撤）。

---

## 与已知库比对后主动排除的观察（不计入发现）

1. **`AiAssistWorkflow::commitAccepted`（aiassistworkflow.cpp:774-780）裸 QFile
   写 JSON 不校验 write 返回值即 commitExternal**——与已知 B.6（wellcompositexml
   :900-912）/B.7（seismictaskservice_interp CSV）同根因（写不校验 + 截断字节
   入库），按纪律标 EXCLUDED_ACTIVE_WORK，不计新发现。
2. **`generateSurferIdwFactor` 的 runFactorWork compute 捕裸 `proc =
   m_proc.data()`（constraintfactorjobs_factor.cpp:1034）**——#379 在
   startConstraintJob 统一面已修（QPointer），生产侧该点未跟进；但
   QgisProcessingService 为 app 生命周期对象，现实不可达悬垂，弱于已知的
   D.24 家族且无新证据链，不单列。
3. **`WellFaciesConfig::load()` 内置明文公网端点 + API key（wellfaciesservice.cpp:77-82）**
   ——代码注释明示 2026-10-19 产品裁决（用户明示豁免），属已裁决事项非缺陷
   发现；仅提示：密钥入库后建议轮换通道。

## 覆盖清单

**逐行级精读（含调用方/被调用方交叉）**：
- workflow: mappingworkbench{.cpp,_compose,_predict,_polygonize,_consistency,_edit}.cpp、
  mappingworkbench.h、mapversioncontroller_async.cpp、constraintworkflow.cpp、
  constraintfactorjobs.cpp、constraintfactorjobs_factor.cpp（runFactorWork/
  runFactorAlgorithm/declareFactorResult/generateSurferIdwFactor 全文 + publish 主体）、
  constraintfactorjobs_geostat.cpp（publishGeostatJob + compute 主干）、
  aichatcontroller.cpp、aichattoolrunner.cpp、aiassistworkflow.cpp、
  wellsectionworkflow.cpp、inversionworkflow.cpp、depthconversionworkflow.cpp、
  mapexport.cpp、horizonbatchexport.cpp、mapbookqueue.cpp、importledger.cpp、
  projectlayerrefresh.cpp、folderimport.cpp、wellpredictionservice.cpp。
- ai: chat/llmclient.cpp、chat/chatsession.cpp、chat/chatmessage.cpp、
  modelregistry.cpp、wellfaciesservice.cpp、onnxlocalpredictor.cpp、
  tileinference.cpp（plan/softmax 段）。
- linkage: wellmaplink.cpp、seismicmaplink.cpp、selectioncontext.cpp、
  threewaylocator.cpp（全文四件）。
- 关键被调方（为验证发现而读）: services/jobrunner.h（全文）、
  services/paleotaskservice.cpp（TaskRunner/beginNewSession/applyFinish/commitStore）、
  workflow/derivedassets.cpp（commitExternal）、catalog/datacatalog_versions.cpp
  （updateVersionExtra 两重载/mutationSeq 递增点）、qgis/qgisprojectservice.cpp
  （project() 非空性）。

**接口级/择段读**：mappingworkbench_impact.cpp（executeRecompute/staleness 段）、
mappingworkflow.cpp（栅格读/凸包/IDW 段）、batchjobqueue.cpp（resume/pump/
startOnSlot 段）、workflows.h（ConstraintWorkflow 契约面）、constraintjobs.h、
ai/chat/llmclient.h、aichatcontroller.h、mapversioncontroller.h、
wellsectionworkflow.h、inversionworkflow.h、horizonbatchexport.h、
stratigraphicwebsession.cpp（头部/析构段）、vendor sbm SgyVolume.h（拷贝语义）。

**未覆盖/浅覆盖区域（如实声明）**：
- workflow 的 evolution/realization/datagraph/faciesclassify/wellfaciesworkflow/
  derivation/faultsurface/faultinterpretation/welltopseditor/pythonconsole/
  officepreview/xmlpreview/sectionworkbench/storagegovernance/projectareasummary/
  propertymodel/registration/wellsiting/surfacegridding/paleomap* 家族——只做了
  异步面 grep 与头文件 generation 守卫盘点，未逐行审（其中 propertymodel/
  registration/wellsiting/surfacegridding/paleomap* 属已知 B.8/B.9/C-10 排除域，
  sectionworkbench/wellsitingworkflow 等在 02-excluded-files 内由活跃分支处理）。
- ai/onnxpredictionservice.cpp（已知 C.13 域）、remotepredictrouter.cpp（上轮已修
  C-15 域）、horizonsuggest.cpp（仅扫取消位）。
- 未构建、未运行任何测试；全部结论基于静态读码 + 调用链论证。
