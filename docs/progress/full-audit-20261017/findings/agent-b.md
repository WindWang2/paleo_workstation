# Agent B — 算法与服务层深度审计报告（2026-10-17）

- 审计对象：`/home/kevin/projects/paleo_workstation-full-review-20261017`（HEAD = origin/master c720aef5，只读）
- 范围：`src/algorithms/**`（全部子目录）、`src/services/**`；按任务书优先深审 master 新合入 #496（a1efbd74）的算法侧改动（paleoalgorithms.cpp / singlefactor/structural.cpp / workflow/surfacegridding.cpp / domain/constraintsemantics.cpp）。
- 去重基线：#468/#470/#471/#478/#479 + COMPREHENSIVE_AUDIT_2026-10-16 + SWARM_REVIEW_2026-10-15 + codex/audit-remediation 已改文件（faultsurface、inversion/volume、wavelet、constraintparse、localidw、velocitymodel 的 z=0/T=0 边界、seismictaskservice_attr、faciesclassificationservice）。根因重合者一律未列。
- 方法：高风险数值函数逐行审 + 解析推导对拍（velocitymodel 雅可比手推 + 数值差分验证）；服务层沿 prepare→compute→commit→publish 链与取消/迟到结果路径逐段核。仅静态审查（禁构建）。

## 汇总

| ID | 严重度 | 置信 | 模块 | 一句话 |
|----|--------|------|------|--------|
| B-02 | P1 | VERIFIED | algorithms/velocitymodel | V0-k 拟合 Gauss-Newton 的 ∂T/∂k 解析雅可比写错（缺 1/k），带噪检查井拟合系统性偏差 |
| B-01 | P2 | VERIFIED | workflow/surfacegridding | min_curvature 工作流路径屏障栅格化仍是半格超采样（#488/#496 判定为 P1 缺陷的同款），陡斜屏障可漏 |
| B-03 | P2 | HIGH | algorithms/geostat/sgs | 邻域截断的等距并列打破是 stdlib 实现定义（partial_sort/sort 等价类），违背同 seed 逐位可复现契约 |
| B-04 | P2 | VERIFIED | services/seismictaskservice_slice | 异常路径 workers 未 join → std::terminate（audit-remediation 只修了 attr 两处，漏了 slice 这处） |
| B-05 | P3 | SUSPECTED | algorithms/seismicattr | semblanceDimsFit 守卫自身 i64 乘法可溢出回绕，恶意维度可绕过护栏 |
| B-06 | P3 | VERIFIED | services/seismichorizoncluster | nodata 按 float==double 比较，非 float 可表示 nodata 泄为数据（#165 同族残留） |
| B-07 | P3 | VERIFIED | algorithms/paleoalgorithms | GeologicalSmoothing 无像元预算闸 + 每趟 `cur = nxt` COW detach 全幅拷贝 |
| B-08 | P3 | VERIFIED | services/paleotaskservice | percent() 可报 >100%、ETA 负值显示「约 1s」（进度回灌溢出时） |
| B-09 | P3 | VERIFIED | algorithms/singlefactor/structural | 显式软边界（explicitSoftBoundaries）不走 R4 域可达过滤，回执口径与停线不一致 |
| B-10 | P3 | SUSPECTED | algorithms/inversion/lowfreq | layerImpedance = nCells×nL 无内存预算闸；nearestCell O(井×格) 暴力 |

新发现计数：P1×1，P2×3，P3×6（B-04 属既有根因的修复覆盖缺口，按任务书标注 Existing Match）。

---

## B-02 [P1 / VERIFIED] V0-k 拟合 Gauss-Newton 的 k 向雅可比公式错误——带噪检查井拟合收敛到错误正规方程的不动点

