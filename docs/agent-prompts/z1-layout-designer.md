你在 paleo_workstation 项目把图件设计器（layout designer）从壳子升级为产品级
制图编辑器。这是一个大体量任务（多交付物、多阶段），按 ZCode agent 工作流执行。

【执行方式】在 ZCode 中以 agent/teamwork-preview 模式执行：自行拆解子任务并行
开发。子任务建议划分（可调整）：
  A) 元素工具栏 + 添加项（map/legend/scalebar/label/picture/north arrow/shape）
  B) 属性面板（QgsLayoutItemPropertiesWidget 宿主 + 自定义 property overrides）
  C) 模板库 + 导入导出（.qpt 模板保存/加载、QgsLayoutExporter 出图 PNG/PDF/SVG）
  D) 撤销/重做栈（QgsLayoutUndoStack）+ 状态持久化
  E) 测试贯通（每个子任务配 QTest，最后全量）
Lead 负责分派、合并冲突裁决、最终验收。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui -lqgis_analysis），
moc=/usr/lib/qt6/moc，测试 QT_QPA_PLATFORM=offscreen + QTest。
QGIS 4.2 GUI 已自带：QgsLayoutView、QgsLayoutDesignerInterface（本仓库
src/ui/layoutdesignershell.{h,cpp} 已实现）、QgsLayoutItemPropertiesWidget、
QgsLayoutItemGuiRegistry（含 createItemGuiGroup/新项拖拽工厂）、
QgsLayoutExporter、QgsLayoutAtlas、QgsLayoutUndoStack。
先读 src/ui/layoutdesignershell.{h,cpp} 和 tests/tst_layoutshell.cpp 了解现状。
Qt MOC 注意：mocs_compilation.cpp 里所有 Q_OBJECT 头要能被独立 include——
头文件自包含（补全 QHash/QPair/QStringList 等容器 include）。

【第一步：建 worktree（由 lead 先建好，子任务共享）】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-layout -b wave/layout-designer
cd ../pw-layout

【产品边界】QGIS 负责 GIS——制图排版/导出全部走 qgis_gui 原生组件；
Paleo 只做业务接线（当前工程、manifest、活动层位、页面跳转）。
不要自绘设计器画布；QgsLayoutView 就是画布。

【交付物】
1. src/ui/layout/layoutitempanel.{h,cpp} — 属性面板宿主：包装
   QgsLayoutItemPropertiesWidget，加项目级 overrides（当前活动层位标题注入、
   比例尺联动 preset）。暴露 setItem(QgsLayoutItem*)、itemChanged 信号。
2. src/ui/layout/layoutitempalette.{h,cpp} — 元素工具面板：add map / legend /
   scalebar / label / picture / north arrow / shape / polygon / polyline /
   marker / attribute table / manual table / elevation profile / text table /
   3D map / page properties。至少实现 8 类主元素 + 5 类次要元素的新增按钮，
   通过 QgsLayoutItemGuiRegistry 的标准工厂路径创建（不要手写 new QgsLayoutItemXxx
   后裸 add——走 registry 以保持将来兼容性）。暴露 itemRequested(type) 信号。
3. src/ui/layout/layoutexportactions.{h,cpp} — 导出：PNG/PDF/SVG 三格式，
   使用 QgsLayoutExporter::exportToImage/exportToPdf/exportToSvg；分辨率
   对话框、地图页范围选项；导出完成后 statusbar 消息 + 打开所在目录。
4. src/ui/layout/layouttemplates.{h,cpp} — 模板：save-as-template（.qpt）/
   load-from-template / 内置 3 个默认模板（A4 横/A4 纵/A0 大图）随资源打进
   二进制或落地 docs/templates/ 并能在 UI 里一键应用。
5. src/ui/layout/layoutundostack.{h,cpp} — QgsLayoutUndoStack 包装：undo/redo
   actions 挂到设计器菜单/快捷键，状态在切换页面后仍可撤销。
6. src/ui/layoutdesignershell.{h,cpp} — 集成上面全部：设计器壳现在应有
   menubar（File/Items/Layout/Settings 四个菜单）、左侧元素面板、右侧属性
   面板、中央 QgsLayoutView、底部 status bar + zoom controls + page navigator。
   保留现有接口签名（mainwindow 接线由父层收尾）。
7. tests/tst_layoutdesigner_full.cpp — 全面覆盖：加 map item 后 view 有内容；
   export 产生非空 PNG/PDF；模板保存/加载 roundtrip 元素数量+尺寸保持；
   undo/redo 在加/删/改后正确回退；属性面板 setItem 后控件值反映 item 状态；
   offscreen 下全部可跑。

【纪律】TDD：每个子任务先写测试（红）再实现（绿）。standalone /tmp 编译，
别等主构建。允许在 worktree 内多次 commit（每个子任务一个 commit）。
不碰：CMakeLists.txt、paleomainwindow.*、appcontext.*、pagepanels.*、
workflows.*、seismicpreviewpanel.*、segyreader.*、constraintstore.*、
paleoshapetools.*、correlationpanel.*、vertexeditorshim.*、paleomaptools.*、
现有 tst_* 测试文件（新增自己的测试文件可以）。
完成后在 worktree 分支 git commit -a（仓库惯例 trailer），
汇报：各子任务测试数、改动/新增文件清单、给父层的集成注意事项。
