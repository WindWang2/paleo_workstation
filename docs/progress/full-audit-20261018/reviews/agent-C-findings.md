# Agent C 审查报告 — src/workflow / src/ai / src/linkage

- 基线：`366aef97`（worktree `paleo_workstation-full-review-20261018`，只读审查）
- 审查人：Agent C（全仓深度审计，2026-10-18）
- 输入：`01-conflict-matrix.md`、`99-external-issues-snapshot.txt`（44 个开放 issue 全文通读）、`/tmp/paleo-audit/issue-465.txt`
- 纪律声明：本报告只描述问题与修法建议，未修改任何源码/测试/构建文件；未运行 cmake/编译/ctest。文中「逐行读」均指实际完整阅读对应文件；未逐行阅读的文件在 §未覆盖区域如实列出。

---

## 1. 覆盖清单

### 1.1 深度精读（整文件或关键长函数逐行）

| 文件 | 行数 | 审查深度 |
|---|---|---|
| src/workflow/mapversioncontroller.h/.cpp | 101+528 | 逐行（版本保存/发布/残差摘要/stale advisory） |
| src/workflow/mapversioncontroller_async.cpp | 450 | 逐行（异步快照三段式、busy-token、stale 守卫） |
| src/workflow/mappingworkbench.h/.cpp | 404+1095 | 逐行（构造接线/bindCatalog/record 发布路径/styleLayer） |
| src/workflow/mappingworkbench_predict.cpp | 907 | 逐行（predict/cancel/finishPrediction/ensureInputVersion） |
| src/workflow/mappingworkbench_edit.cpp | 898 | 逐行（copyForEditing/saveEditingVersion/removeProduct/snapshotConstraints/publishConstraintImport/startConstraintImport；#465 与 PR #521 主题按排除矩阵处理，只记独立问题） |
| src/workflow/mappingworkbench_compose.cpp | 325 | 逐行 |
| src/workflow/mappingworkbench_polygonize.cpp | 290 | 逐行 |
| src/workflow/mappingworkbench_consistency.cpp | 321 | 逐行 |
| src/workflow/predictionworkflow.cpp | 501 | 逐行 |
| src/workflow/wellpredictionservice.cpp | 395 | 逐行 |
| src/workflow/batchjobqueue.cpp | 1121 | 逐行 |
| src/workflow/assetops.cpp | 504 | 逐行（rollbackToVersion/purgeAssets/purgeManagedFiles） |
| src/workflow/mappingworkflow.cpp | 984 | 前 500 行逐行 + `startThicknessChain` 全读（其余结构扫描） |
| src/workflow/compositionworkflow.cpp | 1008 | `fuseFactors` 全读（150-290），其余结构扫描 |
| src/workflow/constraintfactorjobs_factor.cpp | 1323 | `runFactorWork`/`runFactorAlgorithm`/`declareFactorResult` 全读（950-1270），其余扫描 |
| src/workflow/evidencerecord.cpp | 134 | 逐行 |
| src/workflow/projectlayerrefresh.cpp | 100 | 逐行 |
| src/ai/onnxpredictionservice.cpp | 766 | 逐行 |
| src/ai/onnxlocalpredictor.cpp | 84 | 逐行 |
| src/ai/remotepredictrouter.cpp | 403 | 逐行 |
| src/ai/wellfaciesservice.cpp | 329 | 逐行 |
| src/ai/modelregistry.cpp | 218 | 逐行 |
| src/ai/horizonsuggest.cpp | 221 | 逐行 |
| src/ai/wellfacieskeystore.cpp | 78 | 逐行 |
| src/ai/chat/llmclient.cpp | 545 | 逐行 |
| src/linkage/seismicmaplink.cpp | 267 | 逐行 |
| src/linkage/wellmaplink.cpp | 146 | 逐行 |
| src/linkage/selectioncontext.cpp | 155 | 逐行 |
| src/linkage/threewaylocator.cpp | 61 | 逐行 |

### 1.2 接口 + 调用链 / 针对性扫描（非逐行）

