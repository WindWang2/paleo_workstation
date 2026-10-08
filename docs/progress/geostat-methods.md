# 方向18：地质统计学方法包（变差函数 / 克里金 / SGS / 断层绕距）

## 范围

- 分支：`goal/geostat-methods-20261003`，worktree `.worktrees/geostat`。
- 基线：origin/master `4741115`。
- 方案：`goal-loop-prompts/18-geostat-methods.md`；ledger：
  worktree 根 `.goal-loop-ledger-geostat-methods.md`。
- 交付：`src/algorithms/geostat/`（variogram / linsolve / neighborhood /
  kriging / sgs / faultpath）+ ConstraintWorkflow 三段式编排 + 约束页方法面
  + 7 个测试（5 功能 + 1 性能 + 1 真工区门控）。

## 口径（关键取舍）

### 变程拟合

- 实验变差函数：O(n²) 全对累积，方向 = 全向或方位角±容差（azimuth 从北
  顺时针，h 与 -h 计同一对）。lag 距离-半方差按 bin 均值报告。
- 拟合：固定 range 下 (nugget, sill) 是线性参数（2×2 正规方程 + 非负
  钳制），range 对数扫描 121 档 + 最优档局部细化。指数/高斯 range 口径
  = 实用变程（~95% 基台）。拟合优度如实：rmse + r2（r2 可为负）。
- 恒定场（零信号）：半方差全 0 → 零参数模型（不虚构变程），r2 报 0。
- **sill 语义 = 拱高（partial sill），总基台 = nugget + sill**。
- 各向异性：几何各向异性（有效距离口径，`semivariance(dx,dy)`）——
  编排层 azimuth ≥ 0 时双方向（az、az+90）拟合，ratio = clamp(沿/垂, 1, 8)。
  UI 标签「克里金（各向异性）」因此是真的各向异性，不是标签修辞。

### 克里金（OK）

- 邻域：桶格索引（side=⌈√n⌉）环形扩张 + max-heap 最近 K（默认 16，
  上限 64）；半径模式另有 minPoints 覆盖闸（最近 K 模式不设下限）。
- 方程组：n+1 加边，自研 LU 列主元求解器（`linsolve.cpp`）。**为什么
  不是 Cholesky**：OK 加边矩阵 [[Γ,1],[1ᵀ,0]] 因 γ(0)=0 对角与 Lagrange
  行不定（conditionally PD 非 PD），Cholesky 不适用——头注释写明。
- 方差口径：σ² = Σλᵢγ(xᵢ,x₀) + μ。**单样本远点渐近 = 2×(块金+拱高)**
  （均值未知的代价）。提示词原文「远点趋近块金+拱高」是简单克里金
  （已知均值）口径，严格 OK 下不成立——按数学事实断言并在
  tst_geostat_kriging 注释说明。
- 重合样本 ε 合并取均值（防奇异）；显著负方差计数为数值失败不静默输出。
- 4×4 手算对拍：纯块金闭式 λ=1/n、μ=C(n−1)/n、σ²=C(1+1/n)；
  2 样本不对称 3×3 手工代数解对拍（测试内独立推导）。

### SGS

- 正态得分变换：(i+0.5)/n 分位（同值并列平均秩），Φ⁻¹ = **erf 二分
  100 迭代**（单调可移植双精度饱和）。**不用有理逼近**：轮3 手打 Acklam
  尾部系数错误，p>0.76 时 Φ⁻¹→e300 级联污染全场（ledger 有纠错记录）。
  教训：数值常数不凭记忆。
- 条件模拟：高斯域 SK（C=S−γ，LU），邻域 = 静态样本 + 已模拟格合并；
  **与静态样本重合的已模拟点剔除**（γ(0)=0 使重合 C 两行相同 → 奇异，
  条件值已由静态样本承载）。
- 反变换：经验分位数表线性内插 + 端部末段斜率外推（直方图忠实样本
  分布）；单样本退化为常值场（如实退化）。
- 随机数：mt19937_64 原始 u64 → 53bit (0,1) → Box-Muller（**不用
  std::normal_distribution**——其序列实现定义，跨平台不可复现）。同
  seed 逐位相等；随机流跨实现连续（Fisher–Yates 路径 + 逐格抽样）。

### 断层绕距（FaultPathMetric）

- 障碍栅格化：多边形边界半像元步进（support.cpp labelHardBarriers 同
  口径，细墙不丢）+ 内部格心 even-odd 扫描线填充（bbox 限定），holes
  挖空。
- 测地距离：**8 邻接 Dijkstra**（各向异性边权：横 dx、纵 |dy|、对角
  hypot）。取舍：快速行进（eikonal）精度略高，但均匀介质下 8 邻接与
  库内 distancetransform.cpp 先例同口径、简单可验；误差上界为斜向
  chamfer 偏差 ~8.2%（oracle 用 9% 门 + 轴向精确断言）。
- 屏障格/不可达格 NaN + 计数如实；源点落屏障 ≤5 格螺旋外搜。
- 语义边界：只做「障碍绕行」距离，不做断层封堵性定量（fault seal
  另立项，方案原文）。

