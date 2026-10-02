# Goal-Loop 方向 14：交会分析与无监督相分类（Crossplot-Driven Facies Classification）

## 背景（实测事实，勿再勘察）

- 数据源已齐：井曲线（LAS + `algorithms/petrophys` 派生曲线 Vsh/φ/Sw）、地震属性体/切片（`algorithms/seismicattr` + SATR DERIVED 资产）、解释层位栅格（GeoTIFF/等值线族）、相编图多边形（D53 Mock 相 类工作流）。
- 「智能编图」现有 Mock 预测 + `RemotePredictionRouter`（`src/ai/remotepredictrouter.*`，PR #96 落地）——但其输入语义是「监督/预设分类」；**无监督相分类（多维空间聚类→lasso 选区→回写分类栅格）整条链路不存在**。
- 既有交会面板：`src/ui/correlationpanel*` 是连井对比面板（非散点交会）；wellcomposite 面板是井剖面。散点交会图（crossplot）组件需新建但可复用现有图表面板基建。
- 分类结果去向已备：catalog DERIVED 资产登记先例（SATR/depth_raster/SATR）；编图页的「应用到地图选中要素」链路存在。
- 真工区 env 门控 `PALEO_REAL_PROJECT_AREA`；纪律照旧：DESIGN.md、层标记、add_paleo_test、`check_layering --strict`、比率门。

## 目标形态

**交会图组件 + 聚类核 + 分类回写**三件套：

1. **交会图**（视图层 `src/ui/crossplot/`）：2D/3D 散点（X/Y/Z 轴可换：任意井曲线/属性/栅格采样）、密度渲染（大数据量退化到密度云）、lasso/框选选区高亮联动层树与地图。
2. **聚类核**（`src/algorithms/cluster`，纯数值）：k-means++ 与 GMM（EM，对角协方差起步）+ 手选区分类器（凸包/盒规则）；输入 = 多维样本向量（井点或栅格像素），输出 = 类别标签 + 置信度/距离场。
3. **回写**：井点分类 → 井层段表（喂 petrophysics/相统计）；栅格分类 → **分类栅格 DERIVED**（palette 分类色，catalog 登记 + provenance 参数 hash）；相带分类可导编图页作约束图层。

## Oracle 验收（全部须实测通过并记账本）

1. **聚类核断言级正确**：
   - 合成双峰数据 → k-means 聚类中心误差 <5%、纯度 >0.95；k=1 退化为全局均值；空集/单点/全相同点不崩且如实返回。
   - GMM 对两高斯混合：后验均值/权重误差 <10%；BIC/对数似然单调性断言。
   - 手选凸包分类器：内/外/边界点分类正确；与 lasso 顶点语义一致。
2. **数据源接入**（`src/services/` 或 workflow 编排）：任意两个井曲线交会（深度对齐 NaN 剔除对断言）、属性切片×层位栅格交会（像素对采样计数断言）、多维（≥3 维）样本直接投喂聚类。
3. **交会图 UI**：轴切换即时重绘（≥100k 点密度渲染 <200ms 感知）、lasso 选区统计读数（N/均值/占比）、选中点联动定位（点击散点→地图缩放到该井/像素，先例：preview 定位链路）。
4. **分类回写**：聚类标签 → 分类栅格 DERIVED 落盘 + catalog 登记 + palette 分类色；分类栅格像素数/类别统计断言；井层段分类写回项目（tops/层段表先例）。
5. **编图联动**：分类栅格可作为预测/相编图的约束图层或先验相图项参与现有工作流（不另起炉灶）。
6. **性能**：100k 样本 k-means k=8 < 3s、1M 像素栅格分类 < 10s、取消/进度/失败诚实。
7. ledger + `docs/progress/crossplot-facies.md`（聚类选型理由/置信度口径/递延：有监督分类、SOM、时深域交会）。

## 勘察指引

- `src/algorithms/petrophys/*`（曲线抽取/NaN 语义）、`src/algorithms/seismicattr/`（自研 FFT 与体窗读）、`src/io/lasparser.*`、`src/qgis/qgislayerservice.*`（栅格像素读）
- `src/ui/correlationpanel.*`、`src/ui/datapreview/preview*`（面板/dock 先例）、`src/ui/maptools/paleoshapetools.cpp`（lasso/多边形交互先例——CaptureHelpers 抽取已在仓）
- `src/ai/remotepredictrouter.*`、`src/workflow/`（编排先例）、DERIVED 登记先例 `src/metadata/`
- `docs/progress/petrophysics-logs.md`、`seismic-attributes.md`、`ai-assist.md`（本批语义口径）

## 禁区

- 不引第三方 ML/聚类库——k-means/GMM 实现不复杂，自研可控（seismicattr FFT 先例）。
- 不做有监督分类训练管线（样本标注→训练→推理另立项，RemotePredictionRouter 侧归 AI 方向深化）。
- 不改 mapbook/mapbookqueue 批量链路；不改既有编图工作流内部——分类栅格走 DERIVED 资产接入。
- 交会图不做打印导出排版（归 mapbook 方向）；不做 4D/时移。
- 视图层只发信号；聚类/采样统计一律在数据/功能层。

## 迭代协议

- **轮0**：勘察定案——井曲线/属性切片/栅格的样本抽取接口签名、交会图组件复用面；进 ledger。
- **轮1**：`algorithms/cluster`（k-means++/GMM/凸包规则）+ `tst_cluster` 数值断言全绿。
- **轮2**：样本抽取编排（井点/栅格/属性三路统一 `SampleSet`）+ `tst_crossplot_samples`。
- **轮3**：交会图面板（轴换/密度渲染/lasso/联动定位）+ `tst_crossplotpanel`。
- **轮4**：分类回写（井层段 + 分类栅格 DERIVED + 编图约束接入）+ `tst_faciesclassify`。
- **轮5**：真工区实测 + 性能比率 + docs/progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/crossplot-facies -b goal/crossplot-facies-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：
   - `git diff origin/master...HEAD` 全量自审：无调试残留/死代码；层标记齐；`check_layering --strict` 绿；
   - vendor 前缀全量构建零新警告，ctest 全绿；
   - Oracle 每条有命令+输出摘要证据；
   - 发现问题先修再验直到干净。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
