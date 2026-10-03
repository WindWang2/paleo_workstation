# Goal-Loop 方向 18：地质统计学方法包（变差函数 / 克里金 / SGS）

## 背景（实测事实，勿再勘察）

- **克里金系全绿场**：`src/algorithms/` 无 variogram/kriging 任何代码。现有插值核：`constraint_idw`（`paleoalgorithms`）、`singlefactor/localidw`（局部方向 IDW，#109）、`mincurvature`、`rasteralgebra`、`gridsolver`。单因素 TODOS P2 明确递延：「上游井数超过 80 等条件下的各向异性路径会退成 IDW，不能把 UI 标签当成克里金」+ `FaultPathMetric` 断层绕距。
- **方法选择现状**：`constraintpage`/`workflows.cpp` 的插值方法面里，克里金是缺口；编图约束层 `io/constraintstore` 的 lineParams/语义类型映射（`workflows.cpp` 里 `semanticForStoredType`/`lineParamsJson`）已是参数持久化先例。
- **样点抽取先例**：`singlefactor/samples.*`（井点/层位样本采集）、`mappingworkflow` 的 `MappingSamples`。
- **变差函数要的数据**：井/点样对距离-半方差——samples 模块直接供。
- **断层绕距**：`faultset`（棒/切割）+ `faultsurface`（断面 mesh，#111）已落地；`grid_connectivity_v1` 是硬屏障连通先例（栅格 BFS）。FaultPathMetric = 面上任意两点绕断层的测地距离场。
- **产出面**：DERIVED 资产登记、图版发布（`publishLocalDirectionJob` 先例）、progress/ledger 纪律。
- 真工区 env `PALEO_REAL_PROJECT_AREA` 有井数据 + 层位可全链实测。
- 纪律：读 DESIGN.md 再做 UI；每 `src/` 文件三层标记；新顶层模块先 `scripts/new_module.sh`；`add_paleo_test`；`check_layering --strict` 绿；oracle 全是数值断言；不引第三方地统库。

## 目标形态

一个大方法包，四件：**实验变差函数 → 克里金插值 → 序贯高斯模拟 → 断层绕距度量**。

1. **`src/algorithms/geostat/`**（新模块登记）：
   - `experimentalVariogram(samples, lag, nLags)`：方向可选（全向/走向±容差角）；
   - `fitVariogram()`：球状/指数/高斯三模型最小二乘拟合（块金/拱高/变程），拟合优度如实报告；
   - `ordinaryKriging(grid, samples, model)`：局部邻域（最近 K 点或半径内）解克氏方程组——自研小线性求解器（LU/Cholesky，矩阵 ≤ 几十阶，不需要外部库）；输出估值 + 估计方差两张图。
   - `sgs(grid, samples, model, nRealizations, seed)`：序贯高斯——随机路径序贯访问、局部克氏条件模拟、直方图反变换（样本分布忠实）；seed 固定可复现。
   - 断层绕距度量 `FaultPathMetric` **不在本方向**——上游 `single_factor/continuous_metric.py` 的原版实现随方向 23 单因素语义补完一起移植（同一份代码不两遍写）。
2. **方法择优面**：约束/编图方法下拉加「克里金（各向异性）」「SGS 实现族」；井数>80 等路径退化条件按 metric 自动降级并在产物 `extra` 里记 `method_actual`（诚实标注——**不许 UI 标签写克里金结果实为 IDW**，这是 TODOS P2 的原罪条款）。
3. **编排**：`ConstraintWorkflow` 三段式接新算法（prepare/compute/publish 先例）；SGS 多实现跑任务服务逐实现回调进度；等值线/成图管线照旧消费结果栅格。

## Oracle 验收（全部须实测通过并记账本）

1. **变差函数**：合成各向异性场（走向拉长）→ 走向变程 > 倾向变程（比值断言）；恒定场 → 半方差全 0；拟合残差阈值断言。
2. **克里金正确性**（核心断言集）：
   - 样本点处估值 = 样本值（精确断言，克里金固有性质）；
   - 单样本 → 全场均值场；
   - 两等距点中点估值 = 均值（对称性）；估计方差在样本点为 0、远点趋近块金+拱高；
   - 与参考实现/手算小规模情形对拍（生成 4×4 克氏方程组手算断言）。
3. **SGS**：均值/方差直方图与样本分布偏差阈值断言；固定 seed 两次运行逐像素相等；极端情形（单样本）退化如实。
5. **择优诚实面**：井数超阈时产物 `extra["method_actual"]="idw"` 断言；不超阈 = "kriging"。
6. **编排**：DERIVED 登记 + 取消/进度/失败诚实（produce-then-commit 口径）；方法参数入 `lineParams` 持久化往返。
7. **性能**：真工区规模（样本~2k、栅格 1k×1k）克里金 < 60s 比率门记账；SGS 单实现 < 30s。
8. ledger 全账 + `docs/progress/geostat-methods.md`（变程拟合口径、邻域策略、SGS 分布变换法、绕距算法选型、递延：协克里金/泛克里金/交叉验证）。

## 勘察指引

- `src/algorithms/singlefactor/localidw.cpp`（局部邻域插值核的形态）、`samples.*`（样本采集）、`constraintstore.*` + `workflows.cpp` 的参数持久化与三段式
- `src/algorithms/mincurvature.cpp`/`rasteralgebra.cpp`/`gridsolver.*`（栅格 I/O 结构与求解先例）
- `src/domain/faultset.h` + `src/algorithms/faultsurface/`（断层面供绕距）+ `grid_connectivity_v1`（约束面连通先例）
- `src/ui/pages/constraintpage.cpp`（方法下拉落点）、`docs/progress/single-factor-native.md`（本批方法语义口径）
- `tests/tst_singlefactor_parity.cpp` + `tests/fixtures/singlefactor/kernel_parity.json`（对拍式 oracle 先例——本方向可再造 geostat parity fixture）

## 禁区

- 不引第三方地统库（gstat/GSTools/自研矩阵核即可）；不做协克里金/泛克里金/指示克里金——递延写明。
- 不做变差函数交互式拟合 UI（拖动拟合曲线是另一立项）；本轮参数面板给变程/块金/拱高数值输入 + 自动拟合。
- 断层绕距只按「障碍绕行」语义，不做断层封堵性定量（fault seal 另立项）。
- 不碰 `src/catalog`、`src/services/welllogset`（已合）；不碰 `src/workflow/workflows.cpp` 里 single-factor agent 可能仍在迭代的段落范围之外的无关重构。
- 不破坏现有 IDW/constraint 行为——新方法走新枚举值，默认行为不变。

## 迭代协议

- **轮0**：勘察定案——样本结构（samples.* 实际字段）、栅格对象类型、约束参数面接点；变差函数 API 签名进 ledger。
- **轮1**：`geostat` 变差函数 + 拟合 + `tst_geostat_variogram`。
- **轮2**：ordinary kriging + 核断言集 `tst_geostat_kriging`。
- **轮3**：SGS + `tst_geostat_sgs`（可复现性断言）。
- **轮4**：编排接三段式 + 方法择优 + DERIVED + `tst_geostat_workflow`。
- **轮5**：真工区 + 性能记账 + progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/geostat -b goal/geostat-methods-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：`git diff origin/master...HEAD` 全量自审；无调试残留；层标记齐；`check_layering --strict` 绿；vendor 前缀全量构建零新警告；ctest 全绿；Oracle 每条有命令+输出证据。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
