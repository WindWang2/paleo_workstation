# Agent B 审查报告 — src/algorithms + src/services（只读）

基线 366aef97（worktree paleo_workstation-full-review-20261018）。
范围：src/algorithms（144 文件）数值正确性/地质语义/内存/并发；src/services（74 文件）
任务系统/后台作业/缓存/证据索引/渲染服务。
排除依据：01-conflict-matrix.md + 99-external-issues-snapshot.txt（44 个开放 issue
全文通读，含 /tmp/paleo-audit/issue-460..503.txt 逐条核对）。

重要背景事实（影响判定）：本仓在基线上**已包含**多个快照 issue 的修复——
#468 输出张量闸（sgs.cpp:134-143 / sgs3.cpp:391-401）、#470 shuffleDeterministic
（cluster.cpp:135 / som.cpp:88 / supervised.cpp:485）、#471 相对主元
（wavelet.cpp:29-34 已是 1e-12×scale）、#478 的 cokriging 收敛/工作区复用与 SGS
并行化（cokriging.cpp/sgs.cpp 注释自证）、sparse.cpp 缺失样掩膜
（sparse.cpp:164-166,195-197）。这些条目按「已落地」处理，只审残留面。

---

## 1. 覆盖清单

方式：逐行 = 全文件精读；重点 = 数值核/并发核精读+周边调用链；接口 = 头契约+关键
执行路径抽查；grep = 模式扫描（static/mutable 状态、除法、QHash 迭代序、线程派生）。

