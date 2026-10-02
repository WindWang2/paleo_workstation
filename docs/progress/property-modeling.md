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

## 递延

- Pillar / 断块错位网格、Y 型断层、断面两侧的层位错动。
- 沉积相带控制、对象建模、序贯高斯模拟。变差函数克里金未做；若做只保留
  球状和指数两种标准模型，不引第三方地质统计库。
- 斜井测斜表、旋转/错切栅格、断层棒到地图坐标的投影。
- `stage()` 之后写入失败仍会留下一个没有版本的资产（登记器既有行为）。
