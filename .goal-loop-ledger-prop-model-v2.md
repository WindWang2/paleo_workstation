# Goal-Loop Ledger — 方向 45 属性建模 V2（分支 goal/prop-model-v2-20261004）

- 协议：每批 → 测试全绿 → diff 自审（分层/网格几何/统计收敛/诚实口径/i18n）→ 修复 → 再 review；至少两轮零 High/Medium，Low 记 PR body。
- 基线：origin/master @ b57e5f3（2026-10-05 fetch）。
- 资源：构建/测试一律 `-j8`。

## 轮0 勘察定案（2026-10-05）

### 实测事实

- **SGS/变差核已在 master**：`src/algorithms/geostat/`（variogram 拟合 / OK+LU / SGS 核）。
  sgs 核为 **2D 栅格口径**（GridSpec + 2D 环扫邻域 + 2D 变差），消费方 singlefactor/
  constraintfactorjobs。2D 主循环与邻域策略（2D 环访问序）决定堆内并列距离的打破
  顺序——**改 2D 路径的访问序会改逐位结果**，故 2D 入口必须保持算术恒等。
- **斜井测斜表已消费**（方向 19）：`requestFromCatalog` 经
  `ProjectDataFacade::trajectoryFor`（parseDeviationSurvey/parseDeviationText）拿真实
  三维轨迹，无测斜走直井回退（显式语义），`trajectoryWellCount` 已入 provenance。
  本方向补：**「竖直近似」显式口径标注**（现文案先例是「垂直简化/直井回退」，
  本方向 oracle 用词为「竖直近似」，ledger 记同义映射）+ 差异证据测试。
- **Y 型断层（方向 40）未落**：faultsurface.cpp:414 仍拒绝分叉
  （`SurfaceBuildStatus::Branching`）；e2cd549 只落了 domain 数据模型
  （FaultSurfaceSegment/branchCount/junctionVertexIndices）。→ **记依赖不自制**。
- **断层断距无存档字段**：`FaultHorizonCut` 只有 horizon/wkt/hangingSide + `extra`
  前向扩展位（注释明写「如后续断距/封堵属性」）。断距量算 `measureSlip`
  （faultsurface）存在但控制器边界明示不做断距计算。→ 断块错位的断距来源：
  cut.extra 前向键 + 请求级显式传入（测试/合成），无断距保持竖帘并标口径。
- **相带资产现成**：`facies_draft_map` GPKG（字段 region_id/facies_code/confidence/
  ...，方向 27）；ConstraintStore 相区多边形；`facies.<horizon>` 编辑层。属性建模
  V1 不消费相带。→ 相控输入面 = facies GPKG 栅格化到柱（QgsVectorLayer 读取，
  workflow 层先例 constraintfactorjobs.cpp:1514）。
- **FaultTriangle 体域充填重载无生产调用方**（仅测试）；V1 编排走平面竖帘
  FaultSegment（来自 FaultHorizonCut WKT）。V2 错位网格沿用竖帘分块机制 +
  断距驱动的柱位错动（z 向），不动 FaultTriangle 双轨口径。

### 语义决策（钉死）

1. **断块错位 = z 向断距错动**：ZoneGrid 是规则 IJK 地图格架（originX/dx 规则柱位），
   x/y 向 heave 错动会破坏 ijkAt/粗化/切片的全部规则柱假设——递延。本轮：断距矢量
   的 z 分量（throw，正=下掉侧 z 增大/更深）驱动下掉侧柱 top/bot 整体平移，跨断层
   同 k 层面错位量 = throw；连通屏障仍由竖帘 columnBlock 承担（几何错位与连通屏障
   不同层，与提示词口径一致）。
2. **SGS 接入 = 扩核不写核**：geostat 新增三维点集入口 `sgs3`（Sample3/格点目标 +
   连通组分屏障 + IJK 环扫邻域），与 2D `sgs()` 共享同一套序贯模拟机件
   （正态得分变换/SK 解/Box-Muller mt19937_64/分位反变换）——共享件抽到
   `sgs_internal.h`（detail），2D 入口算术恒等（dz=0 精确走 2D 半变差重载）。
   stratgrid 侧 `sgsfill` 只做编排转换（粗化种子→Sample3、分块、相带分区、
   硬数据钉死、多实现落 PropertyVolume）。
3. **相带分区参数域分离**：每相带独立正态得分变换 + 独立样本统计 + 独立 SGS 调用
   （变差几何共享全局模型——带内样本量不足以可信拟合带内变差，如实记口径）。
   带内种子 < 2 → 该带保持未充填并计数（不混全域参数，不虚构）。
4. **对象建模口径**：对象优先——对象 cell 硬覆盖背景场（IDW/SGS）值；对象几何放在
   错位后格架（沿层理展布），对象体可跨断层几何展布（连续地质体两盘各自粗化），
   数值分块仍由背景场机制负责。河道=中线正弦参数化（走向/长度/宽度/厚度/曲率/
   垂向锚定），点坝=弯道内岸椭圆。放置=相带内播种（mt19937_64 可复现）。