| 文件 | 方式 | 风险级 | 关键函数 | 调用链 | 测试对应 |
|---|---|---|---|---|---|
| geostat/kriging.cpp | 逐行 | 高 | solveOrdinaryKriging/SolverCore/ordinaryKriging | krigingsurface→localidw(K=kriging)、workflow | tst_geostat_kriging |
| geostat/cokriging.cpp | 逐行 | 高 | solveCoreAt/ordinaryCoKriging/ConstrainedKrigingSolver::solveAt | 同上+软罚约束 | tst_geostat_cokriging |
| geostat/sgs.cpp | 逐行 | 高 | sgs()/runRealization/SimulatedLattice::queryNearest | 直连 API、sgsfill 经带分派 | tst_geostat_sgs |
| geostat/sgs3.cpp | 逐行 | 高 | sgs3()/StaticIndex3/SimulatedLattice3 | stratgrid/sgsfill | tst_geostat_sgs3 |
| geostat/sgs_internal.h | 逐行 | 高 | unitRandom/NormalScoreTable/solveSimpleKriging/rescale | sgs/sgs3 共享 | tst_geostat_sgs* |
| geostat/variogram.cpp | 逐行 | 高 | experimentalVariogram/fitVariogram | resolveVariogram | tst_geostat_variogram(+_barrier) |
| geostat/linsolve.cpp | 逐行 | 高 | solveDenseLu | 全克里金系 | tst_geostat_linsolve |
| geostat/neighborhood.h | 逐行 | 高 | dedupeSamples/NeighborIndex::queryNearest | 同上 | tst_geostat_neighborhood |
| geostat/faultpath.cpp / crossvalidation.cpp | 逐行 | 中 | faultPathMetric/leaveOneOut | 变差隔断档 | tst_geostat_faultpath |
| inversion/wavelet.cpp | 逐行 | 高 | solveLinear/extractWavelet/makeRicker/rotateWaveletPhase | 反演服务 | tst_inversion_wavelet |
| inversion/sparse.cpp | 逐行 | 高 | sparseSpikeInversion(FISTA)/convSame/convAdj | 反演服务 | tst_inversion_sparse |
| inversion/lowfreq.cpp | 逐行 | 中 | lowFreqImpedance/lowFreqTraceAt/idwAt/nearestCell | inversionworkflow.cpp:481 | tst_inversion_lowfreq |
| paleoalgorithms.cpp | 逐行 | 高 | ConstraintIDW::processAlgorithm/FaciesFusion/GeologicalSmoothing/Isopach/gridDimsForExtent | QGIS 注册表全引擎 | tst_singlefactor_*（多套） |
| mincurvature.cpp + gridsolver.cpp | 逐行 | 高 | solveMinimumCurvature/KdTree/crossValidateLeaveOneOut | min_curvature 引擎 | tst_singlefactor_perf 等 |
| distancetransform.cpp / welldist.cpp | 逐行 | 中 | Dijkstra 主环/writeEuclideanDistances | welldist 因素 | tst_singlefactor_store 等 |
| velocitymodel.cpp | 逐行 | 高 | fitV0k/v0kDepth/v0kTwt/convertTimeGridToDepth | 时深转换工作流 | tst_inversion_welltd |
| seismicattr.cpp | 逐行 | 高 | analyticSignal/complexTraceAnalysis/semblance(Weighted) | seismictaskservice_attr | tst_seismic_attr（attr 核） |
| singlefactor/localidw.cpp | 逐行 | 高 | Engine/evaluateBatch/metricDistance2/软边界罚 | 单因素主插值面 | tst_singlefactor_* |
| singlefactor/krigingsurface.cpp | 逐行 | 高 | resolveVariogram/evaluateLocalKriging/fallbackToIdw | 单因素克里金 | tst_singlefactor、tst_geostat_workflow |
| singlefactor/curvekernel.h | 逐行 | 中 | tangentEnergy/CurveKernel 契约 | localidw 热路径 | tst_singlefactor |
| faciespolygonize.cpp | 逐行 | 高 | smoothMajority/buildBoundaryGraph/smoothArcs/conflateArcs/polygonizeGrid/writeGpkg | paleo:paleo_facies_polygonize | tst_polygonize |
| faciessliver.cpp | 逐行 | 中 | absorbSmallParts（并查集吸收） | polygonize 内 | tst_polygonize |
| cluster/cluster.cpp,som.cpp,supervised.cpp | 重点 | 高 | #470 修复面核验+classifyGaussian/kNN/LDA | faciestraining | tst_cluster*/tst_facies* |
| stratgrid/sgsfill.cpp | 重点 | 高 | fillSgs（分带分派/预算/输出分配） | 相模型充填 | tst_stratgrid* |
| stratgrid/stratgrid.h | 接口 | 中 | cellIndex/cellCount（int 乘法） | sgsfill/propfill | （#478 P3 残留） |
| petrophys.cpp | 逐行 | 中 | vsh 族/phi 族/swArchie/resampleLinear/curveStats | petrophyscomputeservice | tst_petrophys* |
| welldist/wellsiting/curveexpr/rasteralgebra/ensemblestats/surfacevolumes/horizontrack/evolution/faultsurface/faciesmapping/singlefactor 其余（support/partition/contours 族/structural/localdirection/wellacquisition/corridor/cartographic） | 接口+grep | 中低 | 头契约+除法/静态状态扫描 | — | 各自套件 |
| services/paleotaskservice.{h,cpp} | 逐行 | 高 | TaskRunner/shutdown/createTask/onTaskFinished | 全后台任务 | tst_taskservice |
| services/jobrunner.{h,cpp} | 逐行 | 高 | JobRunner::start 三段式/Claim/析构 | 单因素/编图作业 | tst_factorworkflow 等 |
| services/futuretask.h | 逐行 | 高 | paleo::tasks::track/pool | imagelod 等 QFuture 消费方 | tst_futuretask |
| services/seismictaskservice.cpp | 逐行 | 高 | datasetEntryFor(LRU)/startBounded(信号量闸) | 全地震任务 | tst_seismic_* |
| seismictaskservice_slice.cpp | 重点 | 高 | startSliceExtraction/startSectionExtraction（缓存键+取消） | 切片/剖面 UI | tst_seismic_services |
| seismictaskservice_viz.cpp | 重点 | 高 | startTimeSliceTiled（同键取代）/slicePlaneFromWindow | 时间片/体窗 | tst_seismic_services |
| seismictaskservice_attr.cpp | 重点 | 高 | attrSampleOfTrace/内层 std::thread 分块 | 属性计算 | tst_seismic_attr |
| services/evidenceindex.cpp | 重点 | 中 | SQL 全走绑定+where 白名单模板；无锁（同步索引，#494 已记 UI 线程限制） | 证据反查 | tst_evidenceindex |
| services/petrophyscomputeservice.cpp | 重点 | 高 | startBatch（worker 闭包/产物单写者队列/finished 登记目录） | 测井计算批 | tst_petrophys* |
| services/imagelod.cpp | 逐行 | 中 | FullImageCache/requestDecode（epoch 代际） | 井附件图 | tst_imagelod（若有） |
| services 其余（pythonenv/scriptrunner/pythonrepl/datagraph/previewdoc/crossplot*/welllogset/seismichorizoncluster/seismicmapping/frameworkservice/storagegovernance/cataloghealth/crashreport/errorhub/projectdata/derivationgraph/singlefactordef/algoparamschema/scriptprotocol/shortcutregistry/startuptrace/toolavailability） | 接口+grep | 低-中 | #479 清单项核对（pythonenv 无超时等仍在） | — | 各自套件 |