- mappingevidencereview.cpp（前 250 行 + 结构；PR #521 冲突领地）、mappingworkbench_impact.cpp、mappinghierarchy.cpp（PR #521 领地，未深审）
- constraintworkflow.cpp、constraintfactorjobs_{geostat,contours,interpretive,localdirection}.cpp、constraintwellfactors.cpp（grep 线程/异步模式 + 抽读）
- wellfaciesworkflow.cpp、aiassistworkflow.cpp（头部 + 连接模式）、aichatcontroller.cpp / aichattoolrunner.cpp（仅线程/调用模式 grep）
- officepreviewsession.cpp（异步验证路径精读片段）
- src/ai/chat/{chatsession,chatmessage,domaintools,llmkeystore,markdown}.cpp、onnxfixture.cpp、remotepredictconfig.cpp（扫描）
- 支撑事实核验（跨层追证）：src/services/jobrunner.h（逐行）、src/qgis/qgisprocessingservice.cpp `run()`（精读）、src/qgis/qgislayerservice.cpp `instantiate()`（精读）、src/catalog/datacatalog.cpp `BatchSave`/`createStagingCopy`/`checkWriteThread`（精读）、src/metadata/paleoprojectstore.cpp `enqueueWrite`（精读）、src/metadata/mapversionstore.cpp `snapshotInputForPublish`（精读）、src/app/appcontext.cpp 工程开/关接线（精读相关段）、src/services/paleotaskservice.cpp `beginNewSession`（精读）、src/algorithms/paleoalgorithms.cpp INPUT 参数消费方式（抽读）

### 1.3 只登记不审（排除矩阵 §4）

- 导出链：horizonbatchexport.cpp、mapexport.cpp、mapbook.cpp、mapbookqueue.cpp、layouttemplatelibrary.cpp（主仓 master 未提交导出簇同主题 → EXCLUDED_ACTIVE_WORK）。注：`dataexportworkflow.*`/`geologicalexport.*` 在本基线 worktree 中不存在（属主仓未提交新文件）。

---

## 2. Findings（新问题）

### FC-001 [P1] [VERIFIED] 智能预测异步作业把主线程活 QgsMapLayer 传入 worker 线程的 QGIS Processing —— 违反服务层自述成文契约

- **ID**: FC-001
- **Severity**: P1
- **Confidence**: VERIFIED（代码路径与契约文本双向核对）
- **Module**: src/workflow（PredictionWorkflow）× src/ui 接线
- **File:Line/Function**:
  - `src/workflow/predictionworkflow.cpp:229-257`（`PredictionWorkflow::preparePredictionJob`：`job->params.insert("INPUT", QVariant::fromValue(inputLayer))`，`inputLayer = layers->instantiate(...)`）
  - `src/workflow/predictionworkflow.cpp:374`（`computePredictionJob`：`proc->run(job->algorithmId, job->params, ...)`）
  - `src/ui/paleomainwindow_attach_predict.cpp:86-93`（task lambda 在 `PaleoTaskService` worker 线程执行 `pred->computePredictionJob(job.get())`）
