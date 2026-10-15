# 单因素纵深（singlefactor-deep）

分支 `goal/singlefactor-deep-20261015`，基线 `56e458d5`（origin/master，含已合并的 #455）。
已完成 P0（基线）、P1（约束语义统一、QGIS 绘编一致、几何类型/CRS 契约）、P2（遗留项 + 取数/QC/血缘 + IDW/Kriging 数值诚实）、P3（栅格/等值线/样式/成果血缘显示、混合几何、距离变换/最小曲率逐线回执、绘制自动开会话）与 P4（收尾：相界铺设防丢编辑、会话路由与视图重同步、逐版本色带、性能、验收步骤）。
未跑的不记为通过；「当前源码」与早期分析冲突时以当前源码为准。

## 四态口径

- **已实现且验证**：有代码，且有本轮或既有自动化测试断言其行为。
- **已实现但有缺陷**：有代码，行为与地质/数值语义不符。
- **部分实现/未接线**：有一部分代码或只覆盖一部分路径。
- **缺失**：没有实现。

括号内「P1 前 → P1 后」；P2 变化记为「→ **P2：…**」，P3 记为「→ **P3：…**」，P4 记为「→ **P4：…**」。

## 缺口清单（GAP LIST）

### A. 约束语义与方法能力

| # | 能力 | 状态（P1 前 → P1 后） | 证据 / 说明 |
|---|------|----------------------|-------------|
| A1 | 统一约束语义解析（params_json 优先，type 列兜底，blockMode 重分类） | 部分（逻辑只在 `algorithms/singlefactor/constraintparse.cpp`，UI 数字符串）→ **已实现且验证** | `src/domain/constraintsemantics.*`；`tst_singlefactor_constraintuse` |
| A2 | 方法能力判定（逐条 enabled/几何/层位/域/支持/消费/仅制图/原因） | 缺失 → **已实现且验证** | `decideConstraintPart`；`tst_singlefactor_constraintuse` 8 种方法矩阵 |
| A3 | 无效几何不静默参与（零长、自交、非有限坐标） | 部分（只拒 <2 顶点）→ **已实现且验证** | `checkConstraintPart`；constraintparse 逐部件忽略并留因 ；**P4：性能**——自交判定由全段对 O(n²) 改为长轴扫描剪枝（判定不变），3×5 万顶点 24.9 s → 21 ms（`selfIntersectionLargeLinesAreFast`，与改造前逐例等价 `selfIntersectionSweepMatchesBruteForce`） |
| A4 | 旧 IDW（`paleo_constraint_idw`）语义 | **已实现但有缺陷**（只看 type 列：停用 / display_only / none 的打断线都当硬屏障并参与凸包）→ **已实现且验证** | `tst_singlefactor_constraintreceipts::legacyReceiptsMatchPlan` |
| A5 | 本地方向 IDW / Kriging 引擎回执（QC `constraint_receipts`） | 缺失 → **已实现且验证** | 回执来自引擎实际消费集合（栅格化出格的屏障、resolveParameters 后的方向/软边界）；测试断言 回执 == 计划 |
| A6 | Surfer IDW 回执（有限断层绕行） | 缺失 → **已实现且验证** | 同上 |
| A7 | 地统 kriging/sgs/cokriging 记录「约束未使用」 | 缺失（只在 tooltip 文案）→ 已实现，计划侧验证 → **P2：已实现且验证**（端到端） | `tst_geostat_workflow::krigingQcLineageEndToEnd`：qc.json 与 catalog extra 均逐条 `constraints_unused`、零消费 |
| A8 | 血缘 extra `constraints_consumed/unused` | 缺失 → 部分 → **P2：已实现且验证**：旧 IDW / 距离变换因子共用 `insertConstraintReceiptExtras`；曲面网格化写掩膜回执 | `tst_geostat_workflow::idwLineageCarriesReceipts`（完全阻断=hard_barrier 消费，display_only 未用） |
| A9 | 结构 IDW 引擎回执 | 缺失 → 部分 → **P2：已实现且验证**。**更正 P1 分析**：当前源码的结构面模型是 `contour_only`——完全阻断与 display_only 都只作解释软边界（强度>0 且启用屏障时），没有硬隔断；计划已按当前源码改正 | `structuralReceiptsMatchPlan`；另修真缺陷：停用的解释边界此前仍进趋势权重（`structuralDisabledSoftBoundaryHasNoEffect`，回退即失败） |
| A10 | 约束页「可用 / 参与」计数与就绪门 | **已实现但有缺陷**（数 type 字符串：display_only 打断线、零长线都计为可用屏障）→ **已实现**（统一计划驱动，逐条状态 + 当前方法参与数）；无专门 UI 断言 | `ConstraintPage::applyConstraintUsePlan` |
| A11 | 距离变换 / 最小曲率引擎 | **已实现但有缺陷**（只认 type=break_line，display_only/停用也隔断）→ **P2：已实现且验证** | `algorithms/constraintbarriers` 逐部件统一判定，只取完全阻断；输出 `CONSTRAINT_RECEIPTS`。`griddingDisplayOnlyDoesNotCut`：display_only/停用与无约束逐位一致、完全阻断对照改变结果（回退即失败）；曲面网格化掩膜同口径 | → **P3：逐线落格回执**：未落入计算域的完全阻断线记 `consumed=false / outside_compute_domain`，`PALEO_BREAK_LINES`=实际落格条数；全不落格时距离变换走精确欧氏路径（与无约束逐位一致）；计划侧同按域判定 | `griddingReceiptsMatchPlan`（回执 == 带域计划；栅格外屏障输出 == 无约束；回退 3 处失败）
| A13 | 地统输入取用诚实（给了但没读的参数） | 缺失（块金在自动拟合下被忽略、搜索半径/最少点数静默不读、ρ=0 的协克里金仍标协克里金）→ **P2：已实现且验证** | QC/血缘 `ignored_inputs`、`model_provenance`、`search`、`cokriging`（ρ 来源/钳制/共位样本/协变量是否生效）；`searchRadius` 现真正传入内核（`searchRadiusIsHonoured`） |
| A14 | 交叉验证 | 缺失 → **P2：已实现且验证**（克里金/协克里金 LOO，剔除重合样本；>2000 井按步长抽检并记录）；SGS 如实标 `not_computed` | `cross_validation`：ME/RMSE/MAE/标准化平方误差；端到端测试断言每井都估到、RMSE < 样本标准差 ；**P4**：LOO 抽到 `algorithms/geostat/crossvalidation`（实现不变），性能门 `tst_geostat_perf::crossValidationCapBoundsCost`：2000 井 460 ms、8000 井（stride 4）1853 ms，比率 4.03 ≤ 6 |
| A15 | 取数 QC：IDW 逐井参与/跳过 | 缺失（静默跳过非数值）→ **P2：已实现且验证** | `SAMPLE_RECEIPTS` → 血缘 `engine_samples_used/skipped`（原因码），与证据记录的 supporting/skipped_wells 分层保留 |
| A12 | 方法参数越界（directionRatio∉[1,100] / softStrength∉[0,0.8]）提前提示 | 缺失 → **已实现**：计划标 `blocking`，UI 提示「运行将被拒绝」 | 引擎仍整次拒绝（未改） |

