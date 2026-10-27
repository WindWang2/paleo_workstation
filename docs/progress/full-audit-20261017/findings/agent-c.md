# Agent C — 工作流/AI/联动层 + 跨模块调用关系审计报告

审计基线：`origin/master c720aef5`（仓库 `/home/kevin/projects/paleo_workstation-full-review-20261017`）
范围：`src/workflow/`（全部）、`src/ai/`、`src/linkage/`，及 #496/#493/#491/#494 涉及的 algorithms/domain/qgis 接口面。
方式：只读静态深审；每个疑似问题追踪真实执行路径与触发条件；排除矩阵（#460/#465/#469/#480 正文、codex/audit-remediation 已改文件根因、主 checkout 未提交的 constraintpage 编图簇）已核对，重合者未报。

## 汇总

| 严重度 | 数量 | ID |
|---|---|---|
| P1 | 1 | C-01 |
| P2 | 1 | C-02 |
| P3 | 7 | C-03 … C-09 |

无 P0。异步编排基础设施（JobRunner 三段式、世代号、会话守卫、batchjobqueue、aichattoolrunner、SelectionContext 再入界）经逐行复核维持前轮「接近教科书级」评价；本轮最高价值发现集中在 #494 新引入的归因图层缓存生命周期与 #496 未收口的屏障栅格化双轨。

---

## C-01 归因图层 8 格缓存在解析中途删除仍在使用的 QgsLayer → use-after-free

- **Severity**: P1　**Confidence**: VERIFIED（代码路径完整推演；触发条件确定，命中哪个指针取决于 QHash 迭代序，但 ≥9 输入时必然命中一个在用指针）
- **Module**: workflow（#494 新代码）→ qgis/composeattribution
- **File:Line / Function**:
  - `src/workflow/mappingevidencereview.cpp:264-301` `MappingWorkbench::attributionLayer()`
  - `src/workflow/mappingevidencereview.cpp:302-389` `composeAttribution()`、`:392-445` `composeAttributionAt()`（逐输入 `input.layer = attributionLayer(...)` 存裸指针）
  - `src/workflow/mappingworkbench_impact.cpp:51-106` `resolveFrozenCompose()`、`:257-273` `analyzeImpact()`（frozen + substituted 两轮解析）
  - 消费端：`src/qgis/composeattribution.cpp:96-98`（`input.layer->isValid()`、`qobject_cast`、`provider->sample`）、`:485-545` `replayGrid()`
- **Root Cause**: `attributionLayer()` 以 `mutable QHash<QString,QPointer<QgsMapLayer>> m_attributionLayers` 缓存按需打开的版本图层，容量到 8 即 `delete first.value().data()`（QHash 无序，删任意一个）。调用方（composeAttribution / composeAttributionAt / resolveFrozenCompose）在**循环解析全部 ordered_inputs 的过程中**把返回的裸 `QgsMapLayer*` 存进 `ResolvedInput::layer`，解析完才整体交给 `replay()/replayGrid()`。第 9 个不同版本入缓存时驱逐的恰是前 8 个之一 → `inputs[k].layer` 悬垂 → `decideAt()` 对每个输入执行 `!input.layer || !input.layer->isValid()`，非空悬垂指针直接解引用。
- **Trigger**（用户可达，无需异常输入）:
  1. 综合编图勾选 ≥9 个输入层（`prepareCompose`/`checkedInputs()` 对数量无上限），生成 `ordered_inputs` ≥9 的 composed_facies 版本；此后对该成果任一图斑的证据详情（`featureEvidence`→`composeAttribution`）、局部归因（`composeAttributionAt`）即命中；
  2. 影响分析：8 输入成果 + 版本替换（`analyzeImpact` 的 `sub{{before,after}}` 使 substituted 解析引入第 9 个不同版本）→ `replayGrid(frozen.inputs, …)` 解引用已删层。
