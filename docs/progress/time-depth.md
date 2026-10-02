# time-depth — 时深转换与速度建模（goal/time-depth-velocity-20261002）

分支 `goal/time-depth-velocity-20261002`（自 master e6e95af 起）。迭代账本
`.goal-loop-ledger-time-depth-velocity.md`（轮次/Oracle 证据）；本文是交付记录。

## 交付一览（按提交）

| 提交 | 内容 |
|------|------|
| feat(algorithms) | 速度模型核 `src/algorithms/velocitymodel.{h,cpp}`：层间平均（分段线性，锚点位级精确、样点范围外不外推）与 V0-k 线性速度函数 v(z)=V0+kz（TWT 域 2 参数 Gauss-Newton 拟合 + 闭合式换算）；(x,y,twt) 空间查询 power-2 IDW（命中井位即取）；结点二分查询；JSON 序列化（无时间戳→幂等）；时间栅格→深度栅格执行器。数值单测 14 例 |
| feat(workflow) | `DepthConversionWorkflow`（src/workflow/depthconversionworkflow.{h,cpp}）：catalog 关联收集（tops/time_depth/well_head 各资产取最新版本）→建模→`velocity_model` DERIVED 存档；时间域层位 GeoTIFF→深度域 GeoTIFF（`depth_raster` DERIVED + `depth.<H>` 层声明 @ 00_Data，PALEO 测网号域透传）。全链 offscreen 测试 4 例 |
| feat(ui) | 层树右键「转换为深度域…」意图信号（仅 `horizon.*` 层可用，禁用带 reason）；壳接线 `attachDepthConversion`（模型缺时自动从 catalog 井控数据建层间平均模型）；剖面左缘深度标尺反投影修正（与 D2.5 右缘双刻度同法，非常速模型下刻度间距如实非线性，去掉「常速」近似角标） |
| test(perf) | 井数线性比率门（12 井 ≤16× 单井）+ `PALEO_REAL_PROJECT_AREA` 门控真机实测（BASELINE 行誊下表） |

## 语义决策（合并评审重点）

1. **深度基准 = TVD**（井口基准面以下视铅直深度，米）：TD 表 TVD 列优先、
   MD 兜底（同 TimeDepthTool 口径）。TVDSS 换算需逐井 KB——不臆造，递延
   （TODOS.md 登记）。模型锚点即校验炮/分层表的 (TWT,TVD) 对本身。
2. **两种模型语义分立**（测试分别钉住）：
   - 层间平均 = **插值语义**：井内分段线性（层内 Vint 恒定
     Vint=2000·ΔTVD/ΔTWT），锚点（井位×控制点 TWT）**位级精确命中存档
     深度**，样点范围外不外推（同 TimeDepthModel 纪律）；
   - V0-k = **回归语义**：v(z)=V0+kz（直线速度函数，Sheriff,
     Encyclopedic Dictionary of Applied Geophysics "velocity function"），
     闭合式 z(twt)=(V0/k)(e^{k·twt/2000}−1) 处处可算（允许超出样点外推），
     锚点残差以 fitRmsMs 如实记录——拟合模型不冒充精确。
3. **V0-k 拟合走 TWT 域 2 参数 Gauss-Newton**（残差=T 闭合式−控制点 TWT，
   阻尼回溯保 V0+kz>0，固定 60 迭代上限=确定性）。教训：层间速度是 1/v 的
   调和型层平均，对瞬时速度线做线性 LS 有系统偏差（Vint=Δz/Δτ ≠ v(z_mid)），
   闭合式与积分关系不闭合 ~4%——轮2 实测教训，GN 后真值恢复 <1e-6。
   单层井（2 控制点）不强解梯度：V0=Vint、k=0（常速，诚实口径）。
4. **空间加权 = power-2 IDW**（w=1/d²，命中井位即取）——与
   paleo:paleo_constraint_idw 同权重契约。逐井先算一维答案再 IDW：井样点
   范围外的井不参与该查询点加权（不阻断其他井）；井位处不经浮点加权，
   锚点零误差由此钉死。
5. **TD 与分层同井时取校验炮**（实测标定优先于解释分层），记入模型 notes；
   DC.dat 的 Time 列 -99999 哨兵行如实过滤，不臆造。
6. **序列化无时间戳**（format=paleo-velocity-model v1）：重复建模/重复转换
   位级幂等（测试按字节断言）；catalog 提交时 sha256 天然提供溯源。
7. **深度栅格输出走 `writeHorizonGeoTiff`**（同层位导入派生口径）：保留
   PALEO_INLINE_*/PALEO_XLINE_* 测网号域（剖面导航依赖），nodata=-9999，
   PALEO_DT_MS/T0_MS 不携带（那是地震轴元数据，深度面无此语义）。
   层声明 `depth.<H>` @ `00_Data`（数据类派生，同 horizon 先例）。
8. **深度标尺反投影**：左缘深度刻度先反解 TWT 再取像素
   （`py = timeToPixelY(DepthToTwtMs(tick))`）——与 D2.5 右缘双刻度同法；
   修前按常速线性近似，校验炮分段模型下刻度错位且角标自认「常速」。
9. **剖面切片逐样换算的坐标近似**：SgySliceImage 不带道坐标——用层位头
   P1/P2/P3 仿射把 (inline, xline) 映到地图 XY（与层位/井位同坐标架），
   该近似只影响性能实测的取样位置，不影响转换正确性。
10. **查询热点二分**：校验炮表 ~500 结点/井，20 井×263k 像元转换的线性扫
    是显著热点（~1.0s/面）——结点 TWT/TVD 严格递增（fit 已校验）→
    upper_bound 二分，~60ms/面（16×）。

## 966MB 真工区实测（tst_timedepthperf，PALEO_REAL_PROJECT_AREA 门控）

环境：20 井 `时深/TD/*.dat` 校验炮（9666 结点）+ `井位/ExportWellHead.dat`
坐标；层位 C3/C6/D53/D61/D62/D63/D71/D72（641×411 @25m）；体
`200P_seismic.sgy`（il 1315..1725 × xl 4165..4805 × ns 901 @ 2000µs），
中段 IL 直读后端，单次热态（best-of-3）：

| 指标 | 延迟 |
|------|------|
| 速度模型构建（20 井解析+拟合） | 107ms |
| 层位面时深转换（单面 263k 像元） | **60–64ms**（4.1–4.4 Mcells/s） |
| 全部 8 个层位面 | 489ms |
| 体打开（冷，直读 Load） | 370ms |
| IL 切片提取（基线） | 12ms |
| 剖面深度轴逐样换算（641 道 × 901 样） | **298ms**（66.1% in-model） |

注：in-model 66% 因浅于最浅校验炮结点（380ms）的样点在插值语义下如实
NaN（不外推不臆造）；深度标尺只标注视口内深度范围，实际换算量远小于整剖。

## 递延（TODOS.md 同步登记）

- TVDSS/KB 深度基准换算（井位表有 KB 列，可做逐井平移——待用户确认基准口径）。
- V0-k 层段化（逐层 V0/k）与三参数模型；模型对比/编辑 UI。
- 时深转换对话框（模型选择、范围/覆盖预览、输出组选择）——本轮为右键直达
  最小闭环。
- 地震体整体时深转换（深部体重采样）——范围更大，单独立项。
