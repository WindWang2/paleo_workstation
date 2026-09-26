# QGIS 原生能力采纳审计（QGIS_NATIVE_ADOPTION）

> 指令：与 QGIS 相关的内容——图层管理、画布、地图工具、拓扑、矢量编辑等——
> 一律直接使用 QGIS 原生源码/控件，不自造一套。本文是审计表 + 裁定记录。
>
> 审计基线：QGIS 4.2.2（/usr/include/qgis + /usr/lib/libqgis_*.so，
> `nm -D` 符号证据）；本仓 `src/qgis/` 是薄适配层不是重写层。
> 审计日期：2026-09-27。证据命令在各行标注。

## 裁定词表

- **原生**：已在用 QGIS 官方类，无自造对应物。
- **app-only 豁免**：功能存在于 QGIS 但类在 `libqgis_app.so`（APP_EXPORT、
  头文件不安装）——无法链接；降级实现须用原生构件拼装并书面记档。
- **领域豁免**：QGIS 无对应物（地质领域语义），自造合法。

## 审计表

| 能力 | 现状 | 裁定 | 证据 |
|---|---|---|---|
| 图层树管理 | `QgsLayerTreeView` + `QgsLayerTreeModel`（AllowNodeReorder/Rename/ChangeVisibility） | ✅ 原生 | `paleomainwindow.cpp:423` |
| 地图画布 | `QgsMapCanvas`（canvascontroller 懒建持有）+ `QgsRubberBand` | ✅ 原生 | `qgiscanvascontroller.cpp:47` |
| 画布装饰 | `QgsMapDecoration` 派生（比例尺/指北针/网格/水印） | ✅ 原生 | `paleodecorations.h:28` |
| 绘制地图工具 | 全部派生 `QgsMapToolCapture`（Constraint/Polygon/Rect/Point/Circle/Ellipse） | ✅ 原生 | `paleomaptools.h:17` 等 |
| 矢量编辑会话 | `QgsVectorLayer` edit session + `changeGeometry` + `beginEditCommand` undo | ✅ 原生 | `qgiseditingservice.cpp`、`editingtools.cpp:282` |
| 撤销栈 | `PaleoUndoStack` = `QgsMapLayer::undoStack()` 的观察包装（非自造栈） | ✅ 原生 | `editingundostack.h` 头注 |
| 属性表 | `QgsAttributeTableView` + `QgsAttributeTableModel` + `QgsVectorLayerCache` + filter model | ✅ 原生 | `attributetablepanel.cpp:43` |
| 定位器 | `QgsLocatorFilter` 子类 + `QgsLocatorWidget::registerFilter` | ✅ 原生 | `paleolocatorfilters.h:14`、`paleomainwindow.cpp:1743` |
| 图件设计器 | `QgsLayoutDesignerInterface` shell + `QgsLayout`/`QgsLayoutItem*` | ✅ 原生 | `layoutdesignershell.cpp:54` |
| 处理框架 | `qgisprocessingservice`（QgsProcessingRegistry 面） | ✅ 原生 | `qgisprocessingservice.*` |
| 顶点编辑器 | `QgsVertexEditor` 在 libqgis_app，头不安装 | ⚠️ app-only 豁免 | `vertexeditorshim.h:16` 已记档 |
| 顶点编辑工具 | `QgsVertexTool` 同上（nm 证据在头注）→ `QgsMapToolEdit` + `QgsVertexMarker` + edit-buffer 变更 | ⚠️ app-only 豁免 | `vertexeditortools.h` 已记档 |
| SEG-Y 剖面预览 | Grayscale8 QImage 自绘 | ✅ 领域豁免 | 地震剖面 QGIS 无对应物 |
| 连井剖面 | QGraphicsScene（correlationtrack/depthruler/horizonmarkers） | ✅ 领域豁免 | 连井对比 QGIS 无对应物 |
| GeoJSON 临时配准 | `geojsonaffine` 手工仿射 + 水印 | ✅ 领域豁免 | 临时配准语义 QGIS 无对应物（D11 记档） |
| 地层/SEG-Y/时深解析 | `segyreader`/`horizonbinner`/`timedeptool` | ✅ 领域豁免 | 领域解析器 |
| **捕捉（snapping）** | ~~零配置~~ → `nativeSnappingConfig`（AllLayers + Vertex+Segment + 10px）装到 `canvas->snappingUtils()`，工程打开镜像 `project->setSnappingConfig`（随 .qgz 持久化） | ✅ **已原生**（本提交） | `qgiscanvascontroller.cpp:70`、`paleomainwindow.cpp:1238`；钉死测试 `tst_canvas_tools::nativeSnappingEnabled` |
| **拓扑合法性** | ~~提交路径无验证~~ → `QgisEditingService::geometryCommitError`（原生 `QgsGeometryValidator`，QgisInternal 引擎）接入 `commitFeature` 与 `addConstraint`——非法几何如实拒收，不静默 makeValid | ✅ **已原生**（本提交） | `qgiseditingservice.cpp:120`；测试 `tst_edittools::geometryCommitGateValidatesNatively`、`tst_workflows::addConstraintRejectsInvalidGeometry` |
| 图层树右键菜单 | ~~无菜单~~ → `QgsLayerTreeViewDefaultActions`（gui 已安装）组 zoom/feature-count/rename/remove 菜单；`QgsLayerTreeViewMenuProvider` 是 app-only 豁免 | ✅ **已原生**（本提交） | `paleomainwindow.cpp:431` |

## 采纳执行项（全部落地于本提交）

1. **snapping 原生启用** ✅：`QgsSnappingConfig`（enabled、AllLayers、
   Vertex+Segment、10px）→ `canvas->snappingUtils()->setConfig`（创建时装）
   + `project->setSnappingConfig`（工程打开时镜像，随 .qgz 持久化）。
   capture 工具经 `QgsMapCanvas::snappingUtils()` 自动拾取——零工具改动。
2. **提交拓扑门** ✅：`QgisEditingService::geometryCommitError` —
   `QgsGeometryValidator`（QgisInternal，与 QGIS app「检查几何有效性」
   同源）提取错误文本；接入 `PaleoAddFeatureTool::commitFeature` 与
   `ConstraintWorkflow::addConstraint`。非法几何如实拒收、不报不猜；
   不静默 makeValid（修形结果要由人看见再入库）。
   语义边界：null 几何报 empty、空但合法的退化几何（`POLYGON EMPTY`）
   放行——空 ≠ 拓扑违例。
3. **图层树默认动作** ✅：`QgsLayerTreeViewDefaultActions` 组右键菜单
   （zoom-to-layers/selection、feature count、rename、remove）——
   `QgsLayerTreeViewMenuProvider` 是 app-only（头不安装），不链接。

再验证 grep 证据（本提交后）：

```
$ grep -rn "snappingUtils\|QgsGeometryValidator\|QgsLayerTreeViewDefaultActions\|setSnappingConfig" src/
→ 12 命中（canvascontroller/editingservice/editortools/paleomainwindow）
```

`ctest` 全量 **68/68 绿**（新增 3 用例：nativeSnappingEnabled /
geometryCommitGateValidatesNatively / addConstraintRejectsInvalidGeometry）。

## 非目标

- 不重写任何已判「原生」的路径（表中已证）。
- QgsVertexEditor/QgsVertexTool 豁免不变——QGIS 把宿主 UI 留在 app 层
  （`qgis_app`），上游移植回 gui 前 shim 保持，头注已记「移植回来即用真品」。