5. **多 realization = 同资产多 DERIVED 版本**：每 realization 独立 PPROP blob +
   独立版本 commit，extra 带 realization_index/seed/param_hash/口径字段。
6. **斜井口径**：provenance/panel 显式「竖直近似」（= 既有「垂直简化/直井回退」
   同义，oracle 用词统一为竖直近似）。

### 接口契约（先落地者为准）

- 变差/SGS 核归属 **geostat**（方向 41 若再动变差，按本 ledger 契约对齐）：
  VariogramModel 增 `verticalRangeRatio`（≥1，垂向变程=range/ratio，默认 1 =
  既有 2D 行为不变）+ `semivariance(dx,dy,dz)` 三参重载（dz=0 精确委托 2D）。
- stratgrid 消费 geostat DTO（algorithms 内跨子目录 include，singlefactor 先例）。
- Y 型分叉成面：依赖方向 40（未落）；本轮错位网格不自制分叉。

### 轮次计划

- 批次1：geostat sgs3 + variogram 垂向比（2D 恒等）；stratgrid faultoffset +
  sgsfill + 测试（Oracle 1/2/3/4）。**已落（commit 批次1）**。
- 批次2：stratgrid objectmodel（Oracle 5）+ 斜井差异证据（Oracle 6 算法面）。**已落
  （commit 批次2）**。
- 批次3：propertymodelworkflow V2（方法分支/多实现版本/相带栅格化/断距提取/
  provenance/竖直近似标注）+ panel（DESIGN.md 对齐：原生 QGroupBox、种子 mono
  数值面、口径标签 text-muted 次级信息）+ attach + 测试（Oracle 7）。**已落**。
- 收尾：docs/progress/property-modeling.md、TODOS、全量绿、两轮 review、原子提交。

## 批次记录

### 批次1/2（算法核）

- geostat：sgs_internal.h 抽共享机件；sgs3 三维点集入口（连通组分屏障 +
  索引环扫 + 下标全序并列打破）；重合条件显式精确复现（2D 靠方差钳零数值
  路径，3D 做成机制——注释记档）。**纠错**：sgs3 重合路径方差为 0 时仍消耗
  一次随机数（RNG 流计数一致性）。
- faultoffset：**纠错（review 轮1 发现）** out 与 grid 别名时先清零再校验
  → 自赋值毁格架；重写为局部副本 + 一次性写回（编排层原地错位合法）。
- sgsfill：竖帘分块=连通组分；相带域枚举升序确定；种子聚合（同 cell 取
  均值）；硬数据每实现钉死（float 存储精度）——**口径**：不足带内唯一井柱
  的种子 cell 仍钉实测值（硬数据不属于借全域参数）。
- objectmodel：512 段中线折线精算 + 椭圆归一化距离；后放覆盖先放。

### 批次3（编排 + UI）

- PropertyModelRequest V2 面：method（Idw/Sgs）/faultThrows/variogram+
  sgsRealizations/seed/objectSpecs+objectSeed/useFacies+faciesRings。
  paramHash schema 升 `paleo-propmodel-v2`（新输入全量入哈希）。
- computeSnapshot 返回 PropertyModelComputedList（SGS R 实现 = R 体）；
  commitAll 同资产多 DERIVED 版本（fileName R0001 序号；extra 带
  realization_index/seed/param_hash/口径串/父版本锚——Oracle 7）。
- throwSegmentsFromFaultSet：cut.extra["throw_z"]（正=上盘侧 z 增大）+
  hangingSide Left/Right → FaultThrow（dropLeftSide=盘侧==Left）；
  Unknown 盘侧 → 竖帘 + unknownSideCuts 计数。**Y 型依赖记档**：分叉成面
  属方向 40（未落），断距提取只认切割多边形，不自制分叉。
- collectFaciesPolygons（owner 线程 OGR 读最新 facies_draft_map：外环 +
  facies_code；孔洞/跨 CRS 重投影递延——相图出自同工程编图链）；worker 侧
  rasterizeZoneRings 纯几何栅格化（偶奇测试、先命中先得、无覆盖 = -1 背景域）。
- 竖直近似标注：provenance n_vertical_approx_wells + trajectory_caliber
  「X/Y 井真实轨迹（余为竖直近似）」；面板 propCaliberLabel 回写呈现。
- attach：面板纯值 getter → 请求翻译（视图层不 include algorithms——禁令）；
  相带请求无资产 → 降级全域 + 口径标签如实（不是失败）。
- panel：方法/SGS 组（方法=SGS 才启用，IDW 幂随之禁用）/对象组（checkable
  QGroupBox）/相带 checkbox/种子 mono 输入（DESIGN 数值面）；口径标签
  空串隐藏。

