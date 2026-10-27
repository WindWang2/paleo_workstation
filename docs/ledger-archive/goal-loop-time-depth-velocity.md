# Goal Loop ledger — time-depth-velocity（时深转换与速度建模）

- 分支：`goal/time-depth-velocity-20261002`（自 master e6e95af 起，worktree `.worktrees/time-depth`）
- 构建：`cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
- 目标：井分层/校验炮 → 速度模型 → 时间域层位/剖面转深度域，全链 offscreen 可驱动。

## Oracle（验收，全绿即收）

1. 井分层/校验炮 → 速度模型 → 层位时深转换全链闭环，offscreen 可驱动。
2. 数值断言：恒速/两层模型解析比对 ≥4 用例；井锚点处零误差断言。
3. 模型可存可取（catalog 条目），重复转换幂等。
4. 深度标尺/结果上图走层树与角色体系，视图只发信号。
5. 分层绿（--strict），文件头层标记；ctest 全绿；ledger；push + gh pr create。
6. 井震对标既有基建复用并对齐口径，不重写。

## 轮0-1 勘察（事实底座）

- **解析已通**：`parseWellTopsText`（DC.dat 多井分层，真实文件 Time 列多
  -99999 哨兵）/`parseTimeDepthText`（TD/*.dat 单井校验炮，TIME TVDSS TVD MD）
  /`parseWellHeadText`（井位+KB）都在 `src/io/wellfileparsers.h`；DataImportService
  已按 role tops/time_depth/well_head 挂 catalog 关联——速度数据源「接入」实为
  「消费」，不重写。
- **既有 1D 件**：`TimeDepthModel`（常速/校验炮分段，不外推）、
  `TimeDepthTool`（TVD 优先 MD 兜底、文件序纪律）、`VelocityModel`
  （services，逐井表桥）——均为逐井、无空间、无 V0-k、不可存档。
- **空白**：空间速度模型核（拟合+采样+序列化）、层位栅格时深转换执行器、
  上图入口、深度标尺非常速修正（左缘深度刻度按常速线性近似，角标自认）。
- **登记先例**：`DerivedAssetRegistrar`（stage/commit，find-or-create by
  (type,displayName)，版本不可变+只读+sha256+父版本 provenance）。
- **真机数据形状**：`/home/kevin/projects/paleo_project/data/project_area`
  井分层/DC.dat、时深/TD/（20 井）、层位/（8 面 641×411@25m）、
  地震体/200P_seismic.sgy（966MB）、井位/ExportWellHead.dat。
- **夹具**：testdata/project_area 有 A1_TD.dat/DC.dat/D61_sample.dat/
  ExportWellHead.dat；层位 stem 须在 AreaRules.sequenceBoundaries（D61 在，
  D61_sample 不在——未决层位不派生栅格，测试按 D61.dat 摆盘）。

## 技术选型（轮0-1 决议）

| 决策 | 选择 | 因由 |
|------|------|------|
| 模块落位 | 核 `src/algorithms/velocitymodel`（paleo_algorithms）+ 编排 `src/workflow/depthconversionworkflow`（paleo_workflow），布线 `cmake/extra-time-depth.cmake` | 算法层纯数值先例 seisattr；派生产物登记属 workflow（mappingworkflow 先例） |
| 插值库 | 自写（IDW power-2 + 分段线性 + 二分） | vendor 无 Eigen（仅 glm 渲染用）；「Eigen 或自写」按零新依赖 resolve 成自写；权重契约对齐 paleo_constraint_idw |
| V0-k 拟合 | TWT 域 2 参数 Gauss-Newton（残差=T(zᵢ)−tᵢ，阻尼回溯，60 迭代上限） | 层间速度对瞬时速度线的线性 LS 有调和型系统偏差（~4%，积分不闭合）——轮2 实测教训；GN 与闭合式自洽，真值恢复 <1e-6 |
| 深度基准 | TVD（米）；TVDSS/KB 递延 | TD 表 TVD 列优先/MD 兜底同 TimeDepthTool；KB 平移不臆造 |
| 模型序列化 | JSON（paleo-velocity-model v1，无时间戳） | catalog DERIVED 资产 velocity_model；幂等测试按字节断言 |
| UI 入口 | 层树右键意图信号 → 壳 lambda → workflow | UI_LAYER_PLAN「视图只发信号」；attachDepthConversion 独立接线不动 attachWorkflows 签名 |

## 轮2 速度模型核（commit feat(algorithms)）

- `VelocityModel::fit`：逐井校验（≥2 控制点、TWT/TVD 文件序双严格递增、
  坐标有限），坏井跳过记 notes（QC 透明），全坏 → 无效+error。
- 查询：depthForTwt/velocityAt/averageVelocityAt（(x,y,twt)）+
  well* 单井面；结点二分（upper_bound 语义，等值命中左结点→f=0→位级精确）。
- `convertTimeGridToDepth`：逐像元中心（北向上 gt[6] 口径同厚度链），
  nodata/非有限透传 NaN，范围外计 outsideModelCells。

**Oracle 2 证据**（`./build/tst_velocitymodel`，14/14 绿）：
恒速解析（z=v·t/2000，k=0 极限与层间平均同值）、两层手算
（Vint 2250/2750，z(1000)=1062.5、z(2000)=2312.5、Vavg(1000)=2125、
分段互逆）、V0-k 闭合式 vs 独立 Simpson 积分（4 个 twt 点相对 <1e-6）+
反闭合式互逆、合成真值恢复（v0=1800/k=0.6 → 拟合 <1e-6、fitRms<1e-6、
中途点闭合式命中）、带噪声残差可见（1<fitRms<100ms）、**锚点零误差**
（3 井×全部控制点：井位查询位级 == 存档深度）、双井 IDW 手算
（9:1→1100、1:9→1900、1:25→1961.5）、序列化往返（文档幂等+查询位级一致）、
栅格幂等（memcmp 位级）+计数（nodata 1/范围外 2/换算 1）。

## 轮3 转换执行器 + catalog（commit feat(workflow)）

- `requestFromCatalog`：well 实体已解析关联按 role/asset 去重，每资产取
  最高版本（TD 逐井多资产全收）。
- `buildModel`（纯静态，perf/预览直用）/`buildAndStoreModel`（stage→写
  JSON→commit，父版本=源数据文件版本，extra 记 type/wells/knots/notes）。
- `convertRasterToDepth`：GDAL 读时间栅格（含 PALEO 测网号域透传）→核换算
  →`writeHorizonGeoTiff`→DERIVED depth_raster（父版本=时间栅格+模型）→
  `depth.<H>` 层声明（有 layer service 时）→conversionDone 信号。

**Oracle 1/3 证据**（`ctest -R tst_depthconversion`，4/4 绿，offscreen 真服务栈）：
导入（井位/分层/校验炮/层位 D61）→requestFromCatalog 三类路径齐→建模存档
（latestModelPath 命中）→**链上锚点零误差**（A1 井位查校验炮行 TWT 位级
==TVD，含首/中/末行）→convertRasterToDepth→manifest depth.D61（00_Data/
raster/源存在）→catalog velocity_model+depth_raster 各 1 资产、extra 计数
>0→深度栅格回读 3 像元位级 == 模型复算；重复建模/转换：同资产递增版本、
**文件字节一致**（幂等）。

## 轮4 UI（commit feat(ui)）

- LayerTreePanel：`depthConversionRequested(layerId)` 意图信号 +
  layerTreeDepthConvertAction（paleoLayerId `horizon.*` 才可用，禁用带
  reason tooltip）——面板只发信号不干活。
- PaleoMainWindow::attachDepthConversion：壳解析 manifest 声明→模型
  （latestModelPath 复用；缺则 requestFromCatalog+buildAndStoreModel）→
  转换→状态栏+MessageLog 反馈。main.cpp 在 attachWorkflows 后挂
  ctx.depthConversionWf()（AppContext 工程打开时 rebind 同一 catalog 实例）。
- SeismicSectionCanvas 左缘深度刻度反投影（同 D2.5 右缘法）+ 角标去「常速」。

**Oracle 4 证据**：
`ctest -R tst_layertreepanel`（新增 depthConvertActionEmitsIntent：意图信号
携带 horizon.D61、深度产物禁用、禁用带 reason）+ `ctest -R tst_seismic_welltie`
（新增 depthRulerBackProjection：常速 vs 分段模型深度模式渲染可区分、时间
模式冒烟）全绿。

## 轮5 性能（commit test(perf)）

- 比率门（常跑）：`tst_timedepthperf` wellCountScalingRatioGate——12 井
  ≤16× 单井（实测 8.0×），411×641 全格。
- 真机门控（PALEO_REAL_PROJECT_AREA，只读契约前后清单一致）：
  建模 107ms（20 井 9666 结点）；层位面 60–64ms/面（4.1–4.4 Mcells/s）、
  8 面共 489ms；体开 370ms、IL 切片 12ms、**剖面深度轴逐样换算 298ms**
  （641×901 样，66.1% in-model）。二分优化前层位面 ~1.0s/面（16× 收益）。
  BASELINE 行已誊 docs/progress/time-depth.md。

## 轮6 交付（本文档 + review + PR）

- docs/progress/time-depth.md（语义决策 10 条 + 实测表 + 递延）。
- review 清单证据见 PR body 自审结论。

## Oracle 对照（收口）

| # | 条目 | 证据 |
|---|------|------|
| 1 | 全链 offscreen | tst_depthconversion 4/4（导入→建模→转换→登记） |
| 2 | ≥4 解析用例 + 锚点零误差 | tst_velocitymodel 14/14（恒速/两层/闭合式 vs Simpson/真值恢复 + 位级锚点×2 处） |
| 3 | 可存可取 + 幂等 | velocity_model DERIVED + latestModelPath；重复转换文件字节一致 |
| 4 | 层树/角色体系上图，视图只发信号 | depth.<H> 声明 + 意图信号测试；标尺反投影测试 |
| 5 | --strict/头标记/ctest/ledger/push+PR | 见轮6 与 PR |
| 6 | 复用井震对标口径不重写 | 零改 welltie/synthetic 既有代码；TD 口径（TVD 优先/不外推/文件序）对齐 TimeDepthTool/TimeDepthModel |
