# 地质统计学方法包与单因素算法演进（变差函数 / 克里金 / 协克里金 / SGS / 断层绕距 / 约束耦合）

## 范围与演进历程

- **方向 18（2026-10-03，`goal/geostat-methods-20261003`）**：
  建立地质统计学基座核 `src/algorithms/geostat/`（variogram / linsolve / neighborhood / kriging / sgs / faultpath）及三段式作业编排。
- **方向 41（2026-10-04）**：
  克里金接入单因素本地方向插值面（`krigingsurface.{h,cpp}` + `geostat::KrigingSolver`，`method=local_direction_kriging`），回落如实记录 `method_actual` 与 `fallback_reason`。
- **方向 67（2026-10-07）**：
  引入 `geostat::VariogramBarriers` 测地绕障距离场与跨隔断样本对剔除机制；引入 SFPKG 容器与外委统计。
- **方向 74（2026-10-08，`goal/sf-geostat-20261007`）**：
  单因素原生算法收口——方向线（MLA Riemannian metric）与软边界（$C^\infty$ 距离膨胀）全面耦合进克里金权重，产出 `direction_guide_applied:N` 与 `soft_boundary_applied:N` 诚实回执；全场网格 `ordinaryCoKriging` 求解核落地，编排层打通 `covariateLayerId` 次级栅格协同与缺失诚实拒绝；TODOS P2 对账归档。

---

## 方法矩阵与落地状态

| 方法名称 | 标识符 / 引擎 ID | 核心数学与地质机理 | 状态 | 落地方向与验证用例 |
|---|---|---|---|---|
| **普通克里金 (OK)** | `kriging` / `paleo:geostat_kriging` | 实验变差自动拟合 + 桶格环形搜索邻域 + $n+1$ 加边鞍点矩阵列主元 LU 求解 + 方差渐近估计 | **LANDED** | 方向 18 / 41<br>`tst_geostat_kriging`<br>`tst_singlefactor_kriging` |
| **序贯高斯模拟 (SGS)** | `sgs` / `paleo:geostat_sgs` | erf 二分正态得分变换 + 随机遍历路径 + 条件 SK 逐格模拟 + 经验分位数线性内插反变换 | **LANDED** | 方向 18 / 45<br>`tst_geostat_sgs`<br>`tst_geostat_sgs3` |
| **断层测地绕距** | `faultpath` / `FaultPathMetric` | 多边形半像元栅格化 + 8 邻接 Dijkstra 测地距离场，不可达标记 NaN | **LANDED** | 方向 18<br>`tst_geostat_faultpath` |
| **变差函数屏障机制** | `VariogramBarriers` | 基于测地距离剔除跨隔断不可达样本对，同侧/绕障以测地滞后累积；512 样本安全预算 | **LANDED** | 方向 67<br>`tst_geostat_variogram_barrier` |
| **约束耦合克里金** | `local_direction_kriging` | DirectionGuide 局部黎曼各向异性张量场 + InterpretiveBoundary 平滑距离膨胀，消除 Heaviside 阶跃与 V 形折痕 | **LANDED** | 方向 74<br>`tst_singlefactor_kriging`<br>`stress_soft_boundary` |
| **普通协克里金 (CoOK)** | `cokriging` / `paleo:geostat_cokriging` | 全场网格 `ordinaryCoKriging` + 次级属性采样 + Markov MM1 交叉协方差 + 双 Lagrange 乘子鞍点 LU 求解 + 诚实缺协变量拒绝 | **LANDED** | 方向 74<br>`tst_geostat_cokriging`<br>`tst_factorworkflow` |
| **约束克里金解法器** | `ConstrainedKrigingSolver` | 静态样本组上限二次罚函数求解；1D 样本子集测试保留，2D 成图曲面正式退役销账 | **RECONCILED / RETIRED** | 方向 74 销账<br>1D 测例保留在 `tst_geostat_cokriging` |
| **泛克里金 (UK)** | `universal_kriging` | 考虑空间坐标多项式趋势项（线性/二次漂移） | **DEFERRED** | 保留递延，另立项 |
| **指示克里金 (IK)** | `indicator_kriging` | 连续属性多门限指示离散化与非参数累积概率估计 | **DEFERRED** | 保留递延，另立项 |