### 纠错记录（批次3）

- attach 曾把 FaultThrow 错插 vector<FaultSegment>（类型不同不能复用 insert）；
  断距段几何由竖帘全集承担，faultThrows 只驱动错位。
- fail 路径漏写 out.error（旧契约 bad.out.error 同写）→ singular/cancel
  两测试红，修 fail lambda 双写。


## Review 记录（多轮硬要求）

### 轮1（独立审查代理，全量 diff 五维）

**High 2 / Medium 4 / Low 13**，全部 High/Medium 已修，Low 修了 8 条
（恒真断言/退化注释/配对断言/种子 19 位/muted 样式/端点外衰减记档/
columnBlock 语义/verticalFrac=0 记档）：

- [High] sgs3 环扫剪枝步长 max→min（原实现把垂向条件化整段剪没）+
  强各向异性 1000:1 用例。
- [High] 堆未满整盒扫描的性能悬崖（按组已模拟计数早断 + 六面壳访问）。
- [Medium] 2D 黄金锚：master(b57e5f3) 与本分支 geostat 独立编译同场景
  FNV-1a 逐位一致（15131284449114955202），钉入 tst_geostat_sgs。
- [Medium] 无相带路径样本不足静默（caliber 不再依赖相带分支）。
- [Medium] attach 整表深拷贝 → 引用共享体；workflow 逐实现 volume 深拷贝
  → 只首实现携带。
- [Medium] objectmodel 全柱精算无预筛 → 包围盒快筛。

### 轮2（独立复审：验证轮1修复 + Oracle 核对）

**High 1 / Medium 2 / Low 7**，全部 High/Medium 已修，Low 修 6 条：

- [High] 倾斜/断尖格架的剪枝下界不成立（跨柱 top 落差抵消 r·zMin，
  差分复现 848/960 逐位偏差）→ 按优势轴分解重推导（x/y 精确、z 分
  同柱/跨柱），Lattice3Steps 增 topRelief；复现程序复跑 0/960 差异。
- [Medium] commitAll 半途失败：同步路径按完全成功呈现 + realization_count
  全额虚报 → 全条目置败 + 「已登记 k/N」错误串。
- [Medium] 组内点数 < k 时早断失效 → effectiveK < k 短路。
- Low：种子排序（跨库可复现）、objectmodel 失败契约（预收集候选）、
  包围盒垂度补偿、失败轮清口径、恒真断言、lupdate、流式序列化记 TODOS。

## Oracle 验收对照（轮2 独立核对，7/7 通过）

| # | Oracle | 证据 |
|---|--------|------|
| 1 | 断块错位量化 + 跨断块零泄漏 | tst_faultoffset（界面差=±throw/层厚保序/分块不连通/IDW 东侧 NaN）+ tst_propworkflow faultThrowExtractionFeedsOffset（链路 interfaceZ 差=+4 入 provenance） |
| 2 | SGS 硬数据精确复现 | tst_sgsfill hardDataExactEveryRealization（5 实现逐 cell float 精确）+ sgs3 conditionedAtSampleTargets（1e-9）+ 工作流链路 |
| 3 | 统计收敛（容差+种子） | statisticsConvergeAcrossRealizations（24 实现池化均值 ≤0.4σ、方差比 [0.4,2.2]、同种子逐位/异种子>100 差异） |
| 4 | 相控两带独立 + 带界零混参 | faciesZonesSeparateAndDoNotLeak（毒化 B 带→A 带逐位不变；均值 [8,12]/[18,22]） |
| 5 | 对象几何场可复现 + 对象优先 | channelGeometryRecoverableFromField（PCA 走向±5°/宽度/厚度层数）+ objectPriorityOverridesBackground + objectOverrideRegistersInProvenance |
| 6 | 斜井差异证据 + 竖直近似标注 | tst_upscale deviatedVsVerticalDifferentCellHits（代表柱东迁≥2 层）+ deviatedWellUsesTrajectoryStations + trajectory_caliber/n_vertical_approx_wells 断言 |
| 7 | provenance 完整（参数+种子+父锚） | sgsMultiRealizationRegistersIndependentVersions（4 版本 DERIVED、param_hash 唯一、realization_index 0..3、seed、父锚、重跑逐位配对 matched==4） |

附注（轮2 reviewer 提出的口径澄清）：Oracle 3 的「变差面收敛」按
均值/离散度口径断言（实现集实验变差 ≈ 模型的直接 variogram 复现
断言未单列）——记为后续可加强项，不阻塞验收。

## 本机全量

- 增量全量 ctest：仅 tst_startup_trace 失败——**对照实验证实为本机环境
  漂移**（stash 本分支改动后 master 干净基线同败且更差：中位 0.32 vs
  本分支 0.19；基线 2026-10-01 实测环境与当日不同）。tst_singlefactor_perf
  并行负载下抖动、单独复跑通过。其余全绿。