### 方法择优诚实面（原罪条款）

- 降级 metric：**有效样本 < 8**（kGeostatMinSamples，变差函数欠定）→
  compute 段自动降级 paleo:paleo_constraint_idw，产物
  `extra["method_actual"]="idw"` + `algorithm_id` 如实换引擎 id。
- 「井数>80 降级」的历史原罪属**既有**单因素各向异性路径（TODOS P2）；
  本方向克里金对大样本量无悬崖（局部邻域 K=16 封顶，实测 2000 样本
  4.3s/1M 格），降级条件是样本不足而非过量。机制（method_actual 诚实
  标注）与验收条款一致，阈值两侧均有断言。

### 编排

- `GeostatJob` 三段式（LocalDirectionJob 同构）：prepare 只解析 URI/参数
  （GUI 线程）；compute 任务线程纯数值 + GDAL 写 paleo-gs-* 临时目录；
  publish 回 catalog 线程 stage/copy/sha256/commitExternal/declare。
- produce-then-commit：generation 三检，代次变即丢临时目录；取消在
  compute 返回失败且不外泄半成品。
- SGS 逐实现进度：核内 progress 按（实现序号+格进度）/总实现单调推进，
  任务服务壳按既有口径节流上报。
- 栅格预算：工作流层 4M 格闸 + 核层 1e8 格闸。
- 约束线与克里金：方向84 起方向线/软边界经 `PairMetricWarp` 进克里金半方差
  （消费矩阵见下「方向84」节）；SGS 仍不消费约束线（v1 语义：硬屏障连通/
  绕距是独立度量，见 faultpath），产物 extra 如实不含屏障计数。
- **「方法参数入 lineParams 持久化往返」的实现口径**：方法级参数（不是
  逐线语义参数）走 params → QC `parameters` 节 → 参数指纹 → catalog
  extra 落盘，测试断言写读一致（tst_geostat_workflow）。逐线
  params_json 的语义白名单（knownConstraintSemantic）未扩展——往里面塞
  方法级键会破坏逐线 schema 的兼容承诺，属另一变更面。

## 本机命令

```
cmake --build build --target tst_geostat_variogram tst_geostat_kriging \
  tst_geostat_sgs tst_geostat_faultpath tst_geostat_workflow \
  tst_geostat_perf tst_geostat_realarea -j16
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^tst_geostat_' --output-on-failure
python3 tools/check_layering.py --strict
```

2026-10-03 结果：7 项中 6 项通过（variogram 0.3s / kriging 0.3s / sgs 0.4s /
faultpath 0.3s / workflow 3.1s / perf 7.8s），tst_geostat_realarea QSKIP
（PALEO_REAL_PROJECT_AREA 未设置——跳过不是通过，真工区门不记通过）。

## Oracle 对账（逐条）

1. **变差函数**：走向变程 > 1.5×倾向变程（各向异性卷积场，实测 ~3×）；
   恒定场半方差全 0 + 零参数模型；高斯拟合 r2 ≥ 0.9、rmse ≤ 0.08×总基台
   （tst_geostat_variogram，含各向异性模型单元断言）。
2. **克里金**：样本点精确插值（1e-9）+ 方差 0；单样本常值场 + 远点
   2×(nugget+sill)（1e-6）；两等距中点 = 均值 + 闭式方差；4×4 纯块金
   手算；2 样本不对称手算；方差随距递增；半径模式 nodata；重合合并；
   取消/无效（tst_geostat_kriging 12 用例）。
3. **SGS**：同 seed 逐位相等 + 异 seed 相异；对数正态偏斜样本直方图
   忠实（均值 ≤0.2σ、std 比 [0.75,1.35]）；样本格条件化；单样本退化；
   零基台/取消/多实现（tst_geostat_sgs 8 用例）。
4. **FaultPathMetric**：无障碍轴向精确=欧氏、斜向 ≤9% chamfer 门；直墙
   绕行 > 直线且贴解析值 2√(21²+20²)=58（±9%）；环洞内不可达；源点
   落墙外搜；取消/无效（tst_geostat_faultpath 7 用例）。
5. **择优诚实面**：4 井 → `extra["method_actual"]="idw"` +
   algorithm_id=paleo:paleo_constraint_idw + 无方差旁路；25 井 →
   "kriging"（tst_geostat_workflow::insufficientSamplesDegradeToIdwHonestly
   / krigingFullChainRegistersDerived）。
6. **编排**：DERIVED single_factor_raster 登记 + factor 声明 +
   QC/参数指纹/extra 往返断言（azimuth/cell_size/variogramModel/seed/
   realizations 读写一致）；取消诚实（compute 失败 outputPath 空、
   publish 拒绝）。
7. **性能**：见下。

## 性能记账

合成口径（tst_geostat_perf，RelWithDebInfo，本机 16 核；比率门断言，
绝对值 BASELINE 行人工誊入）：