- Module / File:Line / Function：`algorithms/velocitymodel` / `src/algorithms/velocitymodel.cpp:87-89` / `fitV0k`（匿名命名空间）
- Root Cause：
  ```cpp
  const double dTdk = std::abs(k) < 1e-12
      ? -1000.0 * z * z / (v0 * v0)
      : -2000.0 / (k * k) * std::log1p(k * z / v0) + 2000.0 * z / (v0 * v);
  ```
  对 T(v0,k,z)=2000/k·ln(1+kz/v0) 正确偏导是
  `∂T/∂k = −2000/k²·ln(1+kz/v0) + 2000·z/(k·v)`（v=v0+kz）。第二项分母写成了 `v0*v`，缺 `1/k`。
  解析验证：∂T/∂v0 的同型推导（−2000z/(v0·v)）与代码一致，佐导结构；k→0 极限分支 −1000z²/v0² 与正确公式的泰勒展开一致，但通式在 k→0 时含 −2000z/(k·v0) 发散项且无第二项对消——自相矛盾，实锤缺 1/k。
  数值对拍：v0=2000, k=0.5, z=1000 处真值 ∂T/∂k ≈ −185（差分验证），代码公式给 ≈ −1785（错 ~10 倍）。
- Trigger：任何 ≥3 个控制点、层速度随深度变化（k≠0）且数据带噪（真实检查井）的 V0kLinear 拟合。无噪数据（残差可驱到精确 0）不受影响——r=0 使 J'ᵀr=0 恒成立，真值仍是错误系统的不动点。
- Observed vs Expected：迭代收敛（或 60 轮游走）到 J'ᵀr=0 的点而非最小二乘最优 Jᵀr=0——注释承诺的「TWT 域 2 参数 Gauss-Newton 最小二乘」实际是另一个优化问题的驻点。fitRmsMs 如实变大但 fit() 无 RMS 闸，偏差静默入库。
- Impact：V0-k 速度模型系统性偏差 → 层位时深转换（convertTimeGridToDepth / depthForTwt 全链）深度失真；偏差量随噪声与 k 增大。
- Existing Match：NEW。codex/audit-remediation 对 velocitymodel.cpp 的改动只覆盖 v0kDepth/v0kTwt 的 z=0/T=0 边界（已核实 diff），未触及 fitV0k。
- 测试为何没抓住：`tst_velocitymodel.cpp` `v0kFitRecoversSyntheticTruth` 用无噪合成真值（r≡0 不动点不受雅可比影响）；`v0kFitRmsReportsAnchorResidual` 只断言 RMS∈(1,100)，偏差拟合轻松通过。
- Fix Strategy：第二项改 `2000.0 * z / (k * v)`（或统一写成 `−2000.0/k*( std::log1p(k*z/v0) − (k*z/v0)/(1.0+k*z/v0) )` 避免大数对消）。
- Regression Test：① 雅可比一致性：任取 (v0,k,z) 断言解析 dTdk 与中心差分 (T(v0,k+ε)−T(v0,k−ε))/2ε 相对差 <1e-6（当前实现会以 ~10 倍差距红）。② 带噪恢复：合成 (v0=1800,k=0.6) 加已知微扰后，用粗网格暴力搜索 (v0,k) 最小化 Σr² 作 oracle，断言 fitV0k 结果落在 oracle 邻域（容差按噪声水平定）。

## B-01 [P2 / VERIFIED] min_curvature 工作流路径的屏障栅格化仍用半格超采样——#488/#496 已定性为 P1 缺陷的同款漏格