- **Root Cause**: prepare 段解析 INPUT 时直接用 `QgisLayerService::instantiate()` 的返回值——该函数把图层 `proj->addMapLayer()` 进**活 QgsProject**（qgislayerservice.cpp:225-229），是主线程 QObject、与画布共享 GDAL/OGR provider。compute 段在 worker 线程把整个 `job->params`（含 `QVariant<QgsMapLayer*>`）原样交给 `QgisProcessingService::run()`。而该服务在 `run()` 内自述契约（qgisprocessingservice.cpp:662-667，#235-2）：「ConstraintWorkflow 在 worker 线程调用……**调用方不得传入主线程拥有的图层对象**」。paleo 算法侧用 `QgsProcessingParameterRasterLayer` + `parameterAsRasterLayer`（paleoalgorithms.cpp:1033-1064 等）直接解引用活层读栅格/要素。对照同仓正确范本：`runFactorAlgorithm`（constraintfactorjobs_factor.cpp:1191-1255）把图层参数冻结为 URI+CRS 快照、worker 上重建独立层；`computePolygonize`（mappingworkbench_polygonize.cpp:62-115）注释明言「图层对象属主线程，不得跨线程传」并只在 worker 建局部 `QgsRasterLayer`；`computeCompose`/`consistencyImpl` 同款。predictionworkflow 是全仓唯一违例点。
- **Trigger**: 智能预测页（六页工作流之①）选任一 `paleo:*` 栅格算法（如 `paleo:paleo_geological_smoothing`）→ 点运行。页面只收标量参数（P0-1 注释自述），INPUT 由工作流自动解析为活层；任务在 worker 线程执行时，GUI 线程可能同时经画布渲染/识别读同一图层与同一 GDAL dataset。
- **Observed/Expected**: 观察：主线程拥有的 `QgsMapLayer*` 在 worker 线程被 QGIS Processing 直接消费（provider 读块、可能的信号发射）。预期：worker 只接收冻结的 URI/CRS/字段快照并自建层（全仓既有纪律）。
- **Reproduction**: 打开带 horizon 栅格声明的工程 → 智能预测页选 `paleo:paleo_geological_smoothing` 类算法运行，同时缩放画布；静态复核 `attachPredictPage` task lambda 线程归属 + `preparePredictionJob` 的 `QVariant::fromValue(inputLayer)` 即可确证契约违反（无需崩溃复现即成立）。
- **Impact**: 跨线程使用活 QGIS 对象 = 未定义行为：与主线程并发读同一 GDAL dataset / provider 内部缓存可致崩溃或脏数据进预测栅格；违反项目自身成文契约使 #235-2 防线在此文件失效。六页主工作流之一的生产路径。
- **Existing Issue Match**: 无同根因（#472 是 Qgs* 越层散布的架构问题；#480 的 prediction 条目为 :813-824 井表合并与 _async 实例化失败，均已另行处理）。
- **Active PR Match**: `src/workflow/predictionworkflow.cpp` 不在 PR #518/#521 文件清单；`paleomainwindow_attach_predict.cpp` 属 #518 触及的 attach 各件家族，但本 finding 根因与修复点均在 workflow 层，与 #518 主题（issue-sweep 修复）无 hunk 冲突。修复合入前建议与 #518 复核该文件无叠改。
- **Fix Strategy**（最小根因修复）: `preparePredictionJob` 的 INPUT 解析改为照抄 `runFactorAlgorithm` 的 `Source` 快照（`{uri, provider, type, crs}`）进 `job->params`（字符串/值类型）；`computePredictionJob` 在 worker 用 `QgsRasterLayer(uri)`/`QgsVectorLayer(uri)` 重建局部层后再 `QVariant::fromValue` 交给 `proc->run`。约 20 行，模式全仓现成。
- **Regression Test 思路**: `tst_workflows` 增用例：prepare 后断言 `job.params["INPUT"]` 不是 `QVariant<QgsMapLayer*>`（是 URI 快照 map/字符串），且 compute 段（worker）执行后产物与旧结果逐字节一致（用小 fixture 栅格对拍）。注意现有 `tst_workflows.cpp:367` 一带已在 worker 跑 `computePredictionJob`，等于把违例状态固化进了测试——修复时需同步改测试注入方式。

### FC-002 [P3] [VERIFIED] 同步 `saveVersion` 的 `retainMapCitations` 失败产生「成功返回值 + 非空 *error」自相矛盾态

