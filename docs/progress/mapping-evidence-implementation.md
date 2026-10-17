# 编图证据深化实现（mapping-evidence-implementation）

三批修复的落地记录。姊妹篇：`mapping-evidence-gap-matrix.md`（审计发现的
16 项缺口 + flake 根因）描述「改前」；本文描述「改后」与数据契约；
`mapping-evidence-completion.md` 记录证据体系首发（G03–G14）。
引用一律 `文件:函数`，不钉行号（代码仍在演进）。

## 1. 基线与分支

- 基线：`origin/master` = `c720aef5`（2026-10-16，#505）。
- 分支：`codex/mapping-evidence-deepening`；独立 worktree
  `../paleo_workstation-mapping-evidence`（主 checkout 未提交改动未触碰）。
- 提交：
  - `619736e8` P0-A 归因保真与内存安全（X1/X2/X3/X4/X15/X16 + 转面要素序 flake）
  - `2a61b3c6` P1-B 影响分析/受控重算/反向索引（X5/X6/X7/X8/X9/X12/X13 + 评审 0a–0c）
  - `ece5b47b` P1-C 地图—证据—图谱联动与图谱语义（X10/X11/X14 + 评审 0b）

## 2. 已核验并复用的既有能力（未重写）

- **G03 决策口径**：`composeRaster` 的 priority-first-valid/threshold/
  categorical-nearest 决策序原样复用；所有修复只让重放与真实算法同口径，
  未改任何数值算法。
- **G04 井落面**：Formations 表按 `formation==horizon` 取行的地层优势相口径
  与预测写路径同源。
- **G05 三级影响**：determined/potential 分级与采样下界标注沿用。
- **G06 冲突分析**：`paleo::evidenceconflict::analyze` 七类 conflict_type/
  四级 severity，不合成置信度。
- **G08 索引**：`paleo::evidenceindex::Index` 增量同步/幂等/损坏跳过/有界查询。
- **G09** `compareVersions` 像元转移矩阵；`rerun_layer_id` 修正（首发已修）。
- **G10** `paleo::receipts::normalize`/`compareWithPlan` 回执归一。
- **G11 有界性**：受影响图斑采样钳、井清单 500、索引 limit 1000、
  图谱 work/maxTraversalEdges 预算。
- **G12** 反向命中行复用 `EvidenceInteraction::navigate` 定位链；P1-C 另加
  `navigateFeature` 命中行→主目标 + 回退栈（见 X10）。
- **G13** `MappingWorkbench::localOperator` + #442 强制 + 追加式审计。
- **G14** `tst_evidencee2e` 端到端合流（归因→反驳→冲突→反查→保存→重开）。

## 3. 实际修复的缺口（X1–X16 + flake）

### X1 归因缓存驱逐悬垂指针（P0-A）
- **问题**：LRU 驱逐时 `delete` 正在使用的 `QgsMapLayer`（UAF）。
- **修改**：缓存与解析结果都改 `std::shared_ptr<QgsMapLayer>`；驱逐只丢缓存
  引用，持有者保活。
- **文件**：`mappingevidencereview.cpp:attributionLayer`
  （`m_attributionLayers` + `m_attributionLru`，8 上限、命中移尾）；
  `composeattribution.h:ResolvedInput::hold`（每个已解析输入全程固定图层）；
  调用方（impact 保护循环/受影响循环/`compareVersions`/`polygonCount`）持
  `shared_ptr`。
- **测试**：`tst_composeattribution::attributionCacheEvictionKeepsInputsAlive`
  （9 个冻结输入在 resolve 中途驱逐，修复前 `inputs[].layer` 悬垂）。

### X2 冻结网格 ≠ 真实 composeRaster 网格（P0-A）
- **问题**：历史 `gridOf` 对合并范围无条件 `grow(100)`；`composeRaster` 只在
  退化范围时 grow。纯点输入的重放网格与真实网格不同。
- **修改**：成果保存时把网格写入版本 extra `compose_grid`；重放先取冻结网格，
  缺失再按 composeRaster 分支逐条镜像重建（首个「栅格旗标」输入定网格；
  否则全部输入合并范围，仅退化时 grow，64×64）。
