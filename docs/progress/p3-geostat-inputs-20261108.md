# P3：SGS 硬屏障与协变量跨 CRS 采样（2026-11-08）

任务分支 `codex/paleo-p3-geostat-inputs-20261108`（base = origin/master `86454671`）。
目标：让 SGS 实际消费已定义的硬屏障语义（静态井样本与已模拟格**两类邻域同时**
隔断）；让协克里金在井位与输出格点**同一实现**下跨 CRS 读取协变量。贯通冻结
输入 → 数值计算 → QC/血缘 → 既有生产入口。

## 已实现（按工作包）

### W1 冻结输入与计算契约
- `GeostatJob`（constraintjobs.h）新增冻结字段：`barrierLines`（prepare 物化的
  硬屏障部件，store CRS 值对象——compute 不再向 live store 取几何）、
  `barrierCrsWkt`、`plannedBarrierIds`、`constraintsConsumed`（引擎回执）、
  `barrierStats`。主线程物化一次，worker 消费值对象。
- `prepareGeostatJob`：`planConstraintUse` 判定 consumed 的 hard_barrier 部件
  → `singlefactor::ConstraintLine` 冻结；store GPKG CRS 一并冻结（缺 CRS 明确
  失败）。kriging/cokriging 不冻结任何线（计划侧逐条 unused，语义不变）。
- 数值核合法性检查在分配 realizations 大数组**之前**完成（尺寸/值域/样本
  归属/无样本分量/重合组归属矛盾全部拒绝，`realizations` 为空）。

### W2 逐线语义解析与真实回执
- `constraintsemantics.cpp`：sgs 从地统 lump 拒绝拆出——`hard_barrier`
  （full_block，display_only 已在语义层重分类）→ `consume(role=hard_barrier)`；
  direction/soft → `unsupported_by_method`；制图线 → `cartographic_only`。
  `methodReadsConstraints("sgs")=true`；kriging/cokriging 分支不动。
- `materializeSgsBarrier` 回执对账：`constraints_consumed` = 引擎实际落格
  （`BarrierGrid.consumedIds`，{id, role} 结构与 localdirection 血缘同口径），
  **不照抄计划**；计划 consumed 但未落格 → `unused(rasterized_no_cells)`。
- 停用线（disabled）、方向线（unsupported_by_method）、异层位（不进本层
  加载面，`other_horizon` 判定由算法层测试直接覆盖）逐条可核查。
- 降级 IDW（<8 样本）时计划 consumed 的硬屏障如实转
  `unused(degraded_idw_does_not_read_constraints)`，不留黑洞行。

### W3 静态与动态邻域同时隔断
- `sgs.h` 新增纯值 `SgsBarrierInput`（cellComponent/sampleComponent/
  componentCount/consumedBarrierIds/barrierModel）；`SgsParams::barrier`
  为 `std::optional`——**无值时数值路径与旧实现逐位一致**（golden anchor
  全绿为证），不绕一遍新流程。
- `sgs.cpp` 分量路径：静态邻域 = 分量内 `NeighborIndex`；动态邻域 =
  `SimulatedLattice::queryNearestInComponent`（跨分量已模拟格不可见——不为
  凑邻居跨屏障借点）；屏障格/域外格 nodata；无样本分量与 localidw
  「硬隔断无井闭合区」同口径拒绝；井落屏障（ambiguous）拒绝。
- 物化走生产同款 `labelHardBarriers`（grid_connectivity_v1：supercover
  栅格化 + 4 连通，有限端点可绕行），非手绘分区。
- 适配器 `singlefactor/sgsbarrier.{h,cpp}`：BarrierGrid → SgsBarrierInput +
  稳定重编号（分量号 = row-major 首现格索引序，与约束行存储顺序解耦）。

### W4 正态得分、变差与随机流
- 分量内样本各自建 `NormalScoreTable`（去重后），反变换用该分量自己的
  支持分布；共享变差模型按分量实测得分方差标定；`refitVariogramOnNormalScores`
  开关语义保持（不偷偷开启，分量样本 <8 时回退比例缩放）。
- 变差模型**全场共享**（显式或自动拟合），不实现每断块独立拟合——QC
  `sgs.barrier.variogram_shared_across_components=true` + `normal_scores=
  "per_component"` 明示口径。
- 随机流按「实现 × 稳定分量号」双重 SplitMix64 派生；路径 = 分量格子集
  Fisher–Yates。反事实（固定显式模型，只改另一分量样本值）本分量实现
  **逐位不变**——同时抓静态井与已模拟格两类泄漏。

### W5 协变量跨 CRS 采样
- 新组件 `src/qgis/geostatcovariatesampler.{h,cpp}`：井位与格点同一实现；
  位置经 `QgsCoordinateTransform`（空上下文）变换到栅格 CRS 采样，返回核
  的 secondary 坐标保留计算 CRS。相同 CRS 由 `isShortCircuited` 验证直采；
  变换初始化失败且 WKT 不等 → 整体明确失败；**无 CRS 栅格明确失败**（旧
  行为把 CRS 未知当同域直采——本任务修正，两个旧夹具补 CRS 声明）。
- nodata/越界/非有限/单点变换失败/取消分类计数（`CovariateSampleDiagnostics`）；
  不自加比例转换（`rescaleSecondaryToPrimary` 契约不动）。

