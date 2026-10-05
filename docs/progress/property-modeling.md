# property-modeling — 三维地层格架与属性建模（goal/property-modeling-20261002）

分支 `goal/property-modeling-20261002`（自 master `11099e2` 起）。迭代账本
`.goal-loop-ledger-property-modeling.md`。本文只记语义决策和实测数字。

## 交付

| 面 | 内容 |
|----|------|
| 格架核 | `src/algorithms/stratgrid`：`buildZoneGrid`、地层坐标往返、6 邻接 |
| 粗化 | `upscaleWells`：均值 / MD 弧长加权均值 / 中位数 / 众数 |
| 充填 | `fillIdw`：IJK 距离、幂次默认 2、断层竖帘阻断；`PPROP1` 小端容器 |
| 编排 | `PropertyModelWorkflow`：层位 GeoTIFF → 格架 → 粗化 → 充填 → catalog `property_volume` DERIVED，`param_hash` 无时间戳 |
| 视图 | `PropertyModelPanel` 只发信号；剖面 `setZoneOverlay`（NaN 透明）；3D `updatePropertySlice` / `updatePropertyStackLayer` 走现成切片着色器 |

## 语义决策

1. **地层坐标**。柱 (i, j) 上 s = (z − top) / (bot − top)。s = 0 在顶面，
   s = 1 在底面。第 k 层占 s ∈ [k/nk, (k+1)/nk)，末层含 s = 1。
   界面点归更深一层。像元角点为原点，中心在半像元；dy 可为负（北向上
   GeoTIFF，行号增加 y 减小）。
2. **活柱**。bot − top > 1e-4 才是活柱，单元体积 = dx · |dy| · (bot−top)/nk。
   整幅没有活柱时拒绝：全部重合写「奇异面：顶底重合」，全部颠倒写
   「顶底颠倒」。NaN 像元是死柱，不补 0。
3. **粗化缺省聚合器是弧长加权均值**（`ThicknessWeightedMean`）。权重是
   轨迹 MD 长度。直井且 z = MD 时，这段长度就是垂向厚度。NaN 样点把曲线
   断开，不跨缺失插值。均值 / 中位数 / 众数只统计落在层内的有限样点。
   众数并列时取较小值。井不穿层段，或穿了但没有有限样点 → 无值，不补 0。
4. **充填距离是地图米**，w = 1/d^power，power 默认 2。
   d² = (Δi·dx)² + (Δj·dy)² + (Δk·层厚)²，层厚是活柱 (bot−top)/nk 的平均。
   种子落在单元上时该单元取种子（多个精确命中取平均）。一个种子铺满它所在的
   断层块；块内没有种子的单元保持 NaN。
5. **断层阻断是竖帘，不是断块网格**。输入是地图 XY 上的线段（层位切割
   WKT 的 LINESTRING / POLYGON 外环；Z 坐标只取 x,y；内环忽略）。
   柱心连线被严格穿过、被折线顶点切在边的内部、或与断层共线重叠一段正长度
   时，两柱不连通。只共享端点不阻断。各 k 共用这一套块号。剖面棒
   （traceFrac, TWT）没有地图坐标，本轮不投影。
6. **属性体容器 `PPROP1`**。无时间戳，同输入字节级稳定。catalog 类型
   `property_volume`，版本 extra 记 `param_hash`、聚合器、层数、幂次、
   顶底名和曲线名。父版本是能在 catalog 里对上路径的层位栅格。
7. **剖面叠加**。功能层 `projectToSection` 把采样点 (x, y, z) 反查回 IJK，
   图像布局与剖面画布相同（行 = 采样，列 = 道）。NaN 烘焙成透明像素。
   3D 切片把 i 当 xline 轴、j 当 inline 轴、k 当采样轴，放进与地震切片
   相同的归一化立方体。单层轴在显示上把末下标抬 1，避免面片退化成线。
8. **真工区井是直井**（井口 XY = 井底 XY，KB = 0）。粗化用 z = MD。
   斜井测斜表本轮不接。层位 z 与曲线必须同一深度域；对不上时粗化得到
   无值，不把时间面假装成深度。

## 真工区实测（tst_propmodelperf，PALEO_REAL_PROJECT_AREA）

D63 / D72 层位栅格抽到 200×200×20（800000 单元），20 口井 GR，
弧长加权均值 + power-2 IDW：