- **文件**：`mappingartifactwriter.h:ComposeReceipt`（网格字段）；
  `mappingworkbench_compose.cpp:commitCompose`（写 `compose_grid`）；
  `composeattribution.cpp:gridOf`/`frozenGridFromExtra`；
  `mappingworkbench.h:FrozenCompose`。
- **测试**：`tst_composeattribution::frozenGridLocksComposeReplay`（冻结网格
  逐像元同口径；无效 FrozenGrid 走重建，旧逻辑会得到错误范围而失败）。

### X3 块读重放 + fidelity 诚实（P0-A）
- **问题**：图斑级重放用 `provider->sample()` 逐点取样，与真实算法 `block()`
  重采样口径不同；文档声称「unresolved 注明 fidelity」但代码中不存在
  fidelity 字段。
- **修改**：`replay()` 对每个栅格输入在网格对齐窗口做一次 `provider->block`
  读；像元总数 >1,048,576 时退回逐点 `sample()` 并如实记 `sampled_fallback`。
  每输入 `alignment` ∈ `grid_defining | aligned | resampled |
  sampled_fallback | categorical_nearest`；`fidelity=exact` 仅当冻结网格或
  栅格定义的重建网格且全部胜出栅格 aligned、无 fallback；否则 `approximate`
  + `fidelity_reasons`。`replayGrid` 整网格块读同口径。
- **文件**：`composeattribution.cpp:replay`/`replayGrid`/`alignmentsOf`/
  `alignmentFor`。
- **测试**：`tst_composeattribution::resampledInputHonestFidelity`。

### X4 单点重放按点击位置取样（P0-A）
- **问题**：`replayAt` 在用户点击坐标取样；真实算法按像元中心决定。
- **修改**：先算 (col,row)，网格外如实 `位置在编图网格之外`，否则在像元中心
  决策；输出带 `cell{col,row,x,y}`、`query_point`、`grid_source`、`fidelity`。
- **文件**：`composeattribution.cpp:replayAt`。
- **测试**：`tst_composeattribution::pointReplaySnapsToCellCenter`。

### X5 重算计划不绑定影响报告（P1-B）
- **问题**：执行时取「当前版本」，报告过期仍按 V3 静默重算。
- **修改**：`plan.binding = {kind, source_before_version,
  source_after_version, source_asset_id, report_path, analyzed_at,
  catalog_revision}`；`executeRecompute` 拒绝无 binding 的计划；
  `kind==input_version` 时若 `currentVersion(source_asset_id)` ≠
  `source_after_version` 拒绝整个计划（`影响报告已过期`）。
- **文件**：`mappingworkbench_impact.cpp:recomputePlan`/`executeRecompute`。
- **测试**：`tst_impactrecompute::stalePlanBindingIsRejected`（拒绝且未产生
  新 composed_facies 版本）。

### X6 重算拾取旧冻结输入（P1-B）
- **问题**：potential 条目直接复制 `ordered_inputs`（冻结 product id），重算
  跑的是旧输入——产出「新版本」但与旧版相同。
- **修改**：determined/potential 条目统一
  `inputs=[{layer_id, frozen_version, current_version, rerun_layer_id}]`；
  `inputs_changed` 计数；模式词表
  `no_change | blocked_upstream | local_rerun（execution_scope:"full_grid"）
  | whole_rerun`，每条带 `reason`；`no_change`/`blocked_upstream` 结果
  `skipped:true`，永不执行。
- **文件**：`mappingworkbench_impact.cpp:recomputePlan`/`executeRecompute`。
- **测试**：`tst_impactrecompute::constraintChangeYieldsPotentialImpactAndUnusedIsHonest`
  + `controlledRecomputePreservesHistoryAndIsIdempotent`（新成果
  `ordered_inputs` 的 version_id 是**新**因素版本）。

### X7 受影响图斑漏人工后代（P1-B）
- **问题**：只看直接 `facies_polygons` 子版本；人工确认存在 `edited_facies`
  后代，确认数恒 0。
- **修改**：对每个确定变化的 composed 版本，把变化中心映射到
  `downstreamClosure(vid)` 中所有 `facies_polygons`/`edited_facies` 版本；
  每图元读一次 `paleo_interpretation`（`evidence::decodeInterpretation`）。
  行：`{feature_uid, layer_id, product_version, kind, review_status
  （无解释为 "none"）, interpretation_id}`；5000 行/产品截断 →
  `affected_features_truncated`。保护口径在受影响行上计数
  `{confirmed_features, needs_review_features, review_required（≤500）,
  scope:"affected"}`；`whole_rerun` 对全部后代的全部图元计数
  `scope:"whole_product"`。