### B. QGIS 原生绘编一致

| # | 能力 | 状态 | 证据 / 说明 |
|---|------|------|-------------|
| B1 | 点击/吸附/预览/Enter·双击完成/Esc 取消 | 已实现且验证（既有） | `tst_constraintediting::captureEnterDoubleClickAndShapePreview`、`snapToleranceAndEditingExitDialog`、`tst_constraint_draw::snappingActiveDuringCapture` |
| B2 | Backspace 删节点（顶点编辑） | 已实现且验证（既有） | `tst_edittools`（Backspace 路径）；**捕获中 Backspace 回退顶点走 QgsMapToolCapture 原生，未自动化 → 手工** |
| B3 | 捕获中平移 / CAD 约束输入 | 已实现（原生 QgsMapToolCapture + CAD dock）；**仅手工可验** | 无头环境无法驱动 CAD dock 交互 |
| B4 | 移动/插入/删除顶点、撤销/重做、保存、取消、写失败回滚 | 已实现且验证（既有） | `vertexRoundTripUndoRedoSave`、`vertexGesturesPersistInsertDeleteDrag`、`deleteUndoAndCancelRestoresSnapshot`、`failedWriteRevertsBuffer`、`fullStackWriteFailureAndBranching` |
| B5 | 编辑缓冲 ↔ GPKG 一致 | 已实现且验证（既有） | `Fixture::consistent()` 逐字段比对 |
| B6 | 编辑中保存 .qgz 不持久化临时数据源 | 已实现且验证（既有） | `batchSemanticUndoAndNextParser`（writeMapLayer 钩子改写回正式路径） |
| B7 | 编辑中移除图层 | 旧契约「回到会话开始」→ **P2（用户决定）：保留未保存的编辑**，已实现且验证 | `removedLayerPreservesEditsAndMissingAssetRefuses`（原断言随契约改）、`removedLayerPreservesUndoPositionAndDrawnConstraint`；服务改发 `editCommitted`。`releaseHorizon`/切层位的显式回滚路径未改 |
| B8 | 新绘制要素纳入同一撤销栈 | 部分 → P2：已实现且验证（会话内）→ **P3（用户决定 (a)）：已实现且验证（全部绘制）** | 画布绘制统一走 `ConstraintWorkflow::drawConstraint`：无会话时先自动开约束编辑会话（UI 经编辑工具栏 `beginConstraintEdit`，接管保存/取消；无 UI 时经绑定的 `QgisEditingService`），再经会话写入；开不起会话则如实失败、不写 store。会话外直写绘制路径已移除（导入仍走事务化 addConstraint）。`drawAutoStartsEditSession`：自动开会话、首笔可撤销/重做、复用会话、保存保留、取消恢复、启动器拒绝不写、无服务失败（回退即失败） |