- **ID**: FC-002
- **Severity**: P3
- **Confidence**: VERIFIED
- **Module**: src/workflow
- **File:Line/Function**: `src/workflow/mapversioncontroller.cpp:336-343`（`MapVersionController::saveVersion` 收尾）
- **Root Cause**: 版本已落库（`v.version > 0`）后 `retainMapCitations` 失败时只把错误写进 `*error`（且仅当 error 为空时），随后照样 `emit versionSaved` 并返回成功的 `v`。调用方按返回值判成功则错误文本丢失；按 error 判失败则与真实落库状态矛盾。#480 P2-1 修掉了 `_async.cpp:181-182` 的同型自相矛盾态（instantiate 失败返 true 带 error），此处是同族残留实例；异步路径（mapversioncontroller_async.cpp:367-371 把 retainError 写入 `j.error`、`snapshotFinished(success=true, error=retainError)`）已有披露，唯同步路径无消费方。
- **Trigger**: 无任务服务的壳/测试直调 `saveVersion` 且 `retainMapCitations` 失败（版本已写、引用留痕失败）。生产 UI 走 `startSaveVersion`（异步）不经过此分支。
- **Observed/Expected**: 观察：返回成功 + `*error` 非空并存。预期：成功路径不设置 error，或降级为 `errorOccurred` 信号披露（同文件 `record()` 的 writeProject 失败处理式样）。
- **Reproduction**: 测试直调 `saveVersion`，注入 `retainMapCitations` 失败（如 catalog 引用版本缺失后恢复），观察返回值与 error。
- **Impact**: 低（生产不走同步路径）；契约一致性问题——「成功产物+失败报文并存」家族（#485 主题 3）的残留点。
- **Existing Issue Match**: 同族不同实例（#480 P2-1 已修 `_async` 一处；#480 P3-5 已修 predict 井表一处）。
- **Active PR Match**: 无。
- **Fix Strategy**: 收敛到异步路径同款语义：retain 失败时 `emit errorOccurred`（或 `qWarning`）并保持返回成功、不写 `*error`；一行改动。
- **Regression Test 思路**: `tst_versions`/`tst_mapsnapshot` 增「retain 失败时 saveVersion 返回成功且 error 为空、errorOccurred 有信号」。

### FC-003 [P3] [VERIFIED] 异步 publish 快照对 legacy（非冻结）来源零 stale 守卫，与 save 路径防御不对称

- **ID**: FC-003
- **Severity**: P3
- **Confidence**: VERIFIED（代码不对称确证；实际触发需旧工程）
- **Module**: src/workflow
- **File:Line/Function**: `src/workflow/mapversioncontroller_async.cpp:401-450`（`startPublish`）对照 `:327-346`（`launchSaveSnapshot` 的 `markStale` 十一路信号守卫）；`src/metadata/mapversionstore.cpp:976-1030`（`snapshotInputForPublish` 的 legacy 分支直接取 `decls` 的 `layer.source` 活文件）
- **Root Cause**: save 路径为每个矢量来源挂 `job->stale` 原子标记 + 图层变更信号（编辑/换源/CRS/销毁均置 stale），compute/commit 双重复查；publish 路径完全没有等价机制。现代工程的 publish 输入是冻结的 `saved_layers` 受管不可变文件（安全），但 legacy 分支（无冻结记录的旧版本）把**声明图层的当前 source 文件**（可能是可编辑的工作副本 GPKG）交给 worker 哈希/复制，期间 GUI 可继续编辑该文件 → 发布快照混合新旧状态且无人检测。
- **Trigger**: 打开 #439 之前保存过版本的旧工程 → 发起发布 → 发布哈希进行中编辑对应工作副本。
- **Observed/Expected**: 观察：publish 期间来源可变而无 stale 复查。预期：与 save 同款守卫，或至少 legacy 分支复用 `launchSaveSnapshot` 的 token+stale 机制。
- **Reproduction**: 需构造无 `saved_layers` 冻结记录的 map_versions 行（legacy fixture）+ 发布任务运行中写入工作副本。
- **Impact**: 低概率、legacy-only；一旦发生则发布出的「只读快照」内部状态不一致（部分文件旧部分新），违背「Published 快照只读且完整」契约（mapversioncontroller.h:25-29 自述）。
- **Existing Issue Match**: 无直接同根因（#476 是 publish 门禁 90 行复制的同文件不同问题）。
- **Active PR Match**: 无（mapversionstore/mapversioncontroller 不在 #518/#521 清单）。
- **Fix Strategy**: `startPublish` 在 legacy 分支为涉及的可变来源挂与 save 同款 `stale` 原子 + 图层信号（或最小版：commit 前对每个 file 复验 size+mtime 指纹，变化即失败重试）。
- **Regression Test 思路**: tst_mapsnapshot 级：legacy input + 发布哈希中途改写来源 → 发布必须失败并如实报「来源已变化」。