- **文件**：`mappingworkbench_impact.cpp:analyzeImpact`/`recomputePlan`；
  `mappingworkbench.h` 常量与 SyncOptions。
- **测试**：`tst_impactrecompute::controlledRecomputePreservesHistoryAndIsIdempotent`
  （Confirmed 编辑图斑进 affected + review_required + protection 计数）。

### X8 反查谓词把历史当现行（P1-B）
- **问题**：`contradicted` 不排除 `unlinked`；`pendingReview` 漏 `Disputed`；
  行不带关联状态。
- **修改**：`contradicted` 加 `unlinked=0`；`pendingReview` 含 `Disputed`；
  行新增 `unlinked`/`relation_state`（active|history）/`is_current_version`/
  `hit_reason`（按查询类型的中文事实说明）；`meta = {index_state,
  index_error, total, shown}`；`stale_sources` 另用
  `distinctSourceVersions` 做目录核验补充（见 X9/已知限制）。
- **文件**：`evidenceindex.cpp:contradicted`/`pendingReview`/
  `countFor`/`distinctSourceVersions`；
  `mappingworkbench_impact.cpp:evidenceReverseQuery`；
  `mappingworkbenchpage.cpp:setReverseResults`（[历史关联]/[旧版本]前缀 +
  meta 行）。
- **测试**：`tst_evidenceindex::contradictedExcludesUnlinkedAndPendingCoversDisputed`；
  `tst_evidencee2e::reverseQueryHonestyAndBackgroundSync`（行字段）。

### X9 索引同步在 UI 线程且吞错（P1-B）
- **问题**：`syncError` 被忽略；首次查询在 UI 线程增量同步。
- **修改**：`Index::synchronize` 新增 `QVector<CatalogVersion>` 重载（DataCatalog
  版本转发），worker 内建本地 `Index`（open→sync→close，连接只活在线程内）；
  `MappingWorkbench::startEvidenceIndexSync` 走 `JobRunner<IndexSyncJob>`；
  commit 在 GUI 线程丢弃过期快照并发 `evidenceIndexSynced`；
  `evidenceReverseQuery` 不再同步阻塞：`index_state` ∈
  `fresh | syncing | stale（触发后台同步后查现有索引）| error`；无任务服务
  （测试/壳）走同步同代码路径；主窗口接 `evidenceIndexSynced` → 页
  `reissueReverseQuery` 自动重发最近一次反查。
- **文件**：`evidenceindex.h/.cpp:Index::synchronize`；
  `mappingworkbench_impact.cpp:startEvidenceIndexSync`/`computeIndexSync`/
  `commitIndexSync`/`evidenceReverseQuery`；
  `paleomainwindow_workbench.cpp`（同步完成→`reissueReverseQuery`）。
- **测试**：`tst_evidencee2e::reverseQueryHonestyAndBackgroundSync`
  （真 `PaleoTaskService`：stale→syncing→信号→fresh；中途 cancel 索引仍可用）。

### X10 反查命中行定位到来源而非命中图斑（P1-C）
- **问题**：命中行双击走证据来源高亮，证据面板仍停在原图斑，且
  `SelectionTarget.evidenceId` 被改成另一图斑的证据——身份混淆；无回退。
- **修改**：`EvidenceInteraction::navigateFeature(hit)`：project/session 与
  层位闸（层位不符 `命中图斑属于层位 %1，请先切换层位`，不静默切换）；
  `layerForVersion(version_id)` 取命中行的**确切版本**（可能是旧版），
  bind 主图层 + `showFeature`；成功置 `SelectionTarget{…, evidenceId}`，
  origin `reverse_index`。回退栈 `m_back`（≤32）：
  `(layerId,versionId,featureUid)` 变化且 origin ∉
  `{refresh,restore,project,horizon,back,evidence}` 时压前一主目标；
  `back()` 弹到当前工程+层位为止；`reset()`/`activeHorizonChanged` 清空；
  `historyChanged(bool)` 驱动页按钮。页侧 `reverseHitActivated` 信号 +
  「返回上一图斑」（objectName `evidenceBack`，`setCanGoBack` 使能）。