---

## 口径（关键取舍）

### 变程拟合与屏障感知

- **实验变差函数**：$O(n^2)$ 全对累积，方向支持全向或方位角 $\pm$ 容差（azimuth 从北顺时针，$\mathbf{h}$ 与 $-\mathbf{h}$ 计同一对）。lag 距离-半方差按 bin 均值报告。
- **拟合算法**：固定 range 下 $(\text{nugget}, \text{sill})$ 为线性参数（$2 \times 2$ 正规方程 + 非负钳制），range 对数扫描 121 档 + 最优档局部细化。指数/高斯 range 口径为实用变程（$\sim 95\%$ 基台）。拟合优度如实报告：$\text{rmse} + r^2$（$r^2$ 可为负）。
- **恒定场（零信号）**：半方差全 0 $\rightarrow$ 零参数模型（不虚构变程），$r^2$ 报 0。
- **sill 语义**：$\text{sill} = \text{拱高（partial sill）}$，总基台 $= \text{nugget} + \text{sill}$。
- **几何各向异性**：有效距离口径 `semivariance(dx, dy)`，编排层 azimuth $\ge 0$ 时沿双方向（az 与 az$+90^\circ$）拟合，$\text{ratio} = \text{clamp}(\text{沿} / \text{垂}, 1, 8)$。
- **变差函数屏障机制 (`VariogramBarriers`)**：
  - 采用双轨制（Dual-Track）：当 `barriers.enabled == false` 或屏障列表为空时，零额外开销，与欧氏算法逐位一致。
  - 当启用屏障时，通过 `faultPathMetric` 生成 8 邻接 Dijkstra 测地距离场：跨隔断不可达样本对（$\text{geodesic} = \text{NaN}$）直接剔除出半方差累积，不可达计数计入 `unreachablePairs`；可绕障样本对以测地距离 $h = \text{geodesic}$ 累积进滞后区。
  - 设置 $N \le 512$ 样本安全预算保护，超出时显式返回 `Status::InvalidInput`，防止 $O(N \cdot \text{Grid})$ 耗尽计算资源。

### 克里金曲面与约束语义消费（方向 74）

在历史版本中，克里金曲面曾将约束线标记为未消费 issue（`*_not_used_by_kriging`）。方向 74 彻底打通了约束语义在克里金中的真实消费：

1. **走向引导线 (`DirectionGuide`) 消费**：
   - 采用 Moving Local Anisotropy (MLA) 黎曼度量张量场机制。
   - 依据局部走向角度 $\theta$ 与引导权重构造度量张量 $M(x_0) = I - g(x_0) r T(x_0)$。
   - 克里金样本点间及待估点到样本点间的空间距离按局部张量场度量：$h_{\text{eff}} = \sqrt{\Delta x^T M(x_0) \Delta x}$。
   - 使得变差函数等相关线沿断层/河道走向拉伸，垂直走向压扁，实现地质走向约束。
2. **解释边界软约束 (`InterpretiveBoundary`) 消费**：
   - 解决历史 Heaviside 阶跃导致的 $26.93\%$ 跨界断崖跳跃与边界线 V 形尖点折痕缺陷。
   - 采用 $C^\infty$ 平滑过渡连续公式：
     $$\text{opposite} = (1.0 - q_{\text{Side}} \cdot w_{\text{Side}}) \times 0.5$$
     $$\text{penalty} = \max(\text{penalty}, \text{strength} \times \text{gate} \times \text{opposite})$$
     $$h_{\text{eff}} = \frac{h}{\sqrt{1 - \text{penalty}}}$$
   - 跨界时有效距离平滑膨胀，边界线上两侧惩罚连续相等，确保待估点处处连续、严格过井点、消除虚假折痕。