模式扫描结论：src/algorithms 无 static 可变状态、无 mutable 懒缓存（全仓
`grep -rn mutable src/algorithms --include='*.h'` 零命中）；除法均有 max/钳制
守卫；QHash 迭代序共 3 处消费，其中 2 处已排序钉死（faciespolygonize.cpp:875/1030），
1 处未钉（见 FB-001）。

---

## 2. Findings（新问题）

### FB-001 多数滤波并列计票依赖 QHash 迭代序 → 转面成果跨进程不可复现

- **Severity**: P2（High-中）
- **Confidence**: High（机制三重证据：代码、同文件作者自注、Qt 6.8 头文件语义）
- **Module**: src/algorithms
- **File:Line/Function**: `src/algorithms/faciespolygonize.cpp:143-185`（`smoothMajority`，并列裁决环 :168-177）
- **Root Cause**: 3×3 多数滤波的胜者在「计数并列的最大码」之间由
  `QHash<int,int>::cbegin()/cend()` 的遍历序决定（`it.value() > bestCount` 严格大于，
  并列时先遍历到者胜）。Qt 6.8 的 `QHashSeed::globalSeed()` 是每进程随机种子
  （`/usr/include/qt6/QtCore/qhashfunctions.h:39-49` 提供
  `setDeterministicGlobalSeed()`，全仓 `grep setDeterministicGlobalSeed|QT_HASH_SEED`
  零命中——应用从未固定种子），故同一输入栅格在不同进程/运行里可得到不同的平滑结果。
  同文件作者在 :875-877 与 :1030-1032 两次注释「QHash 迭代序随进程哈希种子漂移，
  同输入两次转面会得到不同图斑序」并专门排序钉死——但 `smoothMajority` 这第三处
  消费点漏掉了同一纪律。姊妹实现 `GeologicalSmoothingAlgorithm`
  （paleoalgorithms.cpp:1130-1146）用「并列取数值更小码」的确定性裁决，口径已存在。
- **Trigger**: `SMOOTH`（默认 1）> 0 时，任何 3×3 窗口内「当前码计数不是最大、且
  ≥2 个码并列最大」——相边界椒盐区高发。例：某格 3×3 邻域计数
  {code 3: 3 票, code 7: 3 票, 当前 code 1: 2 票, 其余 nodata}。
- **Observed/Expected**: 观察值（跨两次运行）：code 3 或 code 7 随进程种子漂移 →
  下游 polygonize/吸收/边界图整体分叉、`PROVENANCE.smoothed_cells` 漂移、GPKG fid
  归属漂移（正是 :875 注释修过的问题在数值面复发）。期望：同输入逐位确定
  （如并列取小码，或保持当前码优先级次之）。
- **算例/复现**: 单测构造 3×3 网格 `[[3,7,1],[3,1,7],[7,3,1]]`（中心 code 1），
  在同进程内先 `qSetGlobalQHashSeed(1)` 跑一次 `smoothMajority`、再置 2 跑一次，
  断言两次输出相等（当前代码预期红）。
- **Impact**: 相图矢量化的可复现性契约（与 sgs.h 同 seed 逐位一致口径同级）被
  破坏；演化跨期对比、证据血缘对拍在同数据上出现无解释差异。
- **Existing Issue Match**: 无（#470 是 cluster 随机数分布实现定义——不同机制、
  不同文件）。
- **Active PR Match**: 无（#521 触碰 qgis/ui/workflow 的相面编辑链，
  #518 触碰 algorithms/paleoalgorithms.cpp——均非本文件本主题）。
- **Fix Strategy（最小）**: 裁决环改为确定性并列次键：遍历时
  `if (count > bestCount || (count == bestCount && key < best))`（对齐
  GeologicalSmoothing 的「并列取小码」），或先把 counts 键排序后遍历。不改任何
  非并列路径的数值。