- **Observed vs Expected**: 期望缓存容量治理只影响缓存命中率；实际把仍在语义使用期（ResolvedInput 生命周期）的对象 delete。主线程堆 UAF：`qobject_cast`/`isValid()`/`sample()` 读已释放内存——轻则 ASAN 报堆读越界，重则段错误（GUI 线程）。
- **Existing Match**: NEW（#494 引入；tst_evidencee2e/tst_composeattribution 用例输入数 ≤8，未覆盖 ≥9 输入路径；不在 #460-#485 与 audit-remediation 范围内）。
- **Fix Strategy**: 任选其一（推荐 1）:
  1. `ResolvedInput` 持有 `std::shared_ptr<QgsMapLayer>`（`layer.release(...)` 定制 deleter 或改 unique 语义），缓存与输入列表共享所有权，驱逐只影响下次打开；
  2. 驱逐改为引用计数守卫：解析会话期间对已取指针登记 keep-alive 集合，会话结束（replay 返回后）再 `deleteLater()`；
  3. 最小止血：解析循环内不做驱逐（容量上限只在会话间生效）。
- **Regression Test**: 构造 10 输入的 composed_facies 版本，`featureEvidence`/`composeAttribution` 取图斑证据，ASAN 下断言无 UAF；impact 分析用 8 输入 + substitute 分支同口径补一例。

---

## C-02 SurfaceGridding 屏障栅格化仍是半格采样 + 半开 floor 判格——未随 #488 统一到 supercover 闭域契约

- **Severity**: P2　**Confidence**: VERIFIED
- **Module**: workflow/surfacegridding（#496 只接了 domain 谓词，未迁移栅格化）
- **File:Line / Function**: `src/workflow/surfacegridding.cpp:518-557` `SurfaceGriddingWorkflow::buildBarrierMask()`；对照权威实现 `src/algorithms/constraintbarriers.cpp:81-231`（`supercoverPolylineCells` + `rasterizeBarrierParts`）
- **Root Cause**: #488(b2ed2da4) 把三个 Processing 引擎（paleo_constraint_idw / paleo_distance_transform / paleo_min_curvature）的屏障栅格化统一为 supercover（闭域裁剪 + 4-连通途经链，贴右/下边界点 clamp 进边缘格），并明确记录旧「半格采样在陡斜线/穿角点漏格、对角链墙可从格缝穿透」。`buildBarrierMask`（工作流层网格化，喂 `gridsolver::solveMinimumCurvature` 的 mask）**未被迁移**：仍是 `steps = ceil(len/(cell*0.5))` 沿线采样 + 本地 `cellOf`（floor，x==xMax / y==yMin 判格外、无 clamp）。#496 给它接上了 `context.domain`（计划谓词），但谓词（闭矩形，constraintsemantics::segmentTouchesRect）与引擎标记（半开 floor）口径相反。
- **Trigger / Observed vs Expected**:
  1. 数值：陡斜/穿角屏障链在层位文本网格化 mask 上漏格 → 最小曲率插值从角点缝隙穿渗（与 #488 修复前三个引擎同病）；
  2. 契约：恰在 x==xMax/y==yMin 的贴边屏障——三引擎按闭契约消费（clamp 进边缘格，`edgeAlignedBarrierIsOutsideForAllGridEngines` 四贴边线全消费），此处零格标记 → 回执翻成 `consumed=false/outside_compute_domain`，同一几何在不同引擎得到相反回执；跨 ≥2 段的线其贴边段静默无格。
  3. 测试真空：`tests/tst_surfacegridding_thread.cpp` 两处均 `opt.useBarriers=false`；edgeAligned 一致性测试只覆盖三个 Processing 算法，不含本函数。
- **Impact**: 层位网格化的地质屏障约束可被静默削弱；回执口径与三引擎矛盾误导质控。#496 提交信息自称「计划/引擎域口径统一」，此处是残留缺口。
- **Existing Match**: NEW（swarm 主题 1「同一功能双份维护」的又一实例，但该具体点不在 #480 清单，也不在 audit-remediation 已改文件中）。
- **Fix Strategy**: `buildBarrierMask` 的栅格化段改调 `paleo::algorithms::supercoverPolylineCells`（或直接复用 `rasterizeBarrierParts`，顺带获得 rasterized_cells/fully_shadowed 回执细化）；删除本地半格采样与 `cellOf`。
- **Regression Test**: 把 `edgeAlignedBarrierIsOutsideForAllGridEngines` 的四贴边线口径扩到 `SurfaceGriddingWorkflow::buildBarrierMask`（同网格、断言 mask 边缘格置位）；加 45° 对角屏障链 mask 连通性断言（4-连通下两侧格不可达）。