### FC-004 [P3] [VERIFIED] 相融合（fuseFactors）在 GUI 线程整段同步跑 QGIS Processing；无任务服务的预测同步回退同族——双栈并存且冻结 UI

- **ID**: FC-004
- **Severity**: P3
- **Confidence**: VERIFIED
- **Module**: src/workflow × src/ui 接线
- **File:Line/Function**: `src/workflow/compositionworkflow.cpp:156-279`（`CompositionWorkflow::fuseFactors` 同步 `proc->run("paleo:paleo_facies_fusion", …)`）；`src/ui/paleomainwindow_attach_compose.cpp:49-66`（`fuseRequested` 信号处理器在 GUI 线程直调，无任务化）；对照 `src/workflow/mappingworkbench_compose.cpp:270-303`（`startCompose` 走 JobRunner 后台）。同族：`src/workflow/predictionworkflow.cpp:466-480`（`runPrediction` 无任务服务时三段同线程、含 ONNX 加载+推理）。
- **Root Cause**: 合成编图存在两条入口——`fuseFactors`（合成页「融合」按钮，GUI 线程同步）与 `MappingWorkbench::startCompose`（后台 JobRunner、worker 自建层）。前者是旧栈：同步在 GUI 线程跑栅格融合，大网格上冻结 UI 数秒到更久；且无取消路径。注意：同线程使用活 INPUTS 层**不构成** FC-001 的跨线程违例（合法），问题仅在阻塞与双栈漂移。
- **Trigger**: 合成编图页选多个单因素栅格 → 点融合。
- **Observed/Expected**: 观察：GUI 冻结至融合完成、无任务行、无取消。预期：对齐 `startCompose` 的后台三段式，或明示该入口仅小图可用。
- **Reproduction**: 大网格（~4M 像元闸内）多输入融合，UI 无响应时长≈算法时长。
- **Impact**: GUI 冻结（用户可误判死机）；与后台栈的行为/回执口径漂移（#485 主题 1「双入口」家族的具体点）。
- **Existing Issue Match**: 无逐条命中（#480 P3-2 记的是 constraintfactorjobs 的双异步栈；此处为 compose 双栈）。与 #485 主题 1 同族。
- **Active PR Match**: 无。
- **Fix Strategy**: 最小：`fuseRequested` 接线改走 JobRunner（复用 `startCompose` 的 compute/commit 形制）；或短期给 `fuseFactors` 加像元数闸 + 状态行「同步计算中」提示。
- **Regression Test 思路**: 既有 tst_workflows/tst_factorworkflow 语义用例保持（同步直调仍可用于测试壳）；新增接线级用例断言任务服务在场时融合出任务行且 GUI 事件循环不被阻塞（QEventLoop 探针）。

---

## 3. 排除项汇总（命中外部工作/既有 issue，只记录）