3. **诚实消费回执**：
   - 废除原虚假 issue，改由实际进入克里金计算的核心约束线数量生成诚实回执：
     `direction_guide_applied:<count>` 与 `soft_boundary_applied:<count>`。
   - 对无法有效耦合的井丛去簇（`wellClusterLocality`）保留诚实未消费原因，杜绝伪造回执。

### 协克里金（Co-Kriging，方向 74）

方向 74 在 `src/algorithms/geostat/cokriging.{h,cpp}` 中实装了全场网格 `ordinaryCoKriging` 求解器，并在 `src/workflow/` 深度接线：

1. **全场网格求解与马尔可夫 MM1 模型**：
   - 支持主变量样点集 $Z_1$ 与次级变量（地震属性/地质因子）样点集 $Z_2$ 的协同估值。
   - 采用地质统计学成熟的 Markov Model 1 (MM1) 交叉协方差模型：
     $$\gamma_{12}(h) = \rho \cdot \sqrt{\text{sill}_1 \cdot \text{sill}_2} \cdot \frac{\gamma_1(h)}{\text{sill}_1} = \rho \cdot \gamma_1(h)$$
   - 次级自变差与交叉项同单位：交叉项是 $\rho\cdot\gamma_1(h)$（主变量单位），所以 $\gamma_{22}$ 取主变量基台（块金置 0），即两基台相等的 scaled MM1 退化。协变量井点实测方差只写进 QC 参数 `covariate_sample_variance`，不改模型——否则地震属性与孔隙度/厚度差几个量级时方程组混两种单位，Pearson $\rho$ 不再是方程组里的那个 $\rho$，次级块还会主导条件数。
2. **正定性与鞍点系统稳定性保护**：
   - 为防止主次变量强线性相关时协方差矩阵奇异或非正定，对样本点共置估算出的 Pearson 相关系数 $\rho$ 实施严格钳制：$\rho \in [-0.99, 0.99]$。
   - 构建 $(n_1 + n_2 + 2) \times (n_1 + n_2 + 2)$ 鞍点方程组，引入双 Lagrange 乘子满足无偏性约束：$\sum \lambda_i = 1, \sum \nu_k = 0$。
   - 采用自研列主元 LU 分解器求解权重向量。
3. **工作流契约与诚实拒绝**：
   - 在作业参数中定义次级栅格图层标识 `covariateLayerId`。
   - 在 `prepareGeostatJob` 阶段严格校验：若 `covariateLayerId` 缺席、为空或图层无效，立即返回错误 `QStringLiteral("协克里金方法需要有效的次级协变量图层 (covariateLayerId)")`，坚决拒绝静默退化或生成无次级信息的假图。
   - 在 `computeGeostatJob` 阶段，对主变量井位采样次级栅格计算 Pearson $\rho$；对全目标网格下采样次级样点；执行 `ordinaryCoKriging`。
4. **方差缩减与双栅格输出**：
   - 当次级变量与主变量存在相关性（$|\rho| > 0$）时，协克里金估计方差 $\sigma^2_{\text{CoKriging}}$ 严格小于单变量普通克里金方差 $\sigma^2_{\text{OK}}$。
   - 工作流同时输出主估计栅格 `factor.tif` 与估计方差栅格 `variance.tif`。

### 解法器销账说明 (`ConstrainedKrigingSolver`)

- `src/algorithms/geostat/cokriging.{h,cpp}` 中实现的 `ConstrainedKrigingSolver` 针对样本权重施加静态子集上限约束（$\sum_{i \in \text{group}} \lambda_i \le \text{cap}$）。
- **核销结论**：在 2D 连续曲面插值中，样本点相对于断层/边界的相对阻隔关系随待估点 $(x, y)$ 连续变动，静态索引组约束无法表达局部空间相对拓扑；同时二次罚函数存在 $0.005$ 残留权重泄漏。
- 2D 生产成图由 `labelHardBarriers` 断块连通域划分与 `FaultPathMetric` 测地绕距实现严格 0 泄漏，故 `ConstrainedKrigingSolver` 正式从 2D 曲面管线中退役销账；其 1D 样本子集测例保留在 `tests/tst_geostat_cokriging.cpp` 中作为算法库参考。