- Module / File:Line / Function：`workflow/surfacegridding` / `src/workflow/surfacegridding.cpp:518-557` / `SurfaceGriddingWorkflow::buildBarrierMask`
- Root Cause：#496（叠加 #488）把屏障栅格化统一到 `supercoverPolylineCells`（闭域裁剪 + 4-连通途经链，constraintbarriers.cpp:85），并自述「此前半格采样在陡斜线/穿角点漏格」（paleoalgorithms.cpp:672 注释原话）。但 `gridHorizonText`（层位文本网格化主生产路径）走的 `buildBarrierMask` 保留自己的步进采样（每段 `ceil(len/(cell*0.5))` 个点、每点 floor 落一格）。同一 min-curvature 引擎家族出现两套栅格化：QGIS processing 算法 `paleo_min_curvature`（mincurvature.cpp:238）已用 `rasterizeBarrierParts`（supercover），workflow 路径没有。
- Trigger：陡斜（近 45°/穿角点）break_line 打断线，在 45° 附近半格采样可整格漏标；漏标格成为 mask 上的单格孔洞。
- Observed vs Expected：对 gridsolver 的无通量内边界语义，孔洞格是自由格（band→调和算子），其 4 邻同时耦合断层两盘——「断层两盘各自松弛、互不插值」被单点击穿。期望与 processing 算法路径同为 supercover（漏格不存在）。
- Impact：层位文本网格化（最小曲率主链）断层屏蔽在陡斜线处泄漏，两盘属性跨断层面平均；回执仍报 consumed（plan 侧闭域判定命中），用户不可见。
- Existing Match：NEW（#488/#496 的修复只覆盖 IDW 引擎与 processing 算法路径；pr457-closure.md「剩余风险」清单亦未列此项）。
- Fix Strategy：buildBarrierMask 的手工采样循环替换为 `paleo::algorithms::supercoverPolylineCells(vertices, geom.originX, geom.originY, geom.dx, -geom.dy, geom.cols, geom.rows, mark)`（层：workflow→algorithms include 已在用同头文件的 planConstraintUse 邻域，include 无新增越层）。附带：该函数本地 `cellOf` 无远边缘 clamp（gridsolver.cpp:144 的 cellOf 有 #162 clamp），换成 supercover 后一并消除。
- Regression Test：在 gridHorizonText 直测：构造恰好 45° 穿角点的双段 break_line + 两侧不同趋势散点，断言两盘格值互不受对方井控（当前实现会在交点附近漏格而红）；另加「workflow mask == rasterizeBarrierParts mask」逐位对拍断言（同输入同网格）。

## B-03 [P2 / HIGH] SGS 邻域截断的等距并列打破是标准库实现定义——同 seed 逐位可复现契约被破坏

- Module / File:Line / Function：`algorithms/geostat/sgs` / `src/algorithms/geostat/sgs.cpp:254-263`（合并邻域截断）；次发点 `src/algorithms/geostat/neighborhood.h:55-60`（dedupeSamples 排序等价类）
- Root Cause：
  1) `sgs.cpp` 合并静态+已模拟邻域后超编时用 `std::partial_sort(…, [按 d² 比较])` + `resize(maxPoints)`。比较器只含 d²——规则网格格心对称位置（(±1,0)/(0,±1) 等）d² 相等 → 大量比较等价元素。`partial_sort` 对等价类元素的选取顺序是标准库实现定义（libstdc++/libc++/MSVC 的 heap-select 算法不同），k 截断边界落在并列组内时保留哪几个点跨平台漂移 → 邻域不同 → 模拟场逐位不同。
  2) `dedupeSamples` 的 `std::sort` 以 (x,y) 精确值为键，完全重合点互为等价 → 合并求均的累加序实现定义 → 重复井位均值末位 bit 跨平台漂移。
  项目自订口径（geostat/sgs.h:16-17、SimulatedLattice 注释「访问序是 2D 逐位结果的组成部分」、#325 同 seed 逐位可复现黄金锚）明确以此为缺陷；#470 只覆盖 std::distribution/shuffle 的序列实现定义，不覆盖本根因（排序/选择的等价类排列）。
- Trigger：SGS（2D/sgs3 3D 同构）在规则网格上有对称等距条件点、邻域规模触顶截断；或输入含重合样本。Linux↔Windows（MSVC STL）对照即暴露。
- Observed vs Expected：期望同 seed 同平台族逐位一致；实际截断边界的并列选择随标准库实现漂移。
- Impact：黄金锚/回归基线跨平台假红或假绿；CI Windows 侧结果与 Linux 不一致被当环境噪声。
- Existing Match：NEW（与 #470 同主题不同根因：#470=分布引擎，本条=排序/选择等价类）。
- Fix Strategy：比较器加确定性次键：`(da, a.sourceOrdinal)`（静态样本用 dedupe 序、已模拟点用格线性序），等价类即消失；dedupeSamples 排序键追加原输入下标作次键。
- Regression Test：构造 4 个等距条件点 + maxPoints=3 的 SGS，断言 Linux 与 Windows（或至少在比较器单测中）选取集合确定；单测断言比较器为严格全序（无等价对）。