```
BASELINE geostat_kriging_samples = 2000
BASELINE geostat_kriging_ms_1024 = 4291     # 1024×1024，K=16
BASELINE geostat_kriging_ms_256 = 294
BASELINE geostat_kriging_scale_ratio = 14.60  # 门 ≤ 24（像元比 16×1.5）
BASELINE geostat_sgs_ms_512 = 2426
BASELINE geostat_sgs_ms_256 = 594
BASELINE geostat_sgs_scale_ratio = 4.08       # 门 ≤ 6（像元比 4×1.5）
```

- 真工区规模（样本~2k、栅格 1k×1k）克里金 **4.3s**，对 60s 验收门余量
  14 倍；SGS 单实现 512² 2.4s 外推 1k×1k ~10s 量级，对 30s 门余量充足。
- 真工区实测（tst_geostat_realarea，60s/30s 绝对门 + BASELINE 行）：
  **本机未设置 PALEO_REAL_PROJECT_AREA，QSKIP——不记通过**。

## 递延（未做，如实）

- 泛克里金 / 指示克里金（方案禁区原文递延）。**协克里金已由方向84 接线**
  （`paleo:paleo_local_direction_cokriging`，见下「方向84」节）。
- 交叉验证（留一法）面——gridsolver 有 `crossValidateLeaveOneOut` 先例，
  geostat 侧未接。
- 变差函数交互式拟合 UI（拖动拟合曲线是另一立项；本轮参数面板 =
  数值输入 + 自动拟合）。
- 断层绕距与克里金邻域的正向耦合（「断层两侧不互用邻域」的距离语义
  已由 faultPathMetric 供数，编图链消费属后续方向）；fault seal 定量
  另立项。
- SGS 逐实现独立资产发布（当前产物 = 实现均值场 + 实现间标准差旁路）。
- SGS 求解不考虑约束线屏障（v1 语义边界，extra 如实）。
- 真工区实跑（待 PALEO_REAL_PROJECT_AREA 可用环境）。

## 方向84：约束语义 × 方法消费矩阵（克里金族收口）

同批约束线在各成图方法下的真实消费方式（issue 文案与这里逐条对应；
`method_actual`/`fallback_reason` 先例不变）：

| 约束语义 | local_direction_idw | local_direction_kriging | cokriging（协克里金） |
|---|---|---|---|
| HardBarrier | 栅格连通分量隔离（两侧井不共享） | 同左 + 变差拟合隔断感知（测地滞后距，跨隔断对不进结构估计） | 同 local_direction_kriging（分量隔离；γ22 拟合 v1 未隔断感知） |
| DirectionGuide | CurveKernel 局部张量改写 IDW 距离（tangentEnergy 扣减） | 同公式改写克里金**半方差点对距离**（`PairMetricWarp`，井-井与井-查询同变换） | v1 不消费（issue 如实记 `*_not_used_by_cokriging`） |
| InterpretiveBoundary | 跨线井权重 ×(1−penalty) | 跨线点对距离 ×1/√(1−penalty) 进半方差（IDW 衰减的等效距离口径） | v1 不消费（同上） |
| wellClusterLocality | 去簇权重乘子 | 不消费——克里金权重是无偏约束下的最小方差解，去簇乘子破坏最优性；issue 写明原因 | 同左 |
| ContourStop / CartographicDetour | 制图侧消费（不进分析场） | 同左 | 同左 |

- **实现锚点**：`geostat::PairMetricWarp`（kriging.h，solveAt 三参重载）→
  `localidw.cpp` evaluateBatch 的 krigingMetric 闭包（公式与 IDW 权重路径
  逐条同源）→ `krigingsurface.cpp` 逐条消费回执
  （`direction_guide_consumed_by_kriging_metric` 等）。
- **病态口径**：warp 后方程组奇异/病态走既有 LU 失败 → `idwFallbackCells`
  逐格回落（不新增降级机制）；精确通过井点性质保持（零位移 warp 后仍零）。
- **协克里金**：MM1 交叉模型（γ12 = ρ·γ1），secondary = 协变量栅格井位
  采样（CRS 不一致经 QGIS 变换），γ22 自动拟合；缺协变量 = InvalidInput
  如实拒绝（不回落冒充——回落产物不含协变量信息）。ρ=0 严格退化为普通
  克里金（面级对拍回归）。
- **ConstrainedKrigingSolver（方向67 交付）销账**：其 `WeightGroupConstraint`
  （组权重上限软罚）的三个目标语义均已有更原生表达——硬屏障 = 分量隔离
  （比组 cap=0 更强：跨隔断井根本不进方程组）、软边界 = 距离放大 warp
  （连续语义，无需主动集迭代）、方向线 = 张量 warp；且 OK 权重可负，
  「组权重上限」对负权重不再表达「限制该组贡献」。求解器与回归保留
  （可复用数值资产），TODOS 递延项划掉，不再计划独立接线。

## 尚未完成

- 真工区门（见上，跳过不是通过）。
- UI 真人点击面（离屏参数面已随 constraintpage 编译进 paleo_ui；方法
  下拉/参数 spin 无独立 UI 测试——现有 tst_ui 伞式是否覆盖待下轮核实）。