- **文件**：`evidenceinteraction.cpp:navigateFeature`/`back`/`canGoBack`/
  `reset`/`onTargetChanged`；`mappingworkbenchpage.h/.cpp`（`evidenceBack`
  + `reverseHitActivated`）；`paleomainwindow_workbench.cpp`（三路接线）。
- **测试**：`tst_mappingworkbench::reverseHitNavigationAndBackRestorePrimaryTarget`
  （验收链 C 全链 + 跨层位拒绝 + reset 清栈）；
  `tst_mappingpages::WorkbenchPageTests::evidenceBackButtonReflectsCanGoBack`。

### X11 图谱把计算事实挂到科学判断上（P1-C）
- **问题**：generation_records/spatial_candidates 作为 `References` 挂在
  Assertion（相类型判断）节点；边无事实出处。
- **修改**：`Edge.provenance` 必填（`edgesMissingProvenance` 调试不变量）；
  覆盖层四分：人工记录→Assertion（角色决定 Supported/Contradicted/
  Constrained/References）；`generation_records`→Feature（`Derived`，
  「生成来源（成果血缘，非科学支持）」）；`spatial_candidates`→Feature
  （`References`，「空间相交候选」）；归因 `inputs` cells>0→输入版本节点
  →Feature（`ConsumedIn`，「计算参与：胜出 N 采样像元」）；解释历史最后
  20 条→`ReviewEvent`→Interpretation（`ReviewedBy`，更早计入 `hidden`）；
  `recompute_provenance.recompute_of`→新版→旧版 `Supersedes`（仅 Lineage
  可穿越，不进 `diagnoseCycles`，标签「受控重算替代（人工解释不迁移）」）；
  `Query.relation`（枚举名过滤覆盖层证据边，参与查询相等/缓存键）。
  页侧：关系类型下拉、kind 文案「审核事件」、关系树/边 tooltip 显示
  `依据：<provenance>`（转义）。
- **文件**：`datagraph.h`（Kind/Relation/Edge.provenance/Query.relation/
  `edgesMissingProvenance`/`relationName`）；`datagraph.cpp:buildIndex`/
  `query`（覆盖层与 traversable 两处 Supersedes 闸）；
  `datagraphworkflow.cpp`（导出新 kinds/relations/query 字段与边 provenance）；
  `datagraphpage.cpp`（关系过滤 + 审核事件文案 + provenance tooltip）。
- **测试**：`tst_evidencegraph::overlayProvenanceTargetsAndReviewBound`/
  `supersedesEdgeIsLineageOnlyWithProvenance`/`relationFilterSelectsOverlayEdgeKinds`。

### X12 索引写失败不回滚、截断不可见（P1-B）
- **问题**：写事务失败路径不 `rollback`；特征/行数上限命中静默。
- **修改**：`writeVersionRows` 每条失败路径 `db.rollback()`；
  `indexed_versions.truncated`（schema v2；v1 检测后整表重建，下一轮全量
  重索引）；`stats().truncated_versions`；`SyncOptions` 可注入上限供测试。
- **文件**：`evidenceindex.h/.cpp`（`SyncOptions`/`writeVersionRows`/schema
  升级/`stats`）。
- **测试**：`tst_evidenceindex::truncationIsRecordedAndRescanned`/
  `schemaUpgradeFromV1Rebuilds`/`failedWriteRollsBackAndStaysUsable`。

### X13 compareVersions 不可达 + 无替代事实（P1-B）
- **问题**：`compareVersions` 只在测试中调用；重算成果不记录替代了哪一版。
- **修改**：`analyzeLayerVersionImpact`/`compareWithRecomputeSource` 移到
  workbench（before=同资产上一版本；无 `recompute_provenance` 如实拒绝）；
  对比 JSON 落 `artifacts/reports/compare-<8hex>.json` 并回 `report_path`；
  `compareVersions` 增栅格 CRS 不一致拒绝；页动作 `compareRecompute`；
  `commitCompose` 把 `params.recompute` 存入成果 extra `recompute_provenance`
  （并从 `algorithm_parameters` 剔除）；`executeRecompute` 幂等续跑
  （`recompute_of`+`source_after_version`+输入版本全匹配→`already_done`）。