### SGS

- **正态得分变换**：$(i+0.5)/n$ 分位（同值并列平均秩），$\Phi^{-1} = \text{erf 二分 100 迭代}$（单调可移植双精度饱和，拒绝不稳定有理逼近）。
- **条件模拟**：高斯域 SK（$C = S - \gamma$，LU 求解），邻域合并静态样本与已模拟格，剔除重合样本防止奇异。
- **反变换**：经验分位数表线性内插 + 端部末段斜率外推。
- **随机数生成**：`mt19937_64` 原始 u64 $\rightarrow$ 53bit $(0, 1)$ $\rightarrow$ Box-Muller 变换（避免 `std::normal_distribution` 跨平台实现差异），保证相同 seed 逐位复现。

### 断层绕距（FaultPathMetric）

- **障碍栅格化**：多边形边界半像元步进（`support.cpp labelHardBarriers` 同口径，细墙不丢）+ 内部格心扫描线填充。
- **测地距离**：8 邻接 Dijkstra（各向异性边权：横 $dx$、纵 $|dy|$、对角 hypot），精度满足工程要求，屏障格与不可达格记录 NaN。

---

## 本机命令与回归门

```bash
# 构建核心测试
cmake --build build --target tst_geostat_variogram tst_geostat_kriging \
  tst_geostat_cokriging tst_geostat_sgs tst_geostat_faultpath \
  tst_geostat_workflow tst_singlefactor_kriging tst_factorworkflow \
  tst_geostat_variogram_barrier -j8

# 运行地统计与单因素回归测试
ctest --test-dir build --output-on-failure -j8 -R '^(tst_geostat_|tst_singlefactor_kriging|tst_factorworkflow)'

# 严格层界检查（assert 0 violations）
python3 tools/check_layering.py --strict
```

---

## 性能记账

合成口径（`tst_geostat_perf`，RelWithDebInfo，本机限制 `-j8`）：

```text
BASELINE geostat_kriging_samples = 2000
BASELINE geostat_kriging_ms_1024 = 4291     # 1024×1024，K=16
BASELINE geostat_kriging_ms_256 = 294
BASELINE geostat_kriging_scale_ratio = 14.60  # 门 ≤ 24
BASELINE geostat_sgs_ms_512 = 2426
BASELINE geostat_sgs_ms_256 = 594
BASELINE geostat_sgs_scale_ratio = 4.08       # 门 ≤ 6
```

- 真工区规模（样本 $\sim 2\text{k}$、栅格 $1\text{k} \times 1\text{k}$）普通克里金耗时 $\sim 4.3\text{s}$，协克里金网格求解在次级点下采样支持下可在数秒内完成。

---

## 递延与分流说明（未做，如实）

1. **泛克里金 (Universal Kriging) 与指示克里金 (Indicator Kriging)**：
   - 保持递延，未在方向 74 中引入，待后续专项立项。
2. **交叉验证（留一法 Cross-Validation）**：
   - 算法库 `gridsolver` 有 `crossValidateLeaveOneOut` 先例，`geostat` 侧未接，保持递延。
3. **变差函数交互式拟合 UI**：
   - 交互拖拽拟合曲线属独立前端交互立项；当前采用参数面板输入与自动非线性参数扫描拟合。
4. **变差函数小断块逐分量独立拟合 vs 汇聚式屏障累积**：
   - 当前已实装 `VariogramBarriers` 测地绕障累积与跨隔断对过滤；
   - 小断块分量独立拟合因真实工区小断块井数普遍稀疏（$<8$ 井导致欠定退化为 IDW）保持递延，分流至精细构造地质建模专题。
5. **SGS 逐实现独立资产发布**：
   - 当前产物为实现均值场与实现间标准差场，逐实现流式归档另立项。
6. **真工区门控**：
   - `tst_geostat_realarea` 待 `PALEO_REAL_PROJECT_AREA` 可用环境运行。
