# 单因素纵深（singlefactor-deep）

分支 `goal/singlefactor-deep-20261015`，基线 `56e458d5`（origin/master，含已合并的 #455）。
已完成 P0（基线）、P1（约束语义统一、QGIS 绘编一致、几何类型/CRS 契约）与 P2（遗留项 + 取数/QC/血缘 + IDW/Kriging 数值诚实）。P3 起另行派发。
未跑的不记为通过；「当前源码」与早期分析冲突时以当前源码为准。

## 四态口径

- **已实现且验证**：有代码，且有本轮或既有自动化测试断言其行为。
- **已实现但有缺陷**：有代码，行为与地质/数值语义不符。
- **部分实现/未接线**：有一部分代码或只覆盖一部分路径。
- **缺失**：没有实现。

括号内「P1 前 → P1 后」；P2 变化记为「→ **P2：…**」。

## 缺口清单（GAP LIST）

### A. 约束语义与方法能力

| # | 能力 | 状态（P1 前 → P1 后） | 证据 / 说明 |
|---|------|----------------------|-------------|
| A1 | 统一约束语义解析（params_json 优先，type 列兜底，blockMode 重分类） | 部分（逻辑只在 `algorithms/singlefactor/constraintparse.cpp`，UI 数字符串）→ **已实现且验证** | `src/domain/constraintsemantics.*`；`tst_singlefactor_constraintuse` |
| A2 | 方法能力判定（逐条 enabled/几何/层位/域/支持/消费/仅制图/原因） | 缺失 → **已实现且验证** | `decideConstraintPart`；`tst_singlefactor_constraintuse` 8 种方法矩阵 |
| A3 | 无效几何不静默参与（零长、自交、非有限坐标） | 部分（只拒 <2 顶点）→ **已实现且验证** | `checkConstraintPart`；constraintparse 逐部件忽略并留因 |
| A4 | 旧 IDW（`paleo_constraint_idw`）语义 | **已实现但有缺陷**（只看 type 列：停用 / display_only / none 的打断线都当硬屏障并参与凸包）→ **已实现且验证** | `tst_singlefactor_constraintreceipts::legacyReceiptsMatchPlan` |
| A5 | 本地方向 IDW / Kriging 引擎回执（QC `constraint_receipts`） | 缺失 → **已实现且验证** | 回执来自引擎实际消费集合（栅格化出格的屏障、resolveParameters 后的方向/软边界）；测试断言 回执 == 计划 |
| A6 | Surfer IDW 回执（有限断层绕行） | 缺失 → **已实现且验证** | 同上 |
| A7 | 地统 kriging/sgs/cokriging 记录「约束未使用」 | 缺失（只在 tooltip 文案）→ 已实现，计划侧验证 → **P2：已实现且验证**（端到端） | `tst_geostat_workflow::krigingQcLineageEndToEnd`：qc.json 与 catalog extra 均逐条 `constraints_unused`、零消费 |
| A8 | 血缘 extra `constraints_consumed/unused` | 缺失 → 部分 → **P2：已实现且验证**：旧 IDW / 距离变换因子共用 `insertConstraintReceiptExtras`；曲面网格化写掩膜回执 | `tst_geostat_workflow::idwLineageCarriesReceipts`（完全阻断=hard_barrier 消费，display_only 未用） |
| A9 | 结构 IDW 引擎回执 | 缺失 → 部分 → **P2：已实现且验证**。**更正 P1 分析**：当前源码的结构面模型是 `contour_only`——完全阻断与 display_only 都只作解释软边界（强度>0 且启用屏障时），没有硬隔断；计划已按当前源码改正 | `structuralReceiptsMatchPlan`；另修真缺陷：停用的解释边界此前仍进趋势权重（`structuralDisabledSoftBoundaryHasNoEffect`，回退即失败） |
| A10 | 约束页「可用 / 参与」计数与就绪门 | **已实现但有缺陷**（数 type 字符串：display_only 打断线、零长线都计为可用屏障）→ **已实现**（统一计划驱动，逐条状态 + 当前方法参与数）；无专门 UI 断言 | `ConstraintPage::applyConstraintUsePlan` |
| A11 | 距离变换 / 最小曲率引擎 | **已实现但有缺陷**（只认 type=break_line，display_only/停用也隔断）→ **P2：已实现且验证** | `algorithms/constraintbarriers` 逐部件统一判定，只取完全阻断；输出 `CONSTRAINT_RECEIPTS`。`griddingDisplayOnlyDoesNotCut`：display_only/停用与无约束逐位一致、完全阻断对照改变结果（回退即失败）；曲面网格化掩膜同口径 |
| A13 | 地统输入取用诚实（给了但没读的参数） | 缺失（块金在自动拟合下被忽略、搜索半径/最少点数静默不读、ρ=0 的协克里金仍标协克里金）→ **P2：已实现且验证** | QC/血缘 `ignored_inputs`、`model_provenance`、`search`、`cokriging`（ρ 来源/钳制/共位样本/协变量是否生效）；`searchRadius` 现真正传入内核（`searchRadiusIsHonoured`） |
| A14 | 交叉验证 | 缺失 → **P2：已实现且验证**（克里金/协克里金 LOO，剔除重合样本；>2000 井按步长抽检并记录）；SGS 如实标 `not_computed` | `cross_validation`：ME/RMSE/MAE/标准化平方误差；端到端测试断言每井都估到、RMSE < 样本标准差 |
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
| B8 | 新绘制要素纳入同一撤销栈 | 部分 → **P2：已实现且验证（会话内）** | 编辑会话活跃时绘制走 `beginEditCommand/addFeature`，undo/redo/取消/保存与 store 一致；会话外绘制仍直写 store（不进撤销栈，见待决） |