| B9 | 相界工作副本铺设（releaseHorizon 整组释放）不丢未保存约束编辑 | **已实现但有缺陷**（P3 决定 a+b 叠加：自动会话绘制会被静默回滚）→ **P4：已实现且验证** | `QgisLayerService::isEditingInHorizon`；`prepareFaciesForEditing` 在任何副作用前拒绝并说明原因（releaseHorizon 回滚语义未改，决定 b）；壳侧状态栏显示原因、不自动进编辑。`composePrepareRefusesWhileConstraintSessionEditing`（拒绝无副作用、保存后放行，回退即失败） |
| B10 | 删除 / 语义改型可撤销 | 部分（无会话时直写 store）→ **P4：已实现且验证** | 有 UI 启动器或已绑定编辑服务时先 `ensureConstraintSession`，经会话写入；裸用路径（无会话来源）维持事务化直写。`sessionRoutesRemoveUpdateAndResyncsViews`（回退即失败） |

### C. 几何类型 / CRS / 刷新

| # | 能力 | 状态（P1 前 → P1 后） | 证据 / 说明 |
|---|------|----------------------|-------------|
| C1 | 库层 wkbUnknown + 无 SRS | 现状如实记录：GPKG 层建库 wkbUnknown、无 SRS；QGIS OGR 对纯线库报 `LineString` | `tst_singlefactor_constraintcrs::storeLayerGetsLocalGridCrs`（QINFO 实测）。不迁移 GPKG schema |
| C2 | 读出图层 CRS | **已实现但有缺陷**（无 CRS，配准工程画布按原数值当画布坐标显示）→ **已实现且验证** | `QgisLayerService::instantiate` / `adoptProjectLayers` 补本地工程网格 CRS |
| C3 | 绘制入库 CRS | **已实现但有缺陷**（画布坐标原样写库，配准工程混 CRS）→ **已实现且验证** | 两个绘制控制器经 `constraintcrs::toStoreWkt` 显式变换；无坐标操作时拒绝 |
| C4 | 编辑会话换源保 CRS | 部分（依赖 provider）→ **已实现且验证** | `editSessionKeepsCrs` |
| C5 | 导入 CRS 检查 | 已实现（`acceptsLocalGrid`）；拒绝路径此前无测试 → **已实现且验证** | `tst_mappingworkbench::constraintImportRejectsForeignCrs`（新增）；多要素导入 `constraintImportLocalCrsAndVerbatimRoundTrip`（既有，8 条） |
| C6 | 投影单位诊断 | 部分（地理 CRS 只推警告）→ **P2 决定：保持警告** | 理由：工作流路径只有本地工程网格（导入门拒绝外部 CRS，约束与井点跨 CRS 无坐标操作时直接失败，实测）；改拒绝会破坏 Processing 公共契约与 `tst_algorithm_harness` 既有断言 |
| C7 | 混合几何（点/线/面同层位） | 部分 → P2 已调查 → **P3：已实现且验证**（不迁移 GPKG） | 主图层不过滤（编辑会话要求覆盖全部行、合成取全部要素）；对「存在但主图层类型不覆盖」的几何类型声明只读伴随视图 `constraintgeom.<层位>.<point/line/polygon>`（OGR `geometrytype=`），类型消失即撤销；store 写入、会话内绘制、loadConstraints 后同步。`mixedGeometryViewsPerType`（线在前/点在前）+ `drawAutoStartsEditSession`（会话内画面即声明面视图），回退即失败 ；**P4**：会话保存/取消（editCommitted/editRolledBack）与删除后重同步视图（`sessionRoutesRemoveUpdateAndResyncsViews`，回退即失败） |
| C8 | 声明/刷新一致 | 已实现（loadConstraints/addConstraint 同 layerId 幂等声明；会话中不重声明） | 既有 `tst_constraint_draw::fiveSemanticEntriesDrawAndRoundTrip` |