## B-04 [P2 / VERIFIED] 属性体时间切片扫描的 worker 线程异常路径未 join——audit-remediation 修复的覆盖缺口

- Module / File:Line / Function：`services/seismictaskservice_slice` / `src/services/seismictaskservice_slice.cpp:760-773` / `SeismicTaskService` 切片属性扫描（processColumn 并行分片）
- Root Cause：与 COMPREHENSIVE_AUDIT 1.1（seismictaskservice_attr.cpp:1021-1032、inversionworkflow.cpp:594）同根因：`std::vector<std::thread> workers` 先派发，主线程段 `runRange(0,…)` 若抛异常（如 bad_alloc / processColumn 内分配），workers 析构未 join → `std::terminate()` 杀死进程。
- 触发实证：已核对 codex/audit-remediation 分支 diff——attr.cpp 的两处（hunk @532、@1019）均已加 RAII join，但 slice.cpp 未被改（`git diff --name-only` 为空）。
- Impact：切片属性扫描（相干/包络等交互路径）在内存压力下整进程崩溃，而非任务失败回执。
- Existing Match：codex/audit-remediation（同根因，属其修复覆盖缺口，非新根因——按排除矩阵记 EXCLUDED_ACTIVE_WORK 的补漏项）。
- Fix Strategy：与 attr.cpp 同款（join 守卫 RAII）；顺手全仓 grep `std::vector<std::thread>` 确认无第四处。
- Regression Test：单测注入抛异常的 runRange，断言进程存活且任务落 Failed（attr 侧同款测试补 slice 版）。

## B-05 [P3 / SUSPECTED] semblanceDimsFit 守卫自身可 i64 溢出回绕

- Module / File:Line / Function：`algorithms/seismicattr` / `src/algorithms/seismicattr.cpp:257-262` / `semblanceDimsFit`
- Root Cause：`static_cast<long long>(nIl) * nXl * nS <= kMax`——三个 int 各可达 2³¹−1，i64 乘积最高 ~9.9e27 ≫ 9.2e18，有符号溢出（UB，实践回绕为负 → `<= kMax` 恒真）→ 守卫被绕过；随后 `nOut = nIl*nXl*nS` int 溢出，`for(i<nOut)` 不跑，但主循环 `il< nIl−ilHalf` 可执行 → 对传入的小 buffer 越界读。
- Trigger：仅恶意/畸形维度参数（调用方现都传真实体维度），纵深防御缺口。
- Observed vs Expected：守卫应无条件在 int 域外拒绝；实际可被构造参数绕过。
- Impact：低（生产调用面收敛）；防御性缺陷。
- Existing Match：NEW（#234-2 是本守卫的来历，未涉自身溢出）。
- Fix Strategy：比较改 `nIl <= kMax/(nXl? …)` 的除法链或 `double` 乘积比较（dims ≤ 2³¹ 时 double 精确到 2⁵³ 足够）。
- Regression Test：单测直接调 semblanceCoherence 传 (INT_MAX, INT_MAX, 3, …) + 1 元素 buffer，断言不读越界（ASAN 下无红）。

## B-06 [P3 / VERIFIED] 层位聚类 TWT 栅格的 nodata 比较未按 float 口径归一（#165 同族残留）

- Module / File:Line / Function：`services/seismichorizoncluster` / `src/services/seismichorizoncluster.cpp:71` / `clusterSeismicHorizon`（TimeRaster 路径）
- Root Cause：`times[cell] == noData`——times 为 float、noData 为 double：非 float 可表示的 nodata（1e30/-99999.9 等）读入 float 后与 double 永不相等 → nodata 格当有效样点进聚类。paleoalgorithms.cpp:97（#165）已确立 `v == static_cast<float>(nodata)` 口径，此处未跟随。
- Impact：外部带非常规 nodata 的 TWT 栅格 → 相聚类混入哨兵值，质心/分组失真（仅该入口）。
- Existing Match：NEW（#165 修的是 paleoalgorithms 同型比较；本处为其残留实例）。
- Fix Strategy：`times[cell] == static_cast<float>(noData)`；顺带全仓 grep `== noData`/`== nodata` 收敛同类。
- Regression Test：写一个 nodata=1e30 的 2×1 GTiff，断言聚类样本数不含 nodata 格。

