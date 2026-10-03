# well-trajectory — 井轨迹空间化（goal/well-trajectory-20261003）

分支 `goal/well-trajectory-20261003`（自 origin/master `4741115` 起）。迭代账本
`.goal-loop-ledger-welltraj.md`。井不再当垂直线：测斜站表 → 三维轨迹（最小曲率
法），导入/角色链接/MD↔TVD 服务/消费面（属性建模、剖面、平面图、综合图）全链
按真实轨迹吃井；无测斜井显式回退直井，不虚构造斜。

## 交付一览

| 块 | 内容 |
|----|------|
| 域模型 | `paleo::WellDeviationSurvey`（`src/domain/deviationsurvey.*`）：站表 → `tvdAt/pointAt/tvdToMd`；完整 3D 最小曲率 |
| 导入 | 分类类型 `well_deviation`（目录段 `dev/测斜/井斜`、文件名 `deviation/trajectory/井斜`、SpreadsheetML「井斜」工作表嗅探）→ 角色 `trajectory`（RoleRegistry 现成词，井斜轨迹）；文本站表 `parseDeviationText`（`# Well :` 井名 + `-99999` 哨兵）与 XML `parseDeviationSurvey` 双格式；导入分支照抄时深语义（已决 primary 链接 / 未决不建井不猜） |
| 读面 | `ProjectDataFacade::trajectoryFor(wellId)`（`tdTableFor` 同族：primary `trajectory` 链接 → 站表 → 域模型；无链接 nullopt，坏数据 nullopt + lastError）；`WellLogSet::readCurveTvd`（按列读曲线体并逐点 MD→TVD；survey 空/无效 → tvd≡md） |
| 消费 | 属性建模井筒站点、剖面井轨折线 + 井底坐标、平面「井轨迹投影」线层、综合图 MD↔TVD 口径统一（`DepthTransform` 委托域模型） |

## 最小曲率公式口径

每段（站 1→站 2，弧长 ΔMD，角度 i/A）：

```
cos β = cos i1·cos i2 + sin i1·sin i2·cos(ΔA)     全狗腿角（含方位，非 2D 近似）
RF    = (2/β)·tan(β/2)                             β→0 时 RF→1
Δtvd   = ΔMD/2 · (cos i1 + cos i2) · RF
Δnorth = ΔMD/2 · (sin i1·cos A1 + sin i2·cos A2) · RF
Δeast  = ΔMD/2 · (sin i1·sin A1 + sin i2·sin A2) · RF
```

方位差取短弧（±180° 内插）。数值验证锚：0→90° 造斜（ΔMD=1000m，方位不变）
`Δtvd = Δnorth = 2000/π m`；纯方位 90° 水平转向段 `Δnorth = Δeast = 500·4/π m`
（2D 近似会得 RF=1，被 `tst_deviation::azimuthTurnUsesFullDogleg` 钉死排除）。

## 插值与外延策略

- **站间**：任意 MD 视为「自站 1 起、终点角度按狗腿弧长线性内插」的子段，
  同一公式重算；t=1 与整段增量恒等，站点 MD 精确命中免 ULP。
- **首站前**：井口锚 (0,0,0)@MD0，按首站姿态直线（测斜常以 MD0/incl0 起表）。
- **末站后**：按末站姿态直线外延（水平段 cos i→0 时垂深不增）。
- **tvdToMd**：`pointAt` 的站间二分反解（往返双精度收敛）。初版站表线性反插
  在 1200m 站距 40° 造斜段往返差 ~20m，实测后换掉。水平段无垂深增量 →
  返回段首 MD（不外推猜值）。
- **站表校验**：空表 / 非有限值 / MD<0 / 井斜 ∉ [0,180] / 排序后 MD 重复 →
  `fromStations` 拒绝并写原因（导入不静默吞）。方位角归一 [0,360)。

## 角色与域切换面

- 角色用 RoleRegistry 内置 `trajectory`（井实体、primary=当前生效版本，
  非有序角色无 ordinal 面）；实体面板/数据页改角色下拉天然可见。
- 综合图（`ui/wellcomposite`）：深度轴恒 MD 主显，TVD/TWT 走读数尾缀与副
  刻度（D6.1/D6.5 既有面）；`DepthTransform` 的 MD↔TVD 已统一为域模型口径
  （坏表禁用 + 原因文案，公共 API 不变）。
- 属性建模/剖面按轨迹自动吃 TVD 域（站表 z=TVD），无单独「域切换」开关——
  有测斜即真实轨迹，无测斜即直井，是数据语义不是显示选项。

## 消费面迁移清单

