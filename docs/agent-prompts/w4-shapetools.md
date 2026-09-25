你在 paleo_workstation 项目补全约束绘制的剩余形状工具。

【执行方式】本任务必须使用 teamwork-preview 模式执行：自行拆解子任务
（点工具 / 圆工具 / 椭圆工具 / 测试），三个工具实现可用 teamwork-preview
并行开 worker，测试汇总后统一跑。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2，moc=/usr/lib/qt6/moc，offscreen。
QGIS 4.2 关键 API：QgsMapToolCapture 的 lineCaptured(const QgsCurve*) 是
所有权转移、polygonCaptured(const QgsCurvePolygon*) 是借用指针（要先 clone）；
CaptureTechnique::Shape 独立不可用（app-only registry），必须用点捕获自算。
层 CRS→canvas CRS 变换逻辑见 src/ui/maptools/paleomaptools.cpp 已实现的
PaleoDrawRectTool — 照抄其模式（先读它！）。

【第一步：建 worktree】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-shapes -b wave/shapetools
cd ../pw-shapes

【交付物 — 新文件，不碰现有 maptools 文件】
1. src/ui/maptools/paleoshapetools.h/.cpp：
   - PaleoDrawPointTool（QgsMapToolCapture, CapturePoint）：单击提交
     constraintDrawn("Point (x y)")（canvas CRS）；Esc/右键 abort。
   - PaleoDrawCircleTool（CapturePolygon, StraightSegments）：两个左键=
     圆心+半径点 → "Polygon ((...))" 24 顶点正多边形近似；右键带1点时
     用光标位作半径点提交；裸右键/Esc abort。
   - PaleoDrawEllipseTool：中心+两个轴端点（3次点击）→ "Polygon" 36 顶点
     椭圆近似。
   全部 signals：constraintDrawn(QString wkt)/drawAborted() — 与现有工具一致；
   activate/deactivate 按 paleomaptools.cpp 的模式（startCapturing/stopCapturing）；
   cadDock 默认 nullptr → canvas-owned（resolveCadDock 逻辑复制过来，别共享
   跨文件内部函数——把那个 static 函数在你文件里再造一份小的）。
2. tests/tst_shapetools.cpp — 仿 tests/tst_maptools.cpp 的事件注入脚手架
   （QgsMapCanvas offscreen + click() helper + configureCanvas）：点单击即发；
   圆两点 → Polygon WKT 24+1 顶点闭合、圆心在 bbox 中心；椭圆三点 → 闭合
   多边形、长短轴对；各自的 abort 路径。

【纪律】TDD；/tmp standalone 编译（参照 tst_maptools 的 compile_commands 配方）。
不碰 CMakeLists.txt / paleomaptools.* / 其他模块。worktree 分支
git commit（仓库惯例 trailer）；报告。
