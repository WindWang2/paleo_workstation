# Goal-Loop 方向 45：属性建模 V2——断块错位网格 + 序贯高斯/相控建模

## 背景（实测事实，勿再勘察）

V1 已落等比例 IJK 格架 + 井曲线粗化 + 断层竖帘阻断的 IDW
（`src/algorithms/stratgrid/`、`src/workflow/propertymodelworkflow.*`，
壳层「属性建模」dock 已接）。递延（TODOS P3）：

- pillar/断块错位网格、Y 型断层、沉积相带/对象建模、序贯高斯模拟、
  变差函数克里金、斜井测斜表、断层棒投影均未做。
- **SGS/变差核已在 master（勿重写）**：`src/algorithms/geostat/`
  已落 `variogram.cpp`（变差拟合）+ `kriging.cpp`（OK/LU 解）+
  `sgs.h`（序贯高斯：nRealizations/seed/SK 邻域/分位数反变换，
  mt19937_64 跨平台可复现）——现消费方是 singlefactor 地图面。
  **本方向是把 geostat 核接进 stratgrid IJK 格架**（条件点=粗化
  井柱 cell，序贯路径走三维网格，不是写新模拟核）。
- **本方向扩量约定（无逃逸口）**：对象建模**必做**（河道/点坝
  最小骨架起步）；斜井测斜表消费**必做**（`parseDeviationText`
  已落 io/，直接消费；无契约则如实标「竖直近似」）；
  Y 型断层若方向 40 已落分叉断面则接入，未落则 ledger 记依赖。
- 断层竖帘阻断（grid_connectivity_v1）是硬屏障——断块错位是
  网格几何层面的错位（层位/断距驱动），与连通屏障不同层：
  勘察 stratgrid 网格表示可挂点。
- 斜井测斜表/断层棒投影是数据源级依赖——无契约则递延记档，
  不造数据。
- 相控建模：沉积相带（facies 词表/方向 27 自动编图产物）作
  软/硬约束分区——勘察 catalog 相带资产可消费面。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/prop-model-v2 -b goal/prop-model-v2-20261004 origin/master
cd .worktrees/prop-model-v2
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **断块错位网格**：pillar 修正——断距矢量驱动网格错位，
   跨断块 cell 不连通（勘察 stratgrid 结构）；无断距数据的断层
   保持竖帘行为并标口径。
2. **SGS 接入 stratgrid（不是写核）**：geostat/sgs 核复用——
   条件点=粗化井柱 cell、序贯路径走三维网格、相带分区参数域；
   随机种子参数化沿用核内 seed 契约（可复现），
   多个 realization 各落独立 DERIVED 版本。
3. **相控约束**：相带分区内的参数域分离（各带独立变差/均值面）；
   无相带资产走全域单一模型并在参数面板如实标口径。
4. **对象建模（必做）**：河道/点坝对象放置最小骨架——对象几何
   参数化（走向/宽度/厚度/曲率）、放置规则（相带内播种）、对象
   属性场覆盖网格 cell；对象边界与 SGS 场的优先关系写死口径。
5. **斜井消费（必做）**：测斜表/deviation 数据驱动的井轨迹粗化——
   斜井轨迹穿网格按真实三维路径取 cell，不按竖直投影近似；
   无测斜数据井如实标「竖直近似」口径。
6. **消费面**：错位网格与 SGS/对象结果入既有属性体容器/dock
   显示——不新造呈现层；Y 型断层断面若方向 40 已落则接入
   错位网格，未落记依赖不自制。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；网格/模拟核归
  algorithms，编排归 workflow，dock 归视图层。
- **诚实面**：无断距/无相带/无变差数据各口径如实标注；
  模拟结果带随机种子与参数 provenance，不冒充确定性解。
- **资源**：构建/测试一律 `-j8`。
- **数值正确性**：SGS 在井点精确复现硬数据；大量 realization 的
  均值/变差收敛到模型参数（统计断言带容差）。
- **性能断言**：禁绝对毫秒墙钟；网格规模比率门，邻域搜索限界。
- **冲突**：stratgrid/propertymodelworkflow 热区注明；与 41 的
  变差模块归属按「先落地者为准」在 ledger 记接口契约。
- **ledger**：`.goal-loop-ledger-prop-model-v2.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/网格几何/
  统计收敛/诚实口径/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 断块错位：含断距合成断层的网格在断线两侧错位量化正确，
   跨断块插值零泄漏（cell 不连通断言）。
2. SGS 硬数据：井点处所有 realization 精确复现输入值。
3. 统计收敛：N 个 realization 的均值面/变差面收敛到合成模型
   参数（容差断言，种子可复现）。
4. 相控：两相带合成场景各带独立统计，带界处不混参数。
5. 对象建模：河道对象放置后其几何参数（宽度/走向）在属性场
   可复现；对象内 cell 标记与 SGS 连续场优先级按钉死口径执行。
6. 斜井：带测斜表合成井按真实轨迹粗化，cell 命中序列与
   竖直近似有差异证据；无测斜井标「竖直近似」。
7. provenance：每个 realization/对象场版本带完整参数+种子+
   父版本锚，catalog 可查。