- **Regression Test**: 见上「算例/复现」；另补一条 SMOOTH=1 全链路两次运行
  sha256 相等的长测（同进程双 seed 即可模拟跨进程）。

### FB-002 SGS 并行实现的工作集未计入 #468 内存闸，峰值可至声称值的 ~3-8 倍

- **Severity**: P2（High-中）
- **Confidence**: High（纯算术，代码路径可枚举）
- **Module**: src/algorithms
- **File:Line/Function**: `src/algorithms/geostat/sgs.cpp:205-214`（每实现分配
  path/gaussianField/lattice 三数组）、`:375-392`（`workers = min(R,4)` 并行档）；
  `sgs3.cpp:505-514`（同三数组 + `:225-237` `m_lookup` unordered_map）、`:684-701`
  （并行档）；预算注释 `sgs_internal.h:26-32`（自述「预算 4GiB 时整体 ≤ ~4.5GB」）。
- **Root Cause**: #468 的修复只给**输出张量**（R×cells×8B ≤ 4GiB）加了闸。并行档
  （R>1 且无 Control 回调，geostat 公开 API 直调即满足）里每个 worker 各自持有
  `path`（cells×8B）+ `gaussianField`（cells×8B）+ `lattice.values`（cells×8B）
  ——sgs3 还有 `m_lookup`（n 个 unordered_map 节点，每节点 ~32-48B）。
  workers=min(R,4) 份同时在飞。
- **Trigger**: cells=1e8（单轴闸允许）、R=4（R×cells×8B=3.2GB ≤ 4GiB 全合法）、
  无回调直调 `geostat::sgs3`。
- **Observed/Expected**: 观察值（峰值估算）：2D ≈ 输出 3.2GB + 4 worker × 2.4GB
  ≈ **12.8GB**；3D 再加 4 × (3.2~4.8GB) 查找表 ≈ **25GB+**。期望：符合
  sgs_internal.h:31 注释承诺的「≤ ~4.5GB」（该注释连 `path` 数组与 worker 倍增
  都未计入，属于失实口径）。
- **算例/复现**: 单测无法真跑 1e8 格（也不该跑）；可测预算函数——把「峰值估算 =
  输出 + workers×3×cells×8（3D 另加 n×40B）」抽成可测纯函数后断言其在上述参数
  组合下给出 >4.5GB 的数，并在入口拒绝。
- **Impact**: 合法参数下 OOM（Linux OOM-kill 或 Windows 提交失败），且闸门注释
  给出虚假安全感。与 #468 当初要防的场景同后果。
- **Existing Issue Match**: **部分相邻，非同根因**：#468 文本与证据针对输出张量
  （「realizations 为 vector<double>，最坏 64×1e8×8B」）并已在基线修复；工作集
  ×并行 worker 的放大是 #478 并行化（也在基线）**新引入**的量，未被任何 issue
  文本覆盖。#468 顺带提到的「每实现 gaussianField/lattice 0.8GB churn」是串行
  churn 口径，非 4 worker 并存峰值。
- **Active PR Match**: 无（#518 的 algorithms 触碰面是 paleoalgorithms.cpp）。
- **Fix Strategy（最小）**: 预算闸把并行档峰值计入：`R×cells×8 + workers×3×cells×8
  （3D + workers×n×kLookupBytesPerEntry）≤ kMaxRealizationBytes`，超限要么拒收、
  要么按预算收缩 workers（workers 是资源面，非成果语义，可静默收缩——与实现数
  「拒绝而非钳制」的语义不同）。同步修正 sgs_internal.h 的「≤4.5GB」注释。
- **Regression Test**: 预算函数单测：{R=4, cells=1e8, 3D} 断言拒收/收缩；
  {R=64, cells=8.4M} 断言放行（原闸语义保持）。

### FB-003 stratgrid/sgsfill 聚合输出（R×cellCount float）无组合内存闸

- **Severity**: P3（Medium-Low）
- **Confidence**: Medium-High（代码确定；实际可达规模受 #478 未修的 int 溢出门偶然限制）
- **Module**: src/algorithms
- **File:Line/Function**: `src/algorithms/stratgrid/sgsfill.cpp:204-212`
  （`out->assign(R, PropertyVolume{}); values.assign(cellCount)`）+ 逐带调用
  `geostat::sgs3`（每带各自过 #468 闸）。