- **文件**：`mappingworkbench_impact.cpp:analyzeLayerVersionImpact`/
  `compareWithRecomputeSource`/`executeRecompute`；
  `mappingworkbench_compose.cpp:commitCompose`；
  `paleomainwindow_workbench.cpp`（impactAnalysis/compareRecompute 分发 +
  状态栏口径）；`mappingworkbenchpage.cpp`（按钮行 + 门控）。
- **测试**：`tst_impactrecompute::controlledRecomputePreservesHistoryAndIsIdempotent`
  （二次执行 `already_done` + `compareWithRecomputeSource` 有报告文件）。

### X14 冲突与井不可定位（P1-C）
- **问题**：冲突 finding 与井清单只有纯文本。
- **修改**：井意见 `extra` 带 `{entity_id, x, y, has_coordinates:true}`；
  `evidenceconflict::analyze` 把 `locator` 复制进 `finding.evidence`
  （实体坐标 → `{entity_id,x,y,has_coordinates}`；extra 带 layer/version →
  `{layer_id,version_id}`，version 回退 `op.versionId`）；归因 tab 新增
  `attributionTargets` 列表（井名+坐标；带定位冲突 `[severity] type · label`），
  激活发 `evidenceActivated`（井 `{entity_id,has_coordinates,x,y,horizon}`；
  单因素 `{source_version,horizon}`）；`wells_in_feature_unavailable` 显示。
- **文件**：`mappingevidencereview.cpp:featureEvidence`（井意见 extra）；
  `evidenceconflict.cpp`（`locatorFor` + `analyze` 复制）；
  `mappingworkbenchpage.cpp:renderEvidenceSnapshot`。
- **测试**：`tst_evidenceconflict::opinionLocatorPassesThroughToFindingEvidence`；
  `tst_mappingpages::WorkbenchPageTests::attributionTargetsListAndActivationPayload`。

### X15 计数查询直接拼 UID（P0-A）
- **修改**：`QgsExpression::createFieldEqualityExpression("paleo_feature_uid",
  uid)` 取代字符串拼接。
- **文件**：`mappingworkbench_impact.cpp:analyzeImpact`（保护计数）。
- **测试**：随 `tst_impactrecompute` 保护口径用例覆盖。

### X16 冻结输入解析三处漂移（P0-A）
- **问题**：三份复制实现缺失判定不一致（suffix、存在性检查、文案不同）。
- **修改**：唯一解析器 `resolveFrozenCompose`：`FrozenCompose` 结构承载
  inputs/unresolved/grid；连续输入判定（`factor.*`/`factor_id`/
  `isAnalysisFactorRaster`）+ 文件检查（`resolvedVersionPath + source_suffix`）
  + `raster` 旗标（layer_type 或 .tif/.tiff，缺失时也可知）；缺失原因文案
  带版本 id，三入口同一字符串。
- **文件**：`mappingworkbench.h:FrozenCompose`；
  `mappingevidencereview.cpp:resolveFrozenCompose`。
- **测试**：`tst_composeattribution::missingFrozenInputReportsIdenticalReason`
  （featureEvidence/composeAttributionAt/analyzeImpact 三入口同文案）。

### 转面要素序 flake（P0-A）
- **问题**：`QHash<int,…> byCode` 迭代序随进程随机（该 Qt 构建忽略
  `QT_HASH_SEED`）→ GPKG 要素写入序抖动 → 测试首要素不确定（井落面数
  0/1 翻转）。
- **修改**：两处发射点对 code 键 `std::sort` 后按序输出，同输入同要素序。
- **文件**：`faciespolygonize.cpp`（GDAL 与 noded 两路径 `byCode` 输出处）。
- **测试**：`tst_evidencee2e::scenarioAttributionEvidenceConflictReverseAndReopen`
  修复后 12/12 连跑 + ctest 并行绿。

### 评审补充（P1-B 0a–0c / P1-C 0b）
- **0a**：`analyzeImpact` 的 after 重放传 `FrozenGrid{}`——按替换后输入重建
  网格（新网格定义栅格/新合并范围）；before 仍用旧冻结网格；网格不同
  `diffGrids` 如实降级 potential（`changedInputGridDegradesToPotential`）。
- **0b**：`gridOf` 重建遇目录删除的未知类型输入且位于首个栅格输入之前 →
  `冻结网格无法重建：输入 %1 类型未知（已从目录删除）`；P1-C 另修
  `stale_sources` 文件核验的 `section('|',0,0)` 括号（作用于拼接后全串）。