---

## C-03 旧 IDW 引擎内联栅格化复制了 rasterizeBarrierParts 但缺回执细化字段（双轨维护）

- **Severity**: P3　**Confidence**: VERIFIED
- **Module/File:Line**: `src/algorithms/paleoalgorithms.cpp:542-594`（ConstraintIDWAlgorithm，#496 R3 新增内联实现）vs `src/algorithms/constraintbarriers.cpp:172-231`
- **Root Cause / Observed**: #496 给旧 IDW 手写了与 `rasterizeBarrierParts` 语义相同的「先栅格化、未落格回执翻 outside_compute_domain、只计落格数」逻辑，但缺少统一实现的 `rasterized/rasterized_cells/new_cells` 字段与 `fully_shadowed_by_other_barriers` 告警。同一约束输入在不同引擎产出的回执形状不同，下游（receiptsemantics 归一、UI）拿到的信息不对称；两份实现今后修一处漏一处的风险正是本仓第一大 bug 来源。
- **Existing Match**: NEW（主题重合 swarm 主题 1，具体点未登记）。
- **Fix**: 旧 IDW 改用 `readUnifiedHardBarriers + rasterizeBarrierParts`（需把 supercover 输出并进 `barrierMask`，BFS 复用路径不变）。
- **Test**: `griddingReceiptsMatchPlan` 增加 paleo_constraint_idw 行，断言回执含 rasterized_cells。

## C-04 结构面停线「计划触域拒绝 vs 引擎半径内消费」回执反向失真

- **Severity**: P3　**Confidence**: VERIFIED（#496 决策账已声明为「保守」，但方向是回执少报消费）
- **Module/File:Line**: `src/domain/constraintsemantics.cpp:659-661`（plan：域已知且不触域 → reject outside_compute_domain）vs `src/algorithms/singlefactor/structural.cpp:415-428`（engine：GEOSDistance > softRadiusAuto 才跳过）
- **Root Cause / Observed**: 域外但在软半径 softRadiusAuto 之内的停线：引擎实际进入趋势权重（有数值作用、回执 soft_boundary），计划（约束页 UI、`lastFactorComputeDomain` 域）判 outside_compute_domain。UI 侧用户看到的「未消费」清单与引擎 qc 落盘的 constraint_receipts 不一致。#496 注释自认「计划用触域作保守下界」——建议至少在计划回执 warnings 里标注 `may_still_reach_within_radius`，或计划侧同样用「距离 ≤ 半径」判定（域矩形外扩 softRadiusAuto 即可）。
- **Existing Match**: NEW（#496 自留的剩余风险，未立 issue）。

## C-05 屏障开关按 displayName 大小写不敏感匹配层位

- **Severity**: P3　**Confidence**: HIGH
- **Module/File:Line**: `src/ui/paleomainwindow_attach.cpp:163-170`（#496 R8 接线，workflow 消费方 `barrierHorizons` 来自 `surfacegridding.cpp:140-160`）
- **Root Cause / Observed**: `ctxIn.barrierHorizons.contains(a.displayName, Qt::CaseInsensitive)`——约束库 horizon 列是精确串，资产 displayName 可能带修饰/大小写差异（"H1 top" vs "H1"）；不匹配时静默隐藏本层位的屏障开关（`ctx.hasConstraints=false`），用户无从知道约束存在但开关没亮。下游 `buildBarrierMask→store.load(horizon)` 是精确匹配，两者口径还不一致。
- **Existing Match**: NEW。**Fix**: 匹配用资产登记时的 horizon 字段（`CatalogAsset` 的 horizon/extra）而非 displayName；或显示「层位名不一致，约束未点亮」提示。**Test**: displayName 与约束 horizon 大小写/前后缀差异的夹具断言开关可见性与提示。

## C-06 保存版本 commit 成功后 retainMapCitations 失败：版本已入库 + 失败语义混在 committed=true 里