| 观察到的现象 | 判定 | 依据 |
|---|---|---|
| `mappingworkbench_edit.cpp` saveEditingVersion 的 acknowledge 时机与 spatial_edit_events 继承 | EXCLUDED_EXISTING_ISSUE | #465（核对 /tmp/paleo-audit/issue-465.txt：现行代码 :404-429 已是改后形态——acknowledge 后置说明注释 + 井表合并失败如实披露「版本已保存」，与 issue 描述的旧形态不同；即便仍有残余争议也归 #465 管） |
| `mappingworkbench_predict.cpp:459` 后台读曲线用 `LasParser::parse` 旧入口（整份解析、无大小闸） | EXCLUDED_EXISTING_ISSUE | #466（root cause 在 io/lasparser，workflow 侧是消费者清单条目） |
| `onnxpredictionservice.cpp` 推理与会话构建同一把互斥锁（runTensorOn 全程持锁） | EXCLUDED_EXISTING_ISSUE | #479 P2 第 2 条 |
| `remotepredictrouter.cpp` degradeOrDie 本地 ORT 降级在主线程槽内同步执行 | EXCLUDED_EXISTING_ISSUE | #479 P2 第 1 条 |
| `horizonsuggest.cpp` 候选去重 O(n²)/最近邻 O(n·m)；`wellfaciesservice.cpp` m_accepted 只增不减 | EXCLUDED_EXISTING_ISSUE | #479 P3 |
| `wellfaciesservice.cpp` 等远端井相服务（#133 纪律）响应上限、超时、轮询 15 分钟闸 | 无新问题 | 现行实现有界且如实（对照 #469 的反面教材） |
| `mapversioncontroller.cpp:60-86` 幽灵层披露（dropped_layers）、`:192-268` 跨层提交非原子披露 | EXCLUDED_EXISTING_ISSUE | #480 P2-1/P2-2（现行代码已带披露注释） |
| `mappingworkbench.cpp` generateFactor/generateContours 的 declare 失败口径 | EXCLUDED_EXISTING_ISSUE | #480 P2-3（publishFactorAlias 信号链收尾形态） |
| `mappingworkbench_predict.cpp:843-861` 预测登记成功但井表维护失败并存态 | EXCLUDED_EXISTING_ISSUE | #480 P3-5（现行代码已有「成果可用+失败环节」披露文案） |
| `mappingworkbench_predict.cpp` 取消不再报失败、全 NoData/全井跳过不发布空图 | 无新问题（已修） | #493 U4/U5 已合入基线 |
| `remotepredictrouter.cpp:105` 响应体上限 | 已在基线修复（#469） | 现行代码 :96-148 有 readyRead/metaDataChanged 双通道上限 + `read(limit+1)` 兜底 |
| workflow 层 Qgs* 直连（evidenceinteraction/mappingworkbench_compose/horizonbatchexport/faultinterpretationcontroller.h 等） | EXCLUDED_EXISTING_ISSUE | #472（架构收敛主题） |
| `wellsectionworkflow`/`sectionworkbench` LAS 解析失败静默 continue | EXCLUDED_EXISTING_ISSUE | #480 P3-1 |
| constraintfactorjobs 双异步栈、mutationSeq 陈旧判据、FaultEditStack push 即执行 | EXCLUDED_EXISTING_ISSUE | #480 P3-2/3/4（constraintfactorjobs_factor.cpp:1073-1076 有双栈备忘注释） |
| `mappingworkbench.cpp/.cpp_edit.cpp/mappingevidencereview.cpp/mappinghierarchy.cpp` 上相面拓扑编辑/分割合并/覆盖绘制主题 | EXCLUDED_ACTIVE_WORK | PR #521 |
| `src/ai/onnx*、remote*、wellfacies*、horizonsuggest` 上与 #460-#503 同根因的修复 | EXCLUDED_EXISTING_ISSUE / EXCLUDED_ACTIVE_WORK | PR #518 |
| 导出链（horizonbatchexport/mapexport/mapbook/mapbookqueue/layouttemplatelibrary 及主仓未提交的 dataexportworkflow/geologicalexport 簇） | EXCLUDED_ACTIVE_WORK | 排除矩阵 §4 |
| `seismicmaplink` 测线层死接线 | EXCLUDED_EXISTING_ISSUE | #490 已知限制 3（域外未修，登记在案） |
| `mapversionstore` publish/snapshotInputForPublish 门禁复制 | EXCLUDED_EXISTING_ISSUE | #476 P2-4（#476 注释 :730 显示已做共用收敛） |
| busy-token 跨工程残留（快照 token 按 owner 释放 + epoch 守卫） | 无新问题（基线已含 #464 方向的 workflow 侧修复：`releaseTokens` 按 tokenOwner；store 侧清理属 metadata 域） | #464 |