### D. 栅格成果 / 等值线 / 样式 / 成果版本（P3）

| # | 能力 | 状态 | 证据 / 说明 |
|---|------|------|-------------|
| D1 | 等间距等值线忽略 nodata 洞 | 怀疑缺陷 → **P3：已验证无缺陷**（GDAL `GDALContourGenerateEx` 按波段 nodata 处理，box GDAL 3.10 实测），补回归护栏 | `tst_singlefactor_contours::intervalContoursIgnoreNodata` |
| D2 | 因素色带样式（QgisStyleService / factorstylewriter） | **已实现但有缺陷**（全 nodata 栅格建不起渲染器时仍写出默认灰度 qml 并报成功）→ **P3：已实现且验证**：如实失败 | `styleWriterPresets`（回退即失败） ；**P4：逐版本色带**——共用 `factor_<id>.qml` 照写（兼容），另存 `factor_<id>_v<路径摘要>.qml`，成果声明 styleRef 指向本版本（styleRef 本为自由字符串，无迁移）。`versionedStyleKeepsPerVersionRange`（两版值域各自保留，回退即失败）。画布显示经 applyTo 按本层统计着色，原本不受共用文件影响 |
| D3 | 成果版本血缘显示 | 部分（结构面成果 extra 无约束回执；成果表不显示来源）→ **P3：已实现且验证** | 结构面发布写 `insertQcConstraintReceiptExtras`；`ConstraintWorkflow::factorResultLineage` 给 summary/details，`savedFactorResults` 带 `lineage`；成果表第 4 列「来源与质控」+ 明细 tooltip。`factorResultLineageSummary`、`tst_factorworkflow` 结构面 extra 断言（回退即失败） |
| D4 | UI 工作流（任务 F/G） | 部分 → P3：已实现 → **P4：已实现且验证** | `tst_mappingpages::factorResultsTableShowsLineageColumn`：真实 catalog 版本 → 表头、摘要 == factorResultLineage summary、tooltip == details |

## P4 验收清单

- [x] 相界铺设在约束层有未保存编辑时拒绝（releaseHorizon 回滚语义不改，决定 b）
- [x] 会话保存/取消/删除后伴随视图重同步；删除/语义改型走会话
- [x] 逐版本色带（无 schema 迁移）
- [x] 成果表「来源与质控」列 UI 断言
- [x] 自交判定 O(n²) → 扫描剪枝；LOO 2000 井上限性能门
- [x] 无头验收步骤逐项运行（下表，脚本 `sfdeep-tools/acceptance.sh`，结果 `sfdeep-p4-acceptance.tsv`）
- [x] 自审两轮，High/Medium 为 0

### 验收步骤（P4 末端 HEAD，真实运行）

说明：原始规格的验收文本在本工作区不可得；下表按本文件 P1–P3 验收清单（据规格整理）逐项映射到可无头运行的用例并实跑，另列不可运行项与原因。