| 面 | 行为 |
|----|------|
| `PropertyModelWorkflow::requestFromCatalog` | 井筒站 = 曲线端点 + 区间内测斜站（x/y=井口+位移、z=TVD，严格递增去重）；无测斜保持垂直路径（x/y 恒井口、z=MD）。粗化算法 `stratgrid/upscale` 本就按站段 x/y/z marching 求交，未改动。provenance 记 `n_trajectory_wells`（paramHash 天然含站点坐标） |
| 剖面 `SectionWorkbench::sectionWells` | 填 `SectionWellInfo::trajectory`（md/tvd/x/y/twtMs；twt 优先 TVD 校准回退 MD）并补 `bottomX/Y`（TD 超末站按姿态外延）；dock `computeWellTrajectories` 逐站投影成 `TrajVertex`，canvas 顶点 ≥2 画折线（halo+墨线），否则原两点垂直简化 |
| 平面图 | `paleo::wellTrajectoriesGeoJson`（`src/workflow/welltrajectorylayer.*`，纯函数）组装 surface→站点→TD 投影 LineString（CRS 同 wells 层局部网格 WKT；坏测斜记 error 不静默降级）；组装根 `AppContext::refreshWellTrajectoriesLayer` 落盘 `artifacts/layers/well_trajectories.geojson` + 声明 `00_Data/井轨迹投影` + `applyTrajectoryLayerStyle`（DESIGN.md text 墨色实线 0.6——实测轨迹不用虚线）。无已决测斜不写文件不声明；曾有过现已全解除 → 覆写空 FeatureCollection（无 undeclare 面，图层如实零要素） |
| 综合图 | `DepthTransform` 委托域模型（替换原 2D 近似：dogleg 忽略方位、无水平位移；顺带修掉单站表外延的越界隐患）。D6.1 既有测试零改动通过（夹具全常数方位，两口径等价） |
| 3D | **递延**：`Seismic3DWell.trajectory` 渲染面已在（≥2 点折线），但 `setWells` 无生产喂数通道（仓内仅测试喂数）——接线属 seismic3d 喂数面工作，不在此虚做 |

## 验证

| 轮 | 断言 | 结果 |
|----|------|------|
| 1 | `tst_deviation` 9 例：垂直恒等 / 造斜-稳斜闭式手算（2000/π）/ 子段手算 / 站点恒等 / 往返 1e-6 / 误差面 4 类 / 方位全狗腿 / 外延边界 | ✅ 9/9 |
| 2 | `tst_deviation_import` 8 例：目录/文件名/XML 三判据 + 零误判 + 曲线表优先级 / 文本解析哨兵行 / primary 链接受管回读 / 未决不建井 / plan 期双候选歧义 | ✅ 8/8 |
| 3 | `tst_welllogset_tvd` 10 例：未链接 nullopt 无错 / 文本·XML 站表 / 坏表拒绝 / 恒等 / 非恒等闭式锚点 / 多文件逐文件 / 列 0·越界·单位诚实面·ft 折算 | ✅ 10/10 |
| 4 | `tst_sectiontrajectory` 6 例（轨迹闭式 / 投影横向距离 / 无测斜垂直简化 / 平面端点=井底投影 / 空产出）；`tst_propworkflow` 扩定向井站表 + 井口死柱判别器（直井无值、定向井中深层 6.0、代表柱离开井口柱）+ 老工程 `trajectoryWellCount==0` | ✅ 6/6 + 13/13 |
| 5 | `tst_deviation_realarea`（`PALEO_REAL_PROJECT_AREA` 门控）真机：20 井全 nullopt 回退、无坏链接、平面层空产出、剖面轨迹全空 | ✅ 实测通过（未设 env 时 QSKIP） |

回归：`tst_arearules`（钉死表更新至 5 规则）、`tst_ingestplan`、`tst_propfill`、
`tst_propmodelpanel`、`tst_wellcomposite_*`（DepthTransform 口径统一零扰动）、
`tst_welllog_consumers`、`tst_seismic_section*`、`tst_upscale`、`tst_sections_alignment`
等 17+ 项全绿；`tools/check_layering.py --strict` 绿。

## 递延

- 多分支井/侧钻（sidetrack）：显式拒绝（单分支语义，站表按一井一条 primary
  `trajectory` 链接管理，历史版本挂非主链接）。
- 最小曲率以外的高级插值（radius of curvature / 样条 / cubic）。
- 扭转角（toolface）、井径校正、磁场/重力校正。
- LAS 内 DEV 曲线段当测斜源（LAS 仍按 `well_log` 导入，测斜源只认
  `trajectory` 角色链接）。
- 3D 视口井轨迹生产喂数（渲染面已备，缺喂数通道）。