## B-07 [P3 / VERIFIED] GeologicalSmoothing 无像元预算闸 + 每趟隐式全幅拷贝

- Module / File:Line / Function：`algorithms/paleoalgorithms` / `src/algorithms/paleoalgorithms.cpp:1073-1074, 1152-1159` / `GeologicalSmoothingAlgorithm::processAlgorithm` / `onePass`
- Root Cause：① 输入栅格不经 `PaleoAlgoGuards::gridDimsForExtent`（无 1e8 像元闸），`QVector<float> cur/nxt` 各整幅——外部大栅格（如 20000×20000=4e8×4B×2=3.2GB）直接 OOM；同文件其余算法均有闸。② 每趟 `cur = nxt` 后下一趟 `dst[idx]` 触发 QVector COW detach：正确性无恙（detach 拷贝后 src 冻结），但每趟多付一次全幅拷贝+分配（1e8 格 = 400MB/趟），与「双缓冲」注释的意图相悖。
- Impact：外部输入 OOM 攻击面（自家产物均有闸，故外部直调才触发）+ 多趟平滑的无谓内存 churn。
- Existing Match：NEW。
- Fix Strategy：入口复用 gridDimsForExtent 同口径闸；`std::swap(cur, nxt)` 替代赋值（真双缓冲，免 detach）。
- Regression Test：① 超预算输入抛 QgsProcessingException 的用例；② PASSES=3 大栅格峰值内存对比（或直接断言无每趟重分配——用分配计数钩子）。

## B-08 [P3 / VERIFIED] PaleoTask 进度可报 >100% / ETA 负值

- Module / File:Line / Function：`services/paleotaskservice` / `src/services/paleotaskservice.cpp:72-92` / `PaleoTask::percent / etaText`
- Root Cause：`bytesDone*100/bytesTotal` 分支无 qBound（上游回灌 done>total 时报 101%…）；etaText 的 remaining 为负 → `qMax(1, qRound(secs))` 显示「约 1s」而非钳 0。
- Impact：任务页进度条溢出显示/ETA 抖动，纯表现层。
- Existing Match：NEW。
- Fix Strategy：`qBound(0, …, 100)`；remaining≤0 时显示 `--` 或 0s。
- Regression Test：reportBytes(120,100) 后断言 percent()==100。

## B-09 [P3 / VERIFIED] structural 显式软边界不做域可达过滤——与 R4 停线口径不一致

- Module / File:Line / Function：`algorithms/singlefactor/structural` / `src/algorithms/singlefactor/structural.cpp:433-440` / `buildStructuralSurface`（explicitSoftBoundaries 循环）
- Root Cause：#496 R4 给停线加了「GEOSDistance(line,domain) > softRadius 才跳过」的回执诚实化，但同一函数内 `request.explicitSoftBoundaries` 循环无任何域判定——域外远处的显式软边界照常 appendSoft 消费（数值上同样无作用：软核紧支于半径）。
- Impact：诊断/回执口径不一致（同函数两类软边界一个过滤一个不过滤）；显式边界的 radius 还可大于 softRadiusAuto，保守性更弱。数值结果无影响（紧支撑）。
- Existing Match：NEW。
- Fix Strategy：explicitSoft 也按各自 radius 做 GEOSDistance 可达性判定（>radius 跳过），与停线同构。
- Regression Test：域外 explicitSoft 边界不再出现在 softBoundaryIds/interpretiveBoundaries。

## B-10 [P3 / SUSPECTED] lowFreq 层模式内存无预算闸 + nearestCell O(井×格)