| 步骤 | 内容 | 结果 | 用例 |
|------|------|------|------|
| S1 | 方向线+本地方向IDW/Kriging→consumed，引擎回执==计划 | 通过 | tst_singlefactor_constraintreceipts::localDirectionReceiptsMatchPlan |
| S2 | display_only 不是硬屏障（8 方法+旧IDW） | 通过 | tst_singlefactor_constraintuse::displayOnlyIsNotHardBarrier |
| S3 | 停用约束无数值作用 | 通过 | tst_singlefactor_constraintuse::disabledHasNoNumericEffect |
| S4 | 地统 Kriging 逐条标约束未使用（端到端 QC/血缘） | 通过 | tst_geostat_workflow::krigingQcLineageEndToEnd |
| S5 | 无效几何（<2顶点/零长/自交/非有限）不静默使用 | 通过 | tst_singlefactor_constraintuse::invalidGeometryNeverUsed |
| S6 | 配准画布绘制→库存本地坐标；无坐标操作的外部画布拒绝；会话保CRS | 通过 | tst_singlefactor_constraintcrs::georeferencedCanvasDrawStoresLocalCoordinates<br>tst_singlefactor_constraintcrs::foreignCanvasWithoutOperationRefuses<br>tst_singlefactor_constraintcrs::editSessionKeepsCrs |
| S7 | 外部 CRS 导入拒绝 | 通过 | tst_mappingworkbench::constraintImportRejectsForeignCrs |
| S8 | 结构面 IDW 回执==计划；停用软边界无作用 | 通过 | tst_singlefactor_constraintreceipts::structuralReceiptsMatchPlan<br>tst_singlefactor_constraintreceipts::structuralDisabledSoftBoundaryHasNoEffect |
| S9 | Surfer/旧IDW 回执==计划 | 通过 | tst_singlefactor_constraintreceipts::surferReceiptsMatchPlan<br>tst_singlefactor_constraintreceipts::legacyReceiptsMatchPlan |
| S10 | 距离变换/最小曲率：display_only 不隔断；逐线落格回执 | 通过 | tst_singlefactor_constraintreceipts::griddingDisplayOnlyDoesNotCut<br>tst_singlefactor_constraintreceipts::griddingReceiptsMatchPlan |
| S11 | 绘制自动开会话、每次绘制可撤销/保存/取消（决定a） | 通过 | tst_constraintediting::drawAutoStartsEditSession<br>tst_constraintediting::drawnConstraintJoinsUndoStack |
| S12 | 删除/改型走会话；会话结束后伴随视图重同步 | 通过 | tst_constraintediting::sessionRoutesRemoveUpdateAndResyncsViews |
| S13 | 相界铺设在约束未保存编辑时拒绝（不经releaseHorizon丢编辑） | 通过 | tst_constraintediting::composePrepareRefusesWhileConstraintSessionEditing |
| S14 | 移除图层保留编辑（B7 用户决定） | 通过 | tst_constraintediting::removedLayerPreservesEditsAndMissingAssetRefuses<br>tst_constraintediting::removedLayerPreservesUndoPositionAndDrawnConstraint |
| S15 | 混合几何按类型声明（无GPKG迁移） | 通过 | tst_constraintediting::mixedGeometryViewsPerType<br>tst_constraintediting::mixedPointLineHorizonProbe |
| S16 | 顶点编辑/撤销重做/保存/取消/写失败回滚 | 通过 | tst_constraintediting::vertexRoundTripUndoRedoSave<br>tst_constraintediting::deleteUndoAndCancelRestoresSnapshot<br>tst_constraintediting::failedWriteRevertsBuffer |
| S17 | 捕获 Enter/双击完成、预览、吸附 | 通过 | tst_constraintediting::captureEnterDoubleClickAndShapePreview<br>tst_constraintediting::snapToleranceAndEditingExitDialog |
| S18 | 等值线 nodata 洞 | 通过 | tst_singlefactor_contours::contoursDoNotCrossANodataHole<br>tst_singlefactor_contours::intervalContoursIgnoreNodata |
| S19 | 色带样式如实失败、逐版本范围 | 通过 | tst_factorworkflow::styleWriterPresets<br>tst_factorworkflow::versionedStyleKeepsPerVersionRange |
| S20 | 成果血缘摘要/结构面回执进血缘 | 通过 | tst_factorworkflow::factorResultLineageSummary |
| S21 | 成果表「来源与质控」列（UI） | 通过 | tst_mappingpages::factorResultsTableShowsLineageColumn |
| S22 | 地统输入诚实：搜索半径生效；ρ 来源 | 通过 | tst_geostat_workflow::searchRadiusIsHonoured<br>tst_geostat_workflow::cokrigingRhoProvenance |
| S23 | IDW 逐井取样回执 | 通过 | tst_singlefactor_constraintreceipts::idwSampleReceipts |
| S24 | 自交判定性能（3×5万顶点）与等价性 | 通过 | tst_singlefactor_constraintuse::selfIntersectionLargeLinesAreFast<br>tst_singlefactor_constraintuse::selfIntersectionSweepMatchesBruteForce |
| S25 | LOO 2000 井上限性能门 | 通过 | tst_geostat_perf::crossValidationCapBoundsCost |
| M1 | 捕获中 Backspace 回退顶点 | 不可无头运行 | QgsMapToolCapture 原生键盘处理，需真实桌面会话；手工验收 |
| M2 | 捕获中平移 / CAD dock 约束输入 | 不可无头运行 | CAD dock 交互无法在 offscreen 驱动；手工验收 |
| M3 | 真实工区（PALEO_REAL_PROJECT_AREA） | 不可运行 | 盒子无真实工区数据；`realAreaDoesNotInventWellValues`、`tst_geostat_realarea` 如实 SKIP |
| M4 | Windows 构建 | 未在本地运行 | 盒子为 Linux；CI windows 任务每次被并发组取消，无结果 |