| 指标 | 值 |
|------|----|
| 耗时 | 5375 ms |
| RSS | 171656 → 174360 KiB（约 +2.6 MiB） |
| 活柱 | 39997 / 40000 |
| 充填单元 | 799940（活柱全部有值） |
| 比率门 | 3 次累计：40×40×8 约 46 ms，80×80×8 约 175 ms（4× 柱数，约 3.8× 时间） |

修复轮（`goal/property-modeling-fixes-20261002`）之后同一条真工区链是
4571 ms，RSS 169452 → 174220 KiB（峰值 174220）。

壳层有「属性建模」dock：面板发意图，`requestFromCatalog` 收集层位和井，
跑完把 I/J/K 中切片推进 3D 视口；剖面尺寸对得上时再叠层段。

## V2（goal/prop-model-v2，2026-10-05）

方向 45 在 V1 面上落了四块能力，口径全部记
`.goal-loop-ledger-prop-model-v2.md`：

1. **断块错位（faultoffset）**。断距矢量 z 分量驱动下掉侧整柱 top/bot
   平移：沿段线性内插、最近断层主控（并列取下标小者）、柱心恰在线上
   的边界柱不动并计数。连通屏障仍是竖帘 columnBlock——几何错位与连通
   阻断分层。断距来源：`FaultHorizonCut.extra["throw_z"]`（正 = 上盘侧
   z 增大）+ 盘侧 Left/Right；盘侧 Unknown 或无断距保持竖帘并计数。
   heave（水平错动）与深度变化断距递延。
2. **序贯高斯充填（sgsfill）**。geostat 扩三维点集入口 `sgs3`（与 2D
   `sgs()` 共享 sgs_internal.h 机件——不是新模拟核；2D 逐位恒等由
   dz=0 精确委托 2D 半变差保证）。stratgrid 侧编排：条件点 = 粗化井柱
   cell（重合合并取均值）、序贯路径走三维网格、竖帘分块作连通组分
   （跨断块零条件泄漏）、相带分区独立参数域（各带独立正态得分与统计，
   变差几何共享全局模型；带内种子 < 2 保持未充填）、硬数据每实现钉死
   （float 存储精度）。变差模型加 `verticalRangeRatio`（垂向变程 =
   range/ratio）与 `semivariance(dx,dy,dz)` 三参重载。
3. **相带约束 + 对象建模**。相带面 = 最新 `facies_draft_map` GPKG
   （owner 线程 OGR 读外环 + facies_code，worker 纯几何栅格化：偶奇
   测试、先命中先得、无覆盖 = -1 背景域）。对象建模最小骨架
   （objectmodel）：河道 = 中线正弦参数化（走向/长度/宽度/厚度/曲率/
   垂向锚定，512 段折线精算）、点坝 = 地图椭圆；相带内播种
   （mt19937_64 可复现）；**对象优先**——对象 cell 硬覆盖背景场
   （IDW/SGS）值，放置几何全量入 provenance。
4. **多实现版本链与口径**。SGS N 实现 = 同资产 N 个 DERIVED 版本
   （fileName R 序号；extra：realization_index/seed/param_hash/口径串/
   父版本锚）。诚实口径三处落地：provenance JSON、catalog extra、面板
   `propCaliberLabel`——竖直近似井数、断层竖帘/错位计数、相带覆盖、
   种子与参数全套。

面板（DESIGN.md 对齐）：方法 combo（IDW/序贯高斯）联动 SGS 参数组、
相带 checkbox、对象组（checkable QGroupBox，原生控件）、种子 mono
数值面。paramHash schema 升 `paleo-propmodel-v2`（新输入全量入哈希）。

## 递延

- Y 型断层分叉成面（方向 40 数据模型已打底，算法未落——依赖记档不自制）。
- 断距 heave（x/y 向水平错动——破坏规则柱假设）、深度变化断距（生长
  断层）、pillar 网格本体。
- 带内变差拟合（样本量不足以可信）、对象-河道耦合点坝、多对象谱系。
- 相带面孔洞与跨 CRS 重投影（相图出自同工程编图链，同 CRS 假设）。
- 旋转/错切栅格、断层棒到地图坐标的投影。
- `stage()` 之后写入失败仍会留下一个没有版本的资产（登记器既有行为）。