- **Severity**: P3　**Confidence**: VERIFIED
- **Module/File:Line**: `src/workflow/mapversioncontroller_async.cpp:361-367`
- **Root Cause / Observed**: `commitPreparedVersion` 成功后 `retainMapCitations` 失败仅回写 `j.error` 并继续 `emit versionSaved`；jobCompleted 里 `snapshotFinished(…, job->prepared.committed=true, …, job->error)`——「成功产物 + 失败报文」并存，引用保留缺额无补偿/重试入口，且 UI 按位序可能只显示成功。与 swarm 主题 3 同型（predict 登记+井表合并失败是该主题已登记实例；此点未被 #480 收录）。
- **Existing Match**: NEW（主题重合）。**Fix**: citations 失败降级为 versionSaved 附带 warning（或提供重试保留入口），error 不与 committed=true 同帧。

## C-07 wellfactor 版本 extra 的 `layer_id` 双重插入（首个死代码）

- **Severity**: P3　**Confidence**: VERIFIED
- **Module/File:Line**: `src/workflow/constraintwellfactors.cpp:611-613`
- **Observed**: `extra.insert("layer_id", id)` 随即被 `extra.insert("layer_id", "product.<versionId>")` 覆盖（前者死代码）；别名语义实际由 `manifest_layer_id` 承担（与 constraintfactorjobs_localdirection.cpp:485-486 同型正确写法）。无行为错误，纯误导性死写。**Fix**: 删第一行。**Existing Match**: NEW。

## C-08 反演统计均值被失败道稀释

- **Severity**: P3　**Confidence**: VERIFIED
- **Module/File:Line**: `src/workflow/inversionworkflow.cpp:584-618`
- **Observed**: `sumStat` 只累计成功道，分母却是 `processedTraces = done`（含 failedTraces 与几何/采样数不符的道）→ `meanResidualEnergyRatio / meanLowFreqVarianceFraction` 系统性偏低；QC 面板据此评估反演质量会高估稳定性。**Fix**: 分母改为成功道数（`done - failed`，为 0 时置 NaN 并如实展示）。**Existing Match**: NEW。

## C-09 预测 INPUT 回退抓任意栅格 + 父版本按名字子串匹配

- **Severity**: P3　**Confidence**: SUSPECTED（设计意图内的回退，但静默语义有风险）
- **Module/File:Line**: `src/workflow/predictionworkflow.cpp:228-256`（INPUT 回退：先 `horizon.<target>*`，否则**任一**第一个 raster 声明）、`:174-197`（父版本：实体名 `contains(horizon, CaseInsensitive)` / displayName `contains(horizon)`）
- **Observed**: 无目标层位栅格时预测在无关栅格（如地震属性、别的层位）上运行并登记 DERIVED 成果，仅 provenance 记路径，用户无「输入不对」提示；父版本子串匹配下 "H1" 会挂上 "H12" 的当前版本，血缘虚增。**Fix**: 回退命中非目标层位栅格时在 task detail/extra 里显式记录 `input_fallback=true` 并注明实际层位；父版本匹配收紧为相等比较。**Existing Match**: NEW。

---

## 明确复核为「无新问题」的重点区域（负结果）