## P3 验收清单

- [x] CI 38730511095：lint 通过；linux/asan/perf 失败为 runner「No space left on device」（基础设施，非本分支）；windows 被并发组取消
- [x] 距离变换/最小曲率逐线落格回执（P2 Low 已解决）
- [x] 混合几何按类型声明（无 GPKG 迁移）
- [x] 用户决定 (a)：绘制自动开会话（P2 Low「会话外绘制不可撤销」已解决）
- [x] 用户决定 (b)：releaseHorizon 维持显式回滚（未改）
- [x] 等值线 nodata、样式失败如实、成果血缘显示
- [x] 自审两轮，High/Medium 为 0
- [x] 全量 ctest：399/407，失败 8 个 = 基线 8 个同名

## P2 验收清单

- [x] 结构 IDW 引擎回执 == 计划（contour_only 口径）；停用解释边界无数值作用
- [x] 距离变换 / 最小曲率：display_only/停用不隔断（逐位比对），回执 == 计划
- [x] 曲面网格化掩膜走统一计划，血缘写消费/未用
- [x] 会话内新绘约束进撤销栈（含非线约束；其他层位直写不撞 fid）
- [x] B7：编辑中移除图层保留未保存编辑（用户决定）
- [x] 地统 QC/血缘端到端（约束未用、被忽略输入、模型来源、LOO、ρ 来源）
- [x] 搜索半径/最少点数不再被静默忽略
- [x] IDW 逐井取样回执进血缘
- [x] 混合几何调查、地理 CRS 决定（警告）

## P1 验收清单

- [x] 方向线 + 本地方向 IDW → consumed（引擎回执 == 计划）
- [x] 方向线 + 本地方向 Kriging → consumed（引擎回执 == 计划）
- [x] display_only 打断线不是硬屏障（8 种方法均无 hard_barrier 角色；旧 IDW 不再隔断）
- [x] 停用约束无数值作用（8 种方法）
- [x] 地统 Kriging → 约束逐条标 `method_does_not_read_line_constraints`
- [x] 无效几何（<2 顶点/零长/自交/非有限）绝不静默使用
- [x] 绘制在配准画布 → 库内存本地坐标；重开按显式坐标操作回原位
- [x] 无坐标操作的外部画布 / 外部 CRS 导入 → 拒绝
- [x] 编辑会话换源不丢 CRS
- [x] 结构 IDW 引擎回执（P2 完成）
- [x] 新绘制进撤销栈（P2 完成，会话内）
- [x] 编辑中移除图层：用户决定保留编辑（P2 完成）

## 待决

- ~~**会话外绘制**~~：P3 按用户决定 (a) 解决——绘制前自动开会话。连带后果：(b) 维持 releaseHorizon 显式回滚，故合成等直接调用 releaseHorizon 的路径会回滚未保存的自动会话绘制（UI 切层位入口在编辑中拒绝切换，不受影响）。 **P4 已解决**：直接调用 releaseHorizon 的相界铺设路径现在在约束层有未保存编辑时拒绝（B9）。
- **切层位 / releaseHorizon**：显式回滚未保存编辑（B7 只改了「图层被移除」路径）。是否也改为保留，等用户决定。
- ~~**混合几何**~~：P3 已按类型声明只读伴随视图解决（见 C7）。
