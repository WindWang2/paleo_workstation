# goal/fault-surface — 断棒成面与断距量算

- 分支：`goal/fault-surface-20261002`（自 `origin/master` `eaaf46d`）
- 目标：同一断层的多剖面断棒 → 三角网断面 → 产状/断距 → 工程往返 →
  3D 视口与剖面交线 → 属性充填的体域阻断。

## 语义决策

**棒的坐标。** `FaultStick.points` 仍是剖面内 `(traceFrac, 纵向样值)`。
断面不在 FaultSet 里查测网。调用方给 `SurveyFrame`：原点 IL/XL 与地图 XY、
inline/xline 的世界步长、道号在对向测线上的 traceFrac 范围、`zScale`。
世界坐标 `Z` 向下为正，`Z = sample * zScale`。Inline 上
`xl = lerp(xlineMin, xlineMax, traceFrac)`；Xline 对称；任意线的
traceFrac 是路径的水平弧长比例。

**纵向域。** `FaultStick.verticalDomain` 为 `TwtMs`（默认，JSON `"twt"`）
或 `Depth`（`"depth"`）。缺键当 TWT。成面前任一棒与第一根不一致 →
`MixedDomain`，不换算、不混域。

**单剖面。** 少于 2 根棒，或少于 2 个不同 `matchKey`，状态
`TooFewSections`，文案含「单剖面断层不成面（拒绝向上下外推）」。
不做向上下外推。

**分叉与走向。** 同一剖面上两根棒的 traceFrac 范围和纵向范围同时重叠
（容差 `1e-4`）→ `Branching`。剖面中点沿主方向的转角，以及棒的水平
端到端切向夹角，超过 75° → `DirectionBreak`。Y 型、多分支、从断层多边形
反插断面，见 TODOS，本轮不做。

**三角网。** 顶点只来自棒上的点。相邻剖面按三维弧长归一化拉链。
面积加权法向翻到 `nz ≥ 0`。倾角是 `acos(|nz|)`，0 为水平，90 为直立。
倾向方位从正北顺时针。走向 = 倾向方位 − 90°，再折进 `[0, 360)`。

**断距。** 不改 `FaultHorizonCut` 的 WKT 与上盘字段。新的量算是
`measureSlip(mesh, footwallPlane, hangingPlane)`：两条交线在走向重叠段上
取 9 个站。`throwZ` 是垂向差的绝对值。heave 是水平分离里垂直于下盘交线
走向的分量。

**网格放哪。** 内存在 `Fault::surface`。`fault_set.payload` 继续只存棒和
切割（`withoutSurfaces()`）。新列 `surface`（可空 TEXT）存按断层 id 分键的
网格 JSON。`user_version` 从 1 进到 2；缺列时 `ALTER TABLE`。旧行该列 NULL，
棒和切割不改写。工作流另写 DERIVED 文件 `fault-surface.json`，父版本是调用方
给出的 FaultSet 版本。`src/catalog` 持久层不改。

**体域阻断。** `fillIdw` 的 `FaultSegment` 重载仍是整柱竖帘。新增
`FaultTriangle` 重载，6 连通按单元中心连线是否穿过三角形来分块，权威结果在
`PropertyVolume::cellBlock`。空的 `{}` 与竖帘重载二义，调用处要写成具体的
vector 类型。

## 接口

- `buildFaultSurface(fault, frame) → SurfaceBuildResult`
- `validateMeshTopology(mesh) → MeshTopology`
- `surfaceAttitude` / `attitudeAt`
- `measureSlip(mesh, footwall, hanging) → SlipCurve`
- `intersectSurfaceWithSection(mesh, frame, section) → SectionCut`
- `segmentIntersectsTriangles` / `segmentIntersectsMesh`
- `FaultSurfaceWorkflow::build` / `produce` / `saveToStore` / `sectionCut`
- `fillIdw(grid, seeds, mesh, power, out, progress, error)`
- 视图：`makeFaultSceneMesh`、`fitFaultSceneCamera`、`faultSceneBoundsInsideFrustum`；
  画布 `setFaultSurfaceCut`；dock 原样转发。

`scripts/new_module.sh` 只登记顶层模块。`faultsurface` 在既有 `algorithms`
下，层标记是「数据」，不跑该脚本。

## Oracle

命令在 worktree `build/`，`PATH` 以 `paleo-qgis-deps\Library\bin` 和
`paleo-qgis-prefix\bin` 开头，`QT_PLUGIN_PATH` 指
`Library\lib\qt6\plugins`（Qt 6.11.2 的 qsqlite；`Library\plugins` 里那份是
Qt5 conda）。`QT_QPA_PLATFORM=offscreen`。

1. **成面。** `tst_faultsurface`：3 条 inline、倾角 30° 的平面，法向倾角 /
   方位 / 走向都在 ±0.5° 内；顶点落在棒上；拓扑无退化、无非流形边、有边界边。
   点数不同且剖面打乱后顶点 12、棒序 `s-early, s-mid, s-late`。单棒
   `TooFewSections`。重叠棒 `Branching`，混域 `MixedDomain`，走向突变
   `DirectionBreak`。
2. **断距。** 同一平面上 z=100 与 z=200：throw 100、heave `100/tan(30°)`，
   绝对误差 0.05；交线弧长 200±1。
3. **往返。** `tst_faultsetstore` 9/9。含断面的网格字段相等；`user_version=1`
   且无 `surface` 列的旧库打开后版本为 2，棒/切割还在，断面为空。
4. **编排。** `tst_faultsurfaceworkflow` 5/5。单棒不成面、缺父版本，都不增加
   catalog 版本行。成功的 DERIVED 版本 `parentVersionIds` 是源 FaultSet 版本。
5. **视口。** `tst_faultsurfaceview` 5/5。地质 Z 向下变成场景 Y 向上；约千米
   尺度的包围盒把裁剪远平面推过默认的 1000，八个角点落在视锥内。视口在 GL
   初始化前就能读到三角形数。未拾取的 inline 1 交点 traceFrac 0.5、样值
   `500*tan(30°)`、Y=100（`sectionCutOnUnpickedInline`）。剖面回归
   `tst_faultsectionui` 9/9。本机 offscreen 下 `QOpenGLWidget` 不支持，
   没有走到 `paintGL`；挂载在 `paintGL` 里调用 `FaultSurfaceRenderer::Render`。
6. **性能。** `hundredByTwoHundredUnderThreeSeconds`：

   `fault-surface perf: 24 ms for 100 x 200 (budget 3000 ms, ratio 0.008000)`

   顶点 20000，三角形数小于 `2*100*200`，拓扑通过。比率 24/3000。
7. **真工区。** `PALEO_REAL_PROJECT_AREA` 未设置。
   `/home/kevin/projects/paleo_project/data/project_area` 与本机对应路径都不存在。
   `testdata/project_area` 最大的 SEG-Y 是 4 MB，不是 966 MB 成果。
   不把夹具耗时写成实测。

`tst_faultsurface` 合计 10/10，43 ms（含上面的 24 ms 成面）。

## 递延

- Y 型、多分支、断面自动生长、从断层多边形反插。检测后失败，不生成假面。
- 本机 `tst_metastore::staleLockAutoRecovered` 在能启动 `sh` 时仍拒绝回收锁
  （QLockFile 把刚退出的 pid 当成存活）。该测试不读断面列。其余 metastore
  用例在本轮 13 通过、此 1 失败。
