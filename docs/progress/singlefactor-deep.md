# 单因素纵深（singlefactor-deep）

分支 `goal/singlefactor-deep-20261015`，基线 `56e458d5`（origin/master，含已合并的 #455）。
本轮只做 P0（基线）与 P1（约束语义统一、QGIS 绘编一致、几何类型/CRS 契约）。P2–P4 另行派发。
未跑的不记为通过；「当前源码」与早期分析冲突时以当前源码为准。

## 四态口径

- **已实现且验证**：有代码，且有本轮或既有自动化测试断言其行为。
- **已实现但有缺陷**：有代码，行为与地质/数值语义不符。
- **部分实现/未接线**：有一部分代码或只覆盖一部分路径。
- **缺失**：没有实现。

括号内「P1 前 → P1 后」。

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
| A7 | 地统 kriging/sgs/cokriging 记录「约束未使用」 | 缺失（只在 tooltip 文案）→ **已实现，计划侧验证**；QC/血缘写入路径未单测 | `GeostatJob::constraintsUnused` → QC `constraints_unused` + 血缘 extra |
| A8 | 血缘 extra `constraints_consumed/unused` | 缺失（只有布尔 `constrained`）→ **部分**：本地方向/Surfer（读 QC 回执）与旧 IDW（读 `CONSTRAINT_RECEIPTS`）已写；无端到端发布测试 | `constraintfactorjobs_localdirection.cpp`、`_factor.cpp`、`_geostat.cpp` |
| A9 | 结构 IDW 引擎回执 | 缺失 → **部分**：计划规则已建（完全阻断=硬隔断、display_only=解释软边界、方向线裁到成图域），引擎侧回执未做 | P2 |
| A10 | 约束页「可用 / 参与」计数与就绪门 | **已实现但有缺陷**（数 type 字符串：display_only 打断线、零长线都计为可用屏障）→ **已实现**（统一计划驱动，逐条状态 + 当前方法参与数）；无专门 UI 断言 | `ConstraintPage::applyConstraintUsePlan` |
| A11 | 距离变换 / 最小曲率引擎（非单因素策略包） | **已实现但有缺陷**（只认 type=break_line，display_only/停用也隔断） | 不在本轮范围，P2 |
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
| B7 | 编辑中移除图层 | 已实现且验证（既有）——**契约是「回到会话开始」**（丢弃未保存编辑） | `removedLayerRestoresStoreAndMissingAssetRefuses`；本方向要求与此相反，见「待决」 |
| B8 | 新绘制要素纳入同一撤销栈 | 部分（绘制直接入库，不进编辑撤销栈） | P2 |

### C. 几何类型 / CRS / 刷新

| # | 能力 | 状态（P1 前 → P1 后） | 证据 / 说明 |
|---|------|----------------------|-------------|
| C1 | 库层 wkbUnknown + 无 SRS | 现状如实记录：GPKG 层建库 wkbUnknown、无 SRS；QGIS OGR 对纯线库报 `LineString` | `tst_singlefactor_constraintcrs::storeLayerGetsLocalGridCrs`（QINFO 实测）。不迁移 GPKG schema |
| C2 | 读出图层 CRS | **已实现但有缺陷**（无 CRS，配准工程画布按原数值当画布坐标显示）→ **已实现且验证** | `QgisLayerService::instantiate` / `adoptProjectLayers` 补本地工程网格 CRS |
| C3 | 绘制入库 CRS | **已实现但有缺陷**（画布坐标原样写库，配准工程混 CRS）→ **已实现且验证** | 两个绘制控制器经 `constraintcrs::toStoreWkt` 显式变换；无坐标操作时拒绝 |
| C4 | 编辑会话换源保 CRS | 部分（依赖 provider）→ **已实现且验证** | `editSessionKeepsCrs` |
| C5 | 导入 CRS 检查 | 已实现（`acceptsLocalGrid`）；拒绝路径此前无测试 → **已实现且验证** | `tst_mappingworkbench::constraintImportRejectsForeignCrs`（新增）；多要素导入 `constraintImportLocalCrsAndVerbatimRoundTrip`（既有，8 条） |
| C6 | 投影单位诊断 | 部分（地理 CRS 只推警告） | 不改；P2 评估是否升级为拒绝 |
| C7 | 混合几何（点约束与线同层） | 部分：库层 wkbUnknown 混存点/线时 QGIS 只按一种类型暴露 | P2 调查（不在 P1 改 schema） |
| C8 | 声明/刷新一致 | 已实现（loadConstraints/addConstraint 同 layerId 幂等声明；会话中不重声明） | 既有 `tst_constraint_draw::fiveSemanticEntriesDrawAndRoundTrip` |

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
- [ ] 结构 IDW 引擎回执（P2）
- [ ] 新绘制进撤销栈（P2）
- [ ] 编辑中移除图层 = 保存还是丢弃（待用户决定）

## 待决

- **编辑中移除图层**：现契约（既有测试断言）是恢复到会话开始（丢弃未保存编辑）。方向要求改成相反行为会推翻既有断言，本轮不改，等用户决定。