- Module / File:Line / Function：`algorithms/inversion/lowfreq` / `src/algorithms/inversion/lowfreq.cpp:52-74, 282` / `nearestCell` / `lowFreqImpedance`
- Root Cause：① `r.layerImpedance.assign(nCells*nL)` 无闸：25M 格（5000×5000 测网）×10 层 = 1GB 单数组（外加油气体本身）；② `nearestCell` 对每口井全格暴力扫（自注释「井数×格数 ≤ 百万级」假设大测网下不成立：100 井×25M 格=2.5e9 次距离计算，数十秒级主线程外长阻塞）。
- Impact：大三维工区低频模型构建的内存悬崖与分钟级延迟（在任务线程，不冻 GUI 但任务超时体感差）。
- Existing Match：NEW（#468 是 SGS 的组合闸；本处为独立路径）。
- Fix Strategy：nCells×nL 预算闸（或按层惰性求值不物化整块）；nearestCell 换测网标架正反算（il/xl 步长已知，解析最近格 O(1)）。
- Regression Test：构造大网格小层数断言预算拒绝；nearestCell 与解析下标互逆断言。

---

## #496（a1efbd74）专项复核结论（主攻任务）

逐行复核了该提交在算法侧的全部四文件改动，结论：**合并质量高，未发现新缺陷**，仅上述 B-01（该 PR 应统一而未统一的旁路）与 B-09（同 PR 引入的口径不一致）与其直接相关：

1. `paleoalgorithms.cpp`（+103）：breakReceiptIdx 与逐部件回执下标对位正确（receipt 先 append 后取 size()-1）；未落格屏障回执改写 consumed=false/outside_compute_domain 与「不进凸包、PALEO_BREAK_LINES 只计落格数」自洽；mask 前置 + BFS 复用消除二次栅格化；屏障栅格化段的取消 throw 发生在 outDs 创建之前，无 GDAL 句柄泄漏；`comp.resize(cellCount)` 后全量重写，无未初始化读。
2. `singlefactor/structural.cpp`（+15）：`GEOSDistance_r != 0` 判向正确（1=成功/0=异常），失败保守按可达（不丢消费）；`beyondReach = dist > softRadius` 与软核紧支半径语义一致；触域线保留完整折线（不裁剪）避免距离场改变——数值不变性论证成立。唯一不一致见 B-09。
3. `domain/constraintsemantics.cpp`（+19）：legacy hard_barrier / structural soft_boundary 的计划侧域感知（域已知且不触域 → reject outside_compute_domain）与引擎逐线落格回执同口径；最终采用闭矩形契约与 #488 supercover 的 clamp 语义一致。
4. `workflow/surfacegridding.cpp`（+37）：`context.domain` 精确网格域接线正确（QRectF 语义 [originX, originX+cols·dx]×[originY−rows·dy, originY] 与闭谓词匹配）；`inspectHorizonText` 按层位分组 + 散点 bbox 保守下界已在注释/文档明示（勘察-only，回执链不受影响）；O(H×N) 分组对常量级层位数无碍。遗漏点即 B-01（半格超采样本体未换 supercover）与本地 cellOf 缺 #162 远边 clamp（并入 B-01 修复）。

## 审视后无恙的区域（负结论，供交叉核对）

- **任务生命周期（services）**：PaleoTaskService 的 shutdown 排空 + #416 排队丢弃补偿 + applyFinish 终态幂等闸；createTask/onTaskFinished 的同层位引用计数（先终态者不放行）；析构孤儿化语义有注释边界。futuretask.h 的 watcher-destroyed 兜底取消与非可取消观察者托管。jobrunner 的代次+取消标志每代新开。seismictaskservice 的有界 LRU（8 条目、shared_ptr 生命周期延伸过在途 worker）、#416 几何校验、并发闸 startBounded（槽位=4≤池 4，无死锁构型）、属性扫描的顶替（superseded 静默丢弃，迟到结果不假成功）。imagelod 的 epoch 代际 + pending 重入守卫。errorhub 环形+去重索引的换环作废守卫、锁外发信号。petrophyscomputeservice 的 catalog 登记收进服务线程 finished 回调、取消时部分成果如实交付。
- **数值核**：gridsolver（多级粗化、调和带屏障 #293、停滞守卫、幽灵外推、CV 折采样确定论）；variogram（各向异性方位三角、栅格预算闸、取消透传 D3）；kriging OK 方程/variance=Σλγ+μ/负方差拒发；SGS 正态得分域标定 #325、重合点剔除防奇异；constraintbarriers supercover（整数误差项 4-连通遍历、防御性不推进退出）；seismicattr 的解析信号（reflect 镜像以相对偏移为键、NaN 以 0 入谱出口 NaN、全零窗 0/0→NaN）；horizontrack 的 anchor 对齐 #139、死列不复生、确定性合并；lowfreq 半开层区间无缝；surfacevolumes/rasteralgebra NaN 语义。