**核对过的关键链路（未发现问题，留作复核线索）**：
- 关工程在飞预测：`projectAboutToClose → PaleoTaskService::beginNewSession → requestCancel → cancelAsked → MappingWorkbench::cancelPrediction`（beginPredictionTask 挂的槽）→ `m_request` 清空 + router cancel → 迟到 completed/failed 均被 id 闸门丢弃；`releaseProjectSession` 不关闭 catalog，`finishPrediction` 的 `isOpen()` 早退分支在生产不可达（不会卡 busy）。
- `JobRunner`（jobrunner.h）三段式：commit 在 `finished` 槽同步回 owner 线程、失败态仍进 commit、`PaleoTask::session` 过期比对、`writeBackCommitError`——迟到/取消/换代守卫完整；`BatchQueue`（batchjobqueue.cpp）的会话守卫、熔断、FIFO 补泵、槽位回收均闭合。
- `enqueueWrite` 同步执行（mutex 内直调 + try/catch），`saveVersion` lambda 按引用捕获无悬垂；`BatchSave::flush` 失败回滚内存（#169 语义）、abort-after-flush 幂等（`m_done` 闸）。
- `purgeAssets(atomic=true)` 单 BatchSave 全有或全无 → `removeProduct` 的 `restoreDeclarations` 不会复活已净化资产。
- ONNX 服务：析构排空 `m_inFlight`；`setModelRoot` 持锁删池与推理互斥；runTensorOn 绑定模型名不受并发换活动模型影响（性能问题归 #479）。
- `WellPredictionService` 逐井推进/跳过/失败链的 index 对齐（wells↔m_data↔m_runs 1:1）核对无越界。
- `DataCatalog::createStagingCopy`（m_staging=true、moveToThread(nullptr)、m_store 空）供 worker 独占使用——`MappingWorkflow::startThicknessChain` 的 staging 目录 + journal 回放 + `mutationSeq`/session 守卫闭合。
- `LLvmClient`/`WellFaciesService`/`HttpRemoteTransport` 三处网络栈的 generation/runId/字节上限/密钥脱敏纪律一致，无静默降级为有效结果的路径（horizonsuggest 对 logits 越域整模型拒收是正面例证）。

---

## 4. 未覆盖区域

以下文件在本轮**未逐行阅读**（接口/模式扫描或完全未看），后续审计不应假设其无问题：

- src/workflow：mappingworkbench_impact.cpp（1143）、mappinghierarchy.cpp（186，PR #521 领地）、mappingevidencereview.cpp 后 ~600 行、constraintworkflow.cpp 主体、constraintfactorjobs_geostat/contours/interpretive/localdirection.cpp、constraintwellfactors.cpp、constraintimport.cpp、propertymodelworkflow.cpp（1432）、realizationworkflow.cpp（1257）、wellsitingworkflow.cpp（1241）、wellsectionworkflow.cpp 主体、sectionworkbench.cpp 主体、depthconversionworkflow.cpp、inversionworkflow.cpp、evolutionworkflow.cpp、faciesclassify.cpp、faciesmappingworkflow.cpp、surfacegridding.cpp、welltopseditorworkflow.cpp、welltrajectorylayer.cpp、faultsurfaceworkflow.cpp、faultinterpretationcontroller.cpp、storagegovernancecontroller.cpp、officepreviewsession/xmlpreviewsession/stratigraphicwebsession/pythonconsolecontroller 主体、mamcltool.cpp、mapbook*/mapexport/horizonbatchexport/layouttemplatelibrary（排除矩阵 §4）、aichatcontroller.cpp/aichattoolrunner.cpp 主体、aiassistworkflow.cpp 主体（~700 行未读）、wellfaciesworkflow.cpp 主体、wellpredictionreview.cpp、wellimpedancetwt.h、datagraph/derivation/importledger/folderimport/assetops 之外的数据管理簇（opt-01 领地）
- src/ai：tileinference.cpp（#460/#493 领地未重审）、onnxfixture.cpp、remotepredictconfig.cpp、chat/{chatsession,chatmessage,domaintools,llmkeystore,markdown}.cpp
- src/linkage：seismicmaplink.h/wellmaplink.h/selectioncontext.h/threewaylocator.h 头文件细节（cpp 已全读）
- 导出链（EXCLUDED_ACTIVE_WORK，只登记）

---

## 5. 统计

新问题 4 条：P1×1（FC-001，VERIFIED）、P3×3（FC-002 VERIFIED、FC-003 VERIFIED、FC-004 VERIFIED）。SUSPECTED 单列：0（宁可不上也不猜）。