- **Root Cause**: fillSgs 的最终输出 = R × 总格数 × 4B float，无任何闸。#468 的闸
  在每带 sgs3 入口（R×带内目标×8B ≤ 4GiB）——多带各自合规、总和无界。
- **Trigger**: 多相带 3D 相建模，总格数 ~5×10^8（int 可容纳）、R=64：输出
  64×5e8×4B = **128GB**；每带瞬时仅 ~4GiB 双精度 + 输出累计。
- **Observed/Expected**: 观察值：合法输入 OOM。期望：入口拒收（同 #468 拒绝语义）。
- **Impact**: 大三维工区+高实现数的相充填 OOM。可达性受限于 #478 P3 未修的
  `stratgrid.h:66-67` int 溢出闸缺失（cellCount 为 int 乘法，负值时 assign
  直接巨量 bad_alloc——fail-fast；本条指的是 int 内合法段的无闸放大）。
- **Existing Issue Match**: **#468 同族**（「实现数×格点数组合内存闸缺失」），
  但 #468 证据只列 geostat 两个入口、其修复也只落在这两处——本条是未被该修复
  覆盖的第三入口（stratgrid 充填链）。按规则登记为同族残留，供主 Agent 决断
  归并 #468 或单开。
- **Active PR Match**: 无。
- **Fix Strategy（最小）**: fillSgs 入口补 `R × cellCount × sizeof(float) ≤ 预算`
  闸（常量可复用 kMaxRealizationBytes）；顺带补 #478 P3 的 `cellCount ≤ INT_MAX`
  闸（三行）。
- **Regression Test**: 小格架 + R 钳制后断言：超预算参数被拒且 *error 非空。

---

## 3. SUSPECTED（未达报告门槛，单列）

- **S-1 Isopach 顶/底符号约定**（paleoalgorithms.cpp:1186-1316 +
  constraintfactorjobs_factor.cpp:380）：`thickness = INPUT_TOP − INPUT_BASE`。
  若用户喂深度域（正值向下，TWT/TVD）构造面，几何上顶<底 → 全图负厚度；默认
  `negativeToNodata=true` 时**整图 nodata（响亮失败）**，非静默错值。是约定/文档
  问题还是缺陷取决于构造面在本仓的域约定（elevation 正向上），未追完全链，
  不作为 Finding。
- **S-2 lowfreq nearestCell O(井×格)**（lowfreq.cpp:52-74）：注释自证
  「井数 × 格数 ≤ 百万级」，但 nIl×nXl 来自地震体几何（可达 10^6-10^7）×井数
  10^2 → 10^8-10^9 次迭代，在 inversionworkflow 的 worker 线程上秒级。规模依赖，
  无实测收益证据，按「不做推测性优化建议」不升级。
- **S-3 fillSgs 种子回写 float 精度**（sgsfill.cpp:333-338）：硬数据钉死用
  `float` 存储均值——相模型属性面用 float 是行业常规，仅记录。

## 4. 排除项汇总（对照 44 issue 快照 + PR #518/#521）