- **0c**：`evidenceconflict.cpp:opinionsFromAttribution` 跳过 `cells==0` 的
  归因输入（未参与本图斑，不产生 known=false 意见）。

## 4. 核心数据契约（新增/变更字段）

- **成果版本 extra**：`compose_grid{xMin,yMin,xMax,yMax,cols,rows}`；
  `recompute_provenance{recompute_of, impact_report, source_before_version,
  source_after_version, plan_created_at}`（不入 `algorithm_parameters`）。
- **归因重放输出**：`grid_source`（frozen|reconstructed|unavailable）、
  `fidelity`（exact|approximate）、`fidelity_reasons[]`、逐输入
  `alignment`、`inputs[]`（含 0 像元输入）、`unresolved[{layer_id,
  version_id, reason}]`；`replayAt` 增 `cell{col,row,x,y}`、`query_point`。
- **影响报告**：`affected_features[]` 行
  `{feature_uid, layer_id, product_version, kind, review_status,
  interpretation_id}` + `affected_features_truncated`。
- **重算计划**：`plan.binding{…}`（X5）；条目 `inputs[{layer_id,
  frozen_version, current_version, rerun_layer_id}]`、`mode` ∈
  `no_change|blocked_upstream|local_rerun|whole_rerun` +
  `execution_scope` + `reason`；`protection{confirmed_features,
  needs_review_features, review_required[{feature_uid, product_version,
  kind, review_status}], scope:affected|whole_product}`；结果行恒带
  `target_version/mode/ok` + `reason|error`；幂等命中 `already_done` +
  `new_version`。
- **反查行**：新增 `unlinked`、`relation_state`（active|history）、
  `is_current_version`、`hit_reason`；`origin=reverse_index`。
  **`source_version`/`source_feature_uid` 语义变更**：指证据记录登记的
  来源（而非命中图斑自身）；命中图斑身份在 `layer_id`/`version_id`/
  `feature_uid`。
- **反查 meta**：`{index_state, index_error, total, shown}`；
  `index_state` ∈ `fresh|syncing|stale|error`。
- **`evidenceReverseQuery` 签名**：`(name, argument, QString *error,
  QVariantMap *meta=nullptr)`。
- **索引 schema v2**：`indexed_versions.truncated`；`stats().truncated_versions`；
  v1 打开即整表重建。
- **图谱**：`Edge.provenance`；`Kind::ReviewEvent`；
  `Relation::ConsumedIn|ReviewedBy|Supersedes`；`Query.relation`。
- **冲突 finding**：`finding.evidence.locator`（见 X14）。

## 5. 图谱关系语义（relation → 含义 → 事实出处 → 可见模式）

| relation | 含义 | provenance 前缀 | 可穿越/可见 |
|---|---|---|---|
| HasVersion | 资产→版本 | `catalog:asset-version` | 关联/总览（血缘除外） |
| Grouped | 版本→分组 | `catalog:asset-categories` | 按视图分组 |
| Linked | 实体↔版本登记 | `catalog:entity-link` | 关联/实体 |
| Derived | 版本血缘（parentVersionIds 或 evidence inputs 的 DERIVED_FROM）；生成记录→图斑 | `catalog:parentVersionIds` / `catalog:extra.evidence.inputs`；覆盖层 `catalog:extra.evidence#<id>` | lineage()：Lineage/Impact 可穿越 |
| Used | 已登记输入版本→生成活动 | `catalog:extra.evidence.inputs` | lineage() |
| Generated | 生成活动→成果版本 | `catalog:extra.evidence` | lineage() |
| Supported / Contradicted / Constrained / References（版本级） | 解释引用版本边（含 tombstone 「历史已解除」） | `catalog:extra.interpretation_reference_versions` 或 `catalog:extra.evidence.inputs`（角色边） | lineage() |
| EditBaseline / Rollback | 编辑基线/回滚目标（rollbackOf 不计血缘父） | `catalog:extra.editBaseline` / `catalog:extra.rollbackTo` / `catalog:extra.rollbackOf` | lineage()（可成环，不诊断） |
| Materializes | 版本→图斑；解释→图斑 | `snapshot:feature` | 覆盖层（非 Impact） |
| Informs | 相类型判断→解释 | `snapshot:feature` | 覆盖层 |
| Supported / Contradicted / Constrained / References（人工记录） | 证据→科学判断（Assertion） | `gpkg:facies_evidence#<evidence_id>` | 覆盖层；Impact 语义可达 |
| References（空间候选） | 空间候选→图斑 | `spatial:candidate@<source_version>` | 覆盖层 |
| ConsumedIn | 归因输入版本→图斑 | `replay:attribution@<calculation_version>` | 覆盖层，cells>0、depth≥3 |
| ReviewedBy | 审核事件→解释 | `gpkg:paleo_interpretation.history#<event_id>` | 覆盖层，≤20 条 |
| Supersedes | 重算新版本→旧版本 | `catalog:extra.recompute_provenance` | **仅 Lineage**；不进环诊断 |

