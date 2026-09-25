你在 paleo_workstation 项目补全矢量编辑工具集（digitizing/edit tools），
把编辑 shim 升级为能真正改图层要素的工作流。这是一个大体量任务（多交付物、
多阶段），按 ZCode agent 工作流执行。

【执行方式】在 ZCode 中以 agent/teamwork-preview 模式执行：自行拆解子任务
并行开发。子任务建议划分（可调整）：
  A) 添加要素工具（点/线/面 add feature，走 QgsMapToolCapture + commit）
  B) 顶点编辑（vertexeditorshim 真实化：拖动顶点/加顶点/删顶点/undo）
  C) 整形/移动/删除要素（QgsMapToolEdit reshape/move/delete 要素级操作）
  D) 编辑撤销栈（QgsVectorLayerEditBuffer + QUndoStack 集成，图层 undo/redo）
  E) 编辑工具栏 UI（QToolBar 宿主，非 modal）+ 状态联动（编辑态高亮）
  F) 测试贯通
Lead 负责分派、合并冲突裁决、最终验收。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui），
moc=/usr/lib/qt6/moc，QT_QPA_PLATFORM=offscreen + QTest。
QGIS 4.2 GUI 已有 QgsMapToolCapture、QgsMapToolEdit、
QgsVectorLayerEditBuffer（undo 原生）、QgsMapToolAdvancedDigitizing。
现有 shim 参考 src/ui/vertexeditorshim.{h,cpp}；绘制工具参考
src/ui/maptools/paleomaptools.cpp（坐标变换/生命周期模板照抄）。
CAD dock 注入模式见现有 maptools—— 不要跨文件共享内部函数，复制小段即可。
moc 自包含陷阱注意。

【第一步：建 worktree】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-edittools -b wave/edit-tools
cd ../pw-edittools

【产品边界】编辑行为走 QGIS 原生 QgsMapTool* + edit buffer；Paleo 层只
做工具生命周期管理和业务联动（活动层位约束下只能编辑该层位的图层）。
不允许自绘编辑 rubber band 或自定义几何改变逻辑。

【交付物】
1. src/ui/edittools/editingtools.{h,cpp} — 工具集：
   - PaleoAddFeatureTool(QgsMapToolCapture 按 CapturePoint/Line/Polygon
     参数化) — 画完即 commit 到当前 editable 图层的 edit buffer；
   - PaleoReshapeTool(QgsMapToolEdit) — 画 reshape line 修改选中要素；
   - PaleoMoveTool(QgsMapToolEdit) — 拖拽移动选中要素；
   - PaleoDeleteFeatureTool — 删选中要素；
   全部统一发出 featureEdited(QString layerId)/editAborted() 信号；
   生命周期（activate/deactivate/cadDock）照抄 paleomaptools 模式。
2. src/ui/edittools/vertexeditortools.{h,cpp} — 顶点编辑真实化：
   vertexeditorshim 是真 shim 的占位注释——在其旁边新建真顶点编辑器：
   QgsVertexTool（如果 gui 可导出）或 QgsMapToolEdit + 手工 rubber band
   顶点命中；拖顶点改几何、双击空白加顶点、右键删顶点；
   undo 走 edit buffer（QgsVectorLayerEditBuffer 自带 undo）。
   如果 QgsVertexTool 被证实不可用（app-only）就在报告里写清证据并降级
   为 QgsMapToolEdit + 手动 rubber band。
3. src/ui/edittools/editingundostack.{h,cpp} — 编辑 undo：
   QgsVectorLayerEditBuffer::undoStack() 已存在——包装成 PaleoUndoStack：
   undo()/redo()、canUndo/canRedo 信号、跨图层 undo（当前编辑层唯一）、
   保存后清栈或保留（选一个，注释说明）。
4. src/ui/edittools/editingtoolbar.{h,cpp} — 工具栏宿主：QToolBar
   「编辑」内含 select/add/reshape/move/delete/vertex/save/cancel/undo/redo
   actions + 图层选择 combo（只列 editable 图层）；editingStarted/editingStopped
   信号；当前编辑图层高亮。新 QWidget 工具栏，非 modal，非 dock。
5. tests/tst_edittools.cpp — 全面覆盖：add feature 落进 edit buffer
   （featureCount 变化）、vertex drag 改几何、reshape 删顶点、undo/redo
   回退、编辑态图层联动、save/cancel 边界、abort 路径。

【纪律】TDD 每子任务红→绿；standalone /tmp 编译；worktree 内多 commit。
不碰：CMakeLists.txt、paleomainwindow.*、appcontext.*、pagepanels.*、
workflows.*、seismicpreviewpanel.*、segyreader.*、constraintstore.*、
paleoshapetools.*、correlationpanel.*、layoutdesignershell.*、
paleomaptools.*、layout/、correlation/、其余现有 tst_*。
完成后 worktree 分支 git commit -a（仓库惯例 trailer），
汇报测试数、文件清单、集成注意。