### W6 QC 与血缘
- QC `constraints_consumed` 从恒空改为引擎真实回执；`sgs.barrier` 节点
  （model/components/barrier_cells/component_samples/component_cells/口径）；
  `cokriging.covariate_sampling`（wells/grid 两类位置的 crs_compute/crs_raster/
  transform_mode/分类计数）；rho 系字段与 schema `paleo_geostat_qc_v1` 兼容
  读取不变。发布链 extra 同源复制。

### W7 入口、取消与资源
- 物化段接入 `computeGeostatJobWithService`（网格段后）；栅格化带取消
  （含 #417 barrierCells<0 哨兵）；采样器每 256 点查取消；既有实现张量/
  求解器/线程预算不动（分量索引只含分量样本，不复制全场张量）；失败/
  取消不留被标记成功的输出（cleanupTemp 既有语义）。

## 验证证据

| 套件 | 结果 |
|---|---|
| tst_geostat_sgs（含 goldenAnchorPinsRefactoredKernel） | 15/15 |
| tst_geostat_barrier_partition（新增：反事实逐位隔离/屏障格 nodata/合法性拒绝/并行=串行/同 seed 复现/单样本退化/屏障上井拒绝/自动拟合隔断） | 11/11 |
| tst_geostat_covariate_sampling（新增：同 CRS 短路/跨 CRS 一致/分类计数/无 CRS 拒绝/取消） | 7/7 |
| tst_geostat_workflow（新增 6 例：sgsBarrierFullChain/unusedReasons/crossCrs/missingCrs/行序无关 SHA/降级回执；含 cokrigingRhoProvenance 夹具补 CRS） | 20/20 |
| tst_singlefactor_constraintuse（sgs 分支断言更新） | 30/30 |
| tst_singlefactor_constraintreceipts / tst_geostat_cokriging / tst_geostat_sgs3 / tst_factorworkflow（夹具补 CRS）/ tst_mappingpages | 全绿 |
| Source gates：layering --strict / qgs-audit / qgis-spread / file-budget / i18n / ui_tokens / docs_drift | 全绿（无新增违规、无基线变化） |

命令口径：`ctest` 带 add_paleo_test 沙箱（QT_QPA_PLATFORM=offscreen、XDG 隔离、
QGIS_PREFIX_PATH=vendored prefix）；裸跑需 `LD_LIBRARY_PATH=<prefix>/lib`。

## 已知边界与限制

- SGS 屏障模型声明为 `grid_connectivity_v1`（生产路径 resolveParameters 钉死
  的唯一模型，有限端可绕行）；`interpretation_partition_v1` 未接入 SGS
  （其生产入口本就仅测试使用）——未支持配置在运行前明确拒绝的原则由
  「CRS 缺失/变换不可用即失败」与数值核合法性检查承担。
- 约束库 CRS 是工程网格（ENGCRS，无大地基准）：与 EPSG 井之间无 PROJ 变换
  时物化明确失败（提示配准/统一坐标系），不静默丢线。生产口径：井与库
  同处工程网格（同 WKT 短路/等价直采）。
- 协克里金 ρ 估计、`secondary_rescaled_to_primary`、cross-validation 口径
  未改动；无约束输出字段含义不变（constraints_consumed 为空列表）。
- 每断块独立变差拟合仍属 TODOS 递延（小块少井政策不在本任务）。

## 与并行任务的关系

- 不改 #906 占用的 `algorithmadapters.{h,cpp}`（runSgs 原样透传 SgsParams，
  纯值扩展零改动）、`constraintworkflow.cpp`、capabilitygateway、发布内核。
- 与同批 P1/P2（datalist）、P4（wellsection）、P5（mapbook）无共享文件；
  共享接缝仅根 CMakeLists.txt 一行 manifest 登记。

## 独立复审记录（2026-11-08，未参与实现的 reviewer 通读真实 diff）

结论：**可提交**。三类假闭环（只改说明/末端掩膜/偷偷同 CRS）逐环核实均不成立；
无屏障零漂移由未触碰的 golden anchor 实证；物化-回执-降级链路、CRS 等价判定、
线程/取消/错误路径均通过。随本 PR 已修：
- 补镜像反事实测试 `counterfactualReverseIsolationCatchesDynamicLeak`（改先模拟
  分量的样本值、断言后模拟分量逐位不变）——原反事实只钉「先模拟分量不被后
  分量污染」，动态邻域过滤（后分量不得看见先分量已模拟格）此前零断言。
- `toSgsBarrierInput` 的 sampleComponent 短于样本数时尾部按未归属（-1）交
  数值核拒绝，不再静默归 0 号区。
- 删除测试冗余 include；修正 `twoPartitionsStrictlySeparated` 的注释（值域
  断言对零星泄漏不敏感，逐位泄漏由反事实测试钉住）。

复审指出、本 PR 接受的现状（留档）：
- sgs.cpp 原路径与分量路径的每格步进为注释约定的逐句对齐（复制粘贴级，
  file-budget 闸只扫 src/ui 不覆盖）——后续抽公共 per-cell 函数可消除漂移
  风险（不在本任务扩散重构面）。
- 同一 job 对象重复 compute 且首次「有线全未落格」时 unused 条目可能重复
  追加（生产路径 job 一次制备一次计算，不可达；幂等防御已覆盖主路径）。