| 候选 | 判定 | 依据 |
|---|---|---|
| paleoalgorithms.cpp IDW O(rows×cols×samples) 无空间索引；:692-714 `queue` 实为栈式 DFS 命名误导；hull.contains 逐格 GEOS 谓词 | EXCLUDED_EXISTING_ISSUE + EXCLUDED_ACTIVE_WORK | #478 P2 原文；文件与 #518 重合 |
| stratgrid.h:66-67 cellIndex/cellCount int 乘法溢出、sgsfill.cpp 只挡单轴 | EXCLUDED_EXISTING_ISSUE | #478 P3 原文（基线未修，属 issue-sweep 范围） |
| petrophys swArchie isfinite | 已修（基线含 #478 修复） | petrophys.cpp:123-127 |
| horizontrack normalizedCorrelation 直流偏置 | EXCLUDED_EXISTING_ISSUE | #478 P3 |
| dsp/fft 旋转因子递推累积误差 | EXCLUDED_EXISTING_ISSUE | #478 P3 |
| cluster/som/supervised 随机分布不可复现 | 已修（shuffleDeterministic/unitRandom，基线含） | #470 |
| wavelet 高斯消元绝对主元 | 已修（相对主元 1e-12×scale，基线含） | #471 |
| SGS 输出张量组合闸缺失 | 已修（基线含；残留面见 FB-002/FB-003） | #468 |
| cokriging 双重载语义漂移/每点重分配 | 已修（solveCoreAt 单路径，基线含） | #478 |
| sparse.cpp 缺失样以 0 入保真项 | 已修（掩膜，基线含） | #478 P3 |
| sgs finiteCells 取末实现 | 文档化契约（sgs.h:39-40 注释） | #478 修复时钉死口径 |
| pythonenv startStep 无超时 / scriptrunner drainChannel 无上限 / pythonrepl 编码不一致 / wellfaciesservice m_accepted 只增 | EXCLUDED_EXISTING_ISSUE | #479 P3 |
| onnxpredictionservice 推理与会话同锁 / remotepredictrouter 主线程降级 | EXCLUDED_EXISTING_ISSUE | #479 P2 |
| evidenceindex 首查 UI 线程同步 | 已文档化（#494 未解决问题清单） | #494 |
| welldist O(rows×cols×points) | 文档化优化路径（welldist.cpp:97-100 自注） | #478 同族口径 |
| services/jsonsidecard.h、paleoalgorithms.cpp 上的任何其它主题 | EXCLUDED_ACTIVE_WORK | #518 触碰面 |

## 5. 未覆盖区域（如实申报）

- **singlefactor 大文件未逐行**：localdirectionalgorithm.cpp（1486）、fieldcontours.cpp（1287）、
  structuralalgorithm.cpp（843）、support.cpp（948）、partition/corridor/faultpath/
  contouravoidance/regionalcontours/cartographicsmooth/buffertransition/wellacquisition——
  只做头契约+关键除法/索引/静态状态扫描（structural POWER 入口闸 #492 U3 已核在位）。
- **services 大文件未逐行**：seismictaskservice_interp.cpp（1161）、_transcode.cpp（948）、
  previewdoc*.cpp（~1900）、datagraph*.cpp（~1260）、crossplotsamples/sources（~850）、
  faciesclassificationservice/faciestraining、welllogset、seismichorizoncluster、
  seismicmapping、projectdata、crashreport——接口+模式扫描级。
- **algorithms 其余**：evolution/compare.cpp、faultsurface（1211）、wellsiting（614）、
  rastersurface 的 rasteralgebra、surfacevolumes、ensemblestats、curveexpr（566）、
  frameworksuggester、constraintbarriers——接口级。
- **本机环境限制**：只读约束下未编译/未跑测试；FB-001 的跨 seed 复现与 FB-002 的
  预算算例均为可写单测的设计稿，未执行验证。
- **BatchJobQueue（src/workflow/batchjobqueue.cpp）**：任务属 Agent C/D 的 workflow
  域，本 Agent 仅核对了 PaleoTaskService 侧与其对接面（layerHolds 引用计数、
  session 取消），未审队列本体。

## 6. 任务系统正面结论（审查重点 3 的核验记录）

PaleoTaskService/JobRunner/futuretask/SeismicTaskService 逐行未见新缺陷：
- 取消不产生假成功：applyFinish 的运行态闸 + m_cancel 判定；JobRunner commit 入口
  的 session/generation 双比对（#235）；discardPublication 把陈旧成功翻 Cancelled。
- 失败态不访问未产出 outputs：petrophyscomputeservice 分井失败只交付 productPath
  已存在的井；seismic 各 onFinished 在 Failed/Cancelled 分支不触碰 outImage。
- 迟到结果防线：JobRunner Claim.current()（代际）+ task->session() != service->session()；
  imagelod epoch 代际；seismic viz 同键取代（inFlightTiledTasks_）。
- 无界队列/缓存：m_tasks 有 setRetention 双策略；SgyDataCache 预算 LRU；dataset
  注册表 8 条 LRU；FullImageCache 条目上限。均查过。
- 关停：shutdown 的 clear()+补偿 applyFinish（#416）与孤儿化兜底路径推演无竞态。
- 唯二注意点（不构成 Finding）：TaskRunner::run 的 `QString::fromLocal8Bit(e.what())`
  （Linux 上异常文本多为 UTF-8，极端 locale 下乱码——纯文案级）；futuretask::track
  对 cancellable=false 的 purge 任务，beginNewSession 无法取消（设计意图，注释在案）。