## 6. 用户交互流程

- **图斑证据链**：地图选图斑 → 证据面板看解释/记录 → 「计算归因」tab 看
  优先级占比 + fidelity 行（`重放与成果网格逐像元同口径`/`近似重放：…`）+
  逐输入决策分布 + 可定位对象（双击井/冲突定位）→ 证据行双击高亮来源
  （主目标不变）→ 「打开证据图谱」看断言/血缘/审核事件 → 底部「反查」
  按类型检索 → 双击命中行把**命中图斑**设为主目标（回退栈入栈）→
  「返回上一图斑」回到原目标 → 复核动作照常走 review 状态机。
- **影响与重算**：编图页选发生版本变化的输入 →「分析所选输入的版本变化
  影响」→ 状态栏三级计数 + 报告路径 →「按最近影响报告受控重算」→
  逐条 `no_change`/`blocked_upstream`/`local_rerun`/`whole_rerun`（原因如实）；
  输入再更新则整计划拒绝（`影响报告已过期`）；重复触发走 `already_done`
  幂等续跑 →「对比重算成果与旧版本」出转移矩阵 + 报告文件。

## 7. 兼容性与迁移

- **旧成果无 `compose_grid`**：`frozenGridFromExtra` 失效 → `gridOf` 按
  composeRaster 口径重建（`grid_source=reconstructed`；不可重建时如实
  `unavailable` + 原因，不静默错网格）。
- **索引 v1**：打开即整表重建为 v2（索引是可重建检索结构，非事实源）。
- **旧计划无 binding**：`executeRecompute` 拒绝并提示重新分析。
- **`evidenceReverseQuery` 签名加 meta**：旧调用按默认 `nullptr` 兼容；
  `stale_sources` 的 `source_version` 语义修正为「证据登记的来源」
  （UI 已同步改为按命中行导航）。
- **`evidence()`/反查行字段**：新增字段为附加键，消费方按名取值不受影响。

## 8. 已知限制（如实声明）

- `stale_sources` 的「版本已不在目录」分支经公共 API 不可达：
  `removeAsset` 被引用完整性拒绝（存在血缘父边时）；该分支只覆盖外部
  改动/损坏目录，已验证文件缺失分支。
- 反查命中行不带 `project_session`；`navigateFeature` 的会话校验依赖
  层位/版本闸（见 TODOS 递延）。
- `indexed_versions.truncated` 命中的版本每轮 sync 全量重扫
  （单版本特征上限有界，200k 行内）。
- 任务模式下 executeRecompute 第二条目仍报「请等待上一条完成」
  （runner 单飞）；幂等续跑需再次触发。
- `acceptsLocalGridCrs` 容忍任意米制 Engineering CRS（为 OGR datum 重命名
  兼容）——非项目网格 CRS 的井层理论上可被纳入 `wells_in_feature`。
- `fidelity=approximate` 属设计语义：重采样/降级路径如实标注，不归一。
- #496 CRS 迁移与层位 `|`/引号转义本轮未重新审计（仅修了 0b 拼接处）。

## 9. 后续可选工作

- 真实多用户身份/RBAC（`localOperator` 是署名非认证）。
- `paleo_interpretation.history` 防篡改哈希链。
- 受控重算多条目排队执行（替代单飞 + 幂等再触发）。
- 截断索引版本的断点续扫。
- #496 CRS 迁移复审；反查行补 `project_session`；`acceptsLocalGridCrs`
  收紧为项目网格精确匹配。