- **#496 (a1efbd74) 其余改动**：`constraintHorizonSubsetSuffix` '|' 拒绝及六处接入点（含 addConstraint 先写后删的回滚次序、`loadConstraints`/`syncConstraintGeometryViews` 跳过+告警、mappingworkbench 快照 A3 空失败路径——两处调用方 `isEmpty()+error` 语义核对无误）；`assignStoreCrsIfMissing` 三态溯源与 `layerCrsProvenance`；`constraintuseplan` id 归一（trim+fid 兜底与三引擎读者口径一致）；`qgisconstrainteditsession` F4 fid 复核（占用判定含空身份≠本身份，undo/redo 幂等保持）；`lastFactorComputeDomain`（`setCatalog` 清 `m_preparedWellFactors`，无跨工程残留）；`paleoalgorithms` 网格前置后的取消路径（outDs 尚未创建，无泄漏）；`structural.cpp` GEOSDistance 失败保守按可达。测试 `hostileHorizon*`/`pipeHorizon*`/`redoSurvivesFidReuse*` 断言真实（非空转）。
- **#493 (opt-04)**：`cancelPrediction` 先摘 m_request 再 cancel 的次序修正自洽（finishPrediction 有 id 闸门，迟到成功/失败都不回灌）；地震全 NoData 不发布（判据前置 gridShapeOk 正确）；tileinference `softmaxGrid` 退化概率/单通道置信度修正逻辑正确（behavior change 已在决策账声明）。
- **#494 其余**：`evidencerecord` upstream_receipts（cap 32 防环）；`commitCompose` compose_receipts 与 ordered_inputs/winningCells 对齐；polygonize PROVENANCE 落库；`evidenceinteraction` 工程重开恢复（session/projectId/horizon 采纳次序）与 `destroyed` 防替换层误清；`bindCatalog` 对 attributionLayers/evidenceIndex 的工程切换清理；`executeRecompute` 任务模式单飞如实报「请等待」。
- **异步编排**：`services/jobrunner.h`（世代/会话/ownsHandle/commit 失败回写，#159/#235 修复在位）；`mapversioncontroller_async`（epoch/projectDirectory 双守卫、stale 标志、tokenOwner 释放、launch 失败释放 token；#464/#480 已知项未重报）；`batchjobqueue`（会话守卫、槽位忙回队、#415 延迟泵、出队上限=并发度）；`aichattoolrunner`（generation+callId 双闸、单发钩子即拆、toolFinished 统一排队防迭代器失效重入）；`depthconversion`（commit 会话守卫、模型已登记保留在册的如实部分态）；`mappingworkflow` 厚度链（catalog 指针+mutationSeq+session 五重 stale 判定）；`inversionworkflow` 线程池除已排除的 join 问题外无新发现。
- **src/ai**：`tileinference` HEAD 复核——除 #460（needPad 先索引后校验）外，softmax/stitch（每格恰写一次、feather 权重、NoData 不累计、类数跨 tile 一致闸）正确；`remotepredictrouter` runId 契约、超时先 bump 后 abort、单次 timer 连接均正确（#469 响应体上限除外）；`onnxpredictionservice`/`wellfaciesservice` 未发现排除项外的新问题。
- **src/linkage**：`selectioncontext`（再入合并 + 4 代 settle 上限 + 丢弃告警）、`wellmaplink`（QSignalBlocker 防乒乓、索引按编辑代失效、rebind 恢复）、`threewaylocator`（(0,0) 井点防误判）均无新发现；`seismicmaplink` 画布像素绘制属 #472 Qgs 越层主题未重报。

## 覆盖清单（本 agent 实际逐行/逐函数读过）

- workflow 全目录索引 + 深读：mappingworkbench{,_edit,_compose,_predict,_impact,_polygonize,_consistency}.cpp、mappingevidencereview.cpp、evidenceinteraction.cpp、evidencerecord.cpp、surfacegridding.{cpp,h}、constraintworkflow.cpp、constraintwellfactors.cpp、constraintuseplan.cpp、workflows.{h,_internal.h}、mapversioncontroller{,_async}.cpp、batchjobqueue.cpp、mappingworkflow.cpp、inversionworkflow.cpp、depthconversionworkflow.cpp、predictionworkflow.cpp、aichattoolrunner.cpp、wellsectionworkflow.cpp（线程断言与请求世代段）
- ai：tileinference.cpp（全文）、remotepredictrouter.cpp（传输层全文）
- linkage：selectioncontext.cpp、wellmaplink.cpp、threewaylocator.cpp（全文）
- 跨模块接口面：algorithms/paleoalgorithms.cpp（约束消费段全文）、algorithms/constraintbarriers.cpp（全文）、algorithms/singlefactor/structural.cpp（停线段）、domain/constraintsemantics.cpp（域判定段）、qgis/composeattribution.cpp（gridOf/decideAt/replayGrid 段）、qgis/constraintcrs.cpp、qgis/qgisconstrainteditsession.cpp（F4 段）、ui/paleomainwindow_attach.cpp（runHorizonGridding 段，#496 接线复核）
- 提交考古：a1efbd74(#496) 全量 diff + tst_singlefactor_constraintreceipts / tst_constraintediting 关键用例；66e79e61(#493)、ddd1325e(#494)、91fd9432(#491 stat) workflow 侧 diff；b2ed2da4(#488) 与 surfacegridding 的交集核对（确认未迁移）
- 排除矩阵核对：gh issue #480 正文、docs/ledger-archive/reviews/COMPREHENSIVE_AUDIT_2026-10-16.md、docs/ledger-archive/reviews/SWARM_REVIEW_2026-10-15.md、docs/progress/full-audit-20261017/01-conflict-matrix.md