## 覆盖清单（本 Agent 实读文件）

逐行/近逐行（高风险数值与任务链）：
- src/algorithms：paleoalgorithms.cpp、gridsolver.cpp、constraintbarriers.cpp、mincurvature.cpp、seismicattr.cpp、horizontrack.cpp、velocitymodel.cpp、inversion/lowfreq.cpp、singlefactor/structural.cpp、geostat/{variogram,kriging,sgs,sgs_internal.h,neighborhood.h}.cpp、cluster/cluster.cpp(kmeans/predict 段)、surfacevolumes.cpp、rasteralgebra.cpp
- src/workflow（#496 主攻）：surfacegridding.cpp（全）
- src/services：paleotaskservice.cpp、futuretask.h、jobrunner.cpp、errorhub.cpp、imagelod.cpp、seismictaskservice.cpp、seismictaskservice_slice.cpp（缓存+并行段）、seismictaskservice_attr.cpp（计算尾部~350 行：切片读取/并行/顶替/发布）、petrophyscomputeservice.cpp（批量链尾部）、seismichorizoncluster.cpp（头部数据装载段）
- src/domain（#496 主攻）：constraintsemantics.cpp（diff 段+谓词）
- 对照文档/测试：docs/progress/pr457-closure.md（含剩余风险清单）、tst_velocitymodel.cpp（fit 相关三用例）、tst_singlefactor_constraintreceipts（存在性确认）

结构性略读（grep 定位 + 关键段抽读，未逐行）：geostat/cokriging.cpp（solveAt 全读，known 项排除）、sgs3.cpp、faultsurface.cpp（成面中段：分组/重叠/走向/三角化/拓扑校验）、stratgrid/{upscale,propfill}.cpp（索引与闸 grep）、cluster/{som,supervised}.cpp（#470 域）、inversion/{volume,wavelet,sparse,bandlimit}（排除项或低风险）、petrophys.cpp（#478 覆盖）、welldist.cpp、distancetransform.cpp、dsp/fft.cpp（#478）、curveexpr/faciespolygonize/faciessliver/frameworksuggester/ensemblestats/evolution/faciesmapping/wellsiting（未逐行——未见高风险调用面变化，留待下轮）；services 侧 welllogset/singlefactordef/algoparamschema/crossplots*/datagraph/derivationgraph/evidenceindex/storagegovernance/cataloghealth/pythonenv/scriptrunner/pythonrepl/crashreport/previewdoc*/seismicmapping/faciesclassificationservice（后者被 remediation 持有）未逐行（多数为低数值风险编排/登记面或已持有）。

## 排除记录（本轮不报，根因重合）

SGS 实现×格点内存闸（#468）、cluster 随机分布（#470）、wavelet 主元（#471）、#478 全部 11 项（SGS 每格分配、cokriging 双倍填充/每格分配/重载漂移、零并行、finiteCells 末实现、IDW O(n·m)、swArchie、horizontrack Welford、fft 旋转因子、stratgrid cellIndex int 乘法、sparse 缺失样充 0）、#479 全部 8 项（remotepredictrouter 主线程降级、onnx 同锁、pythonenv 无超时、scriptrunner 半行缓冲、python IO 编码、horizonsuggest O(n²)、wellfaciesservice m_accepted、测试盲区）；COMPREHENSIVE_AUDIT 已收：localidw 张量塌缩、inversion/volume int 溢出、velocitymodel z=0/T=0、constraintparse CRS isValid、faultsurface 走向投影、faultoffset 半平面泄漏、seismictaskservice_attr 线程 join（两处，remediation 已改——其 slice.cpp 漏网记为 B-04）、LIS 补码；audit-remediation 其余已改文件的根因重合项。
