你在 paleo_workstation 项目把连井剖面（well correlation panel）从单曲线
升级为产品级多道对比视图。这是一个大体量任务（多交付物、多阶段），按
ZCode agent 工作流执行。

【执行方式】在 ZCode 中以 agent/teamwork-preview 模式执行：自行拆解子任务
并行开发。子任务建议划分（可调整）：
  A) 多曲线道轨框架（每井多 track，track 容器组件）
  B) LAS 曲线浏览器（井文件 → 多 mnemonic 选择 → 动态加道）
  C) 层位对比线 + flatten-on-marker 模式
  D) 深度标尺 + 比例尺控件
  E) 井列排序/拖拽 + 性能（50+ 井不卡）
  F) 测试贯通
Lead 负责分派、合并冲突裁决、最终验收。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui），
moc=/usr/lib/qt6/moc，QT_QPA_PLATFORM=offscreen + QTest。
QGIS 4.2 GUI 已有 QgsLineChartPlot（真折线渲染器，在独立
qgslinechartplot.h）、Qgs2DXyPlot、QgsPlotRenderContext。
现有实现参考 src/ui/correlationpanel.{h,cpp}（单曲线版，用
QgsLineChartPlot 渲 QImage 再嵌 scene），tests/tst_correlation.cpp。
LAS 解析器 src/io/lasparser.{h,cpp} 已就绪。
注意 moc 头文件自包含陷阱（QHash/QPair/QVector 都要显式 include）。

【第一步：建 worktree】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-corr -b wave/correlation
cd ../pw-corr

【产品边界】连井剖面是地质业务视图，用 QGIS 原生 plot 渲染曲线本体；
井列框/层位标线/标尺壳壳可以自绘（业务 chrome），但曲线渲染必须走
QgsLineChartPlot，不允许手写 QPainterPath 折线。

【交付物】
1. src/ui/correlation/correlationtrack.{h,cpp} — 单条曲线道轨组件：
   渲染一条曲线（QgsLineChartPlot → QImage）、标题、单位、颜色可选；
   宽度可调；NaN 断点由多 series 表达（现有实现已做，复用）。
2. src/ui/correlation/correlationwellcolumn.{h,cpp} — 井列容器：纵向 N 个
   track 并排（横向并排也行——选一个，注释说明取舍）、共享深度轴、
   列头（井名 + 井号）、列级高亮/选中、track 增删。
3. src/ui/correlation/curvebrowser.{h,cpp} — LAS 曲线浏览器面板：
   列出当前井 LAS 的所有 mnemonic，勾选即在该井列加/删对应 track；
   显示单位/描述；多选支持。
4. src/ui/correlation/horizonmarkers.{h,cpp} — 层位标线：横向贯穿所有
   井列的对比线（可命名层位名 + 颜色），支持交互拖拽调深度、
   flatten-on-marker 切换（剖面展平到选中层位）、标线增删；
   数据来源用 manifest 里 horizon=* 声明的层位 + SelectionContext 活动层位。
5. src/ui/correlation/depthruler.{h,cpp} — 深度标尺：左侧纵向刻度条，
   跟随视图缩放，单位 m/ft 可选，主次刻度 + 网格线。
6. src/ui/correlationpanel.{h,cpp} — 集成：现在是多井多道的完整面板；
   保留现有 loadWellLas / setWells / wellClicked 信号签名（mainwindow
   接线由父层收尾）；井列拖拽排序；空态提示不变。
7. tests/tst_correlation_full.cpp — 全面覆盖：多 track 并存、curve browser
   勾选驱动道数增减、层位标线跨列、flatten 模式几何变换正确、深度标尺
   刻度可读、50 井渲染 < 3s（性能断言）、selection 联动不回归。
   原 tests/tst_correlation.cpp 不许改——在独立新文件里写。

【纪律】TDD 每子任务红→绿；standalone /tmp 编译；worktree 内多 commit。
不碰：CMakeLists.txt、paleomainwindow.*、appcontext.*、pagepanels.*、
workflows.*、seismicpreviewpanel.*、segyreader.*、constraintstore.*、
paleoshapetools.*、layoutdesignershell.*、paleomaptools.*、tst_correlation.cpp、
其余现有 tst_*。新测试文件名 tst_correlation_full.cpp。
完成后 worktree 分支 git commit -a（仓库惯例 trailer），
汇报测试数、文件清单、集成注意。