### C. 几何类型 / CRS / 刷新

| # | 能力 | 状态（P1 前 → P1 后） | 证据 / 说明 |
|---|------|----------------------|-------------|
| C1 | 库层 wkbUnknown + 无 SRS | 现状如实记录：GPKG 层建库 wkbUnknown、无 SRS；QGIS OGR 对纯线库报 `LineString` | `tst_singlefactor_constraintcrs::storeLayerGetsLocalGridCrs`（QINFO 实测）。不迁移 GPKG schema |
| C2 | 读出图层 CRS | **已实现但有缺陷**（无 CRS，配准工程画布按原数值当画布坐标显示）→ **已实现且验证** | `QgisLayerService::instantiate` / `adoptProjectLayers` 补本地工程网格 CRS |
| C3 | 绘制入库 CRS | **已实现但有缺陷**（画布坐标原样写库，配准工程混 CRS）→ **已实现且验证** | 两个绘制控制器经 `constraintcrs::toStoreWkt` 显式变换；无坐标操作时拒绝 |
| C4 | 编辑会话换源保 CRS | 部分（依赖 provider）→ **已实现且验证** | `editSessionKeepsCrs` |
| C5 | 导入 CRS 检查 | 已实现（`acceptsLocalGrid`）；拒绝路径此前无测试 → **已实现且验证** | `tst_mappingworkbench::constraintImportRejectsForeignCrs`（新增）；多要素导入 `constraintImportLocalCrsAndVerbatimRoundTrip`（既有，8 条） |
| C6 | 投影单位诊断 | 部分（地理 CRS 只推警告）→ **P2 决定：保持警告** | 理由：工作流路径只有本地工程网格（导入门拒绝外部 CRS，约束与井点跨 CRS 无坐标操作时直接失败，实测）；改拒绝会破坏 Processing 公共契约与 `tst_algorithm_harness` 既有断言 |
| C7 | 混合几何（点/线/面同层位） | 部分 → **P2 已调查**：OGR 按**首要素**给图层几何类型（线在前 LineString、点在前 Point），但要素数始终 == store 行数；线解析器把非线逐条记 `not_line` | `mixedPointLineHorizonProbe`（两种顺序）。渲染/工具按单一类型选择的后果未改，转 P3 |
| C8 | 声明/刷新一致 | 已实现（loadConstraints/addConstraint 同 layerId 幂等声明；会话中不重声明） | 既有 `tst_constraint_draw::fiveSemanticEntriesDrawAndRoundTrip` |

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

- **会话外绘制**：未进入编辑会话时绘制仍直接写 store、不进撤销栈。若要一律可撤销，需绘制前自动开会话（改变交互：绘制即进入编辑态），等用户决定。
- **切层位 / releaseHorizon**：显式回滚未保存编辑（B7 只改了「图层被移除」路径）。是否也改为保留，等用户决定。
- **混合几何**：图层几何类型随首要素变化，影响渲染器与捕获工具选择；需要按几何类型分层声明（不迁移 GPKG），转 P3。
