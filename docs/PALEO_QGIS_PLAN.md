# Paleo Workbench 古地理编图开发文档

——基于 QGIS Vendor 的古地理分析、约束编辑、综合编图与成果验证

建议文档版本：V1.0
适用范围：Paleo Workbench C++ 主程序 / QGIS Vendor 集成层
核心原则：QGIS 负责 GIS，Paleo Workbench 负责地质业务。

结合当前界面设计，系统已经形成较清晰的业务主链：
数据管理 → 智能预测 → 约束与单因素 → 综合编图 → 验证

后续古地理编图不建议继续自行实现一套地图、图层、矢量编辑、空间分析和版式系统，而应尽可能建立在 QGIS Vendor 提供的成熟能力上。Paleo Workbench 重点实现地质领域的数据组织、工作流、专业算法、井—震—图联动和业务约束。

QGIS 的 `QgsProject` 本身可以统一管理地图图层、样式、布局、注记等项目状态，`QgsMapCanvas` 可以作为嵌入式地图画布，因此非常适合作为 Paleo Workbench 的空间底座。

## 1. 开发目标

古地理编图模块的目标不是开发一个"类似 QGIS 的地图软件"，而是在 QGIS 空间基础设施上构建面向沉积相分析与古地理编图的专业工作台。

最终应该支持：

```
工区 / 层位
   │
   ├── 原始资料
   │    ├── 井
   │    ├── 测井
   │    ├── 分层
   │    ├── 地震
   │    └──解释成果
   │
   ├── 智能预测
   │    ├── 沉积相预测
   │    ├── 地震相预测
   │    └── 测井相预测
   │
   ├── 地质约束
   │    ├── 物源线
   │    ├── 展布线
   │    ├── 控制点
   │    └── 地质解释边界
   │
   ├── 单因素图
   │    ├── 砂体厚度
   │    ├── 砂地比
   │    ├── 地层厚度
   │    ├── 孔隙度
   │    ├── 渗透率
   │    ├── 距井距离
   │    └── 预测置信度
   │
   ├── 综合古地理编图
   │    ├── 沉积相边界
   │    ├── 相区
   │    ├── 物源方向
   │    ├── 展布方向
   │    ├── 地名 / 井名
   │    └── 地图整饰
   │
   └── 验证
        ├── 井验证
        ├── 地震验证
        ├── 解释成果对照
        ├── 问题记录
        └── 成果发布
```

其中 GIS 能力原则上优先由 QGIS 提供，只有 QGIS 不适合承担的测井专业绘图、地震剖面、AI 推理、地质知识逻辑等能力才由 Paleo Workbench 自研。

## 2. 总体技术架构

建议形成四层架构。

```
┌───────────────────────────────────────────────┐
│                Paleo Workbench UI             │
│ 数据管理 │ 智能预测 │ 约束单因素 │ 综合编图 │ 验证 │
└───────────────────────────────────────────────┘
                         │
┌───────────────────────────────────────────────┐
│             Paleo Geological Domain           │
│ Workflow │ Version │ Provenance │ Validation │
│ Well-Seismic-Link │ Facies │ Constraint      │
└───────────────────────────────────────────────┘
                         │
┌───────────────────────────────────────────────┐
│              QGIS Integration Layer           │
│ Project │ Canvas │ Layer │ Editing │ Processing│
│ CRS │ Style │ Label │ Layout │ Task          │
└───────────────────────────────────────────────┘
                         │
┌───────────────────────────────────────────────┐
│                QGIS Vendor                    │
│ qgis_core │ qgis_gui │ qgis_analysis │ GDAL   │
│ PROJ │ GEOS │ GeoPackage │ Raster/Vector     │
└───────────────────────────────────────────────┘
```

其中应特别避免业务代码直接散布 QGIS API 调用。建议增加一层稳定的 `qgis-integration`，以后无论 Vendor 使用 QGIS 3.x 还是迁移到 QGIS 4.x，只修改这一层。

## 3. QGIS 能力映射

| Paleo 功能 | 建议 QGIS 实现 |
|-----------|---------------|
| 工区工程 | `QgsProject` |
| 地图显示 | `QgsMapCanvas` |
| 图层管理 | `QgsLayerTree` / `QgsLayerTreeModel` |
| 矢量数据 | `QgsVectorLayer` |
| 栅格数据 | `QgsRasterLayer` |
| CRS | `QgsCoordinateReferenceSystem` |
| 空间坐标转换 | `QgsCoordinateTransform` |
| 选择/拾取 | `QgsMapTool` |
| 图形编辑 | QGIS Digitizing Framework |
| 捕捉 | `QgsSnappingConfig` |
| 拓扑编辑 | QGIS Topological Editing |
| 空间分析 | QGIS Processing |
| GDAL算法 | QGIS GDAL Provider |
| IDW | QGIS Interpolation |
| TIN | QGIS Interpolation |
| 等值线 | GDAL / QGIS Processing |
| Raster Calculator | QGIS/GDAL |
| Polygonize/Rasterize | Processing |
| 样式 | `QgsRenderer` / `QgsStyle` |
| 标注 | QGIS Labeling Engine |
| 图例 | QGIS Layout |
| 比例尺 | QGIS Layout |
| 指北针 | QGIS Layout |
| 版式 | `QgsLayout` / `QgsPrintLayout` |
| PDF/SVG/PNG | `QgsLayoutExporter` |
| 后台任务 | `QgsTask` |
| 空间索引 | QGIS Spatial Index |
| 工程模板 | QGZ + QPT + QML |

QGIS Processing 本身就是一个统一的地理处理框架，可以调用 QGIS 原生算法及第三方 Provider。因此本项目不建议自行开发第二套"GIS算法调度系统"，Paleo 的算法服务应优先成为 Processing 的调用器和领域封装器。

## 4. 工程与数据组织

### 4.1 一个 Paleo 工区对应一个 QGIS Project

建议：

```
project_area/
│
├── project.qgz
│
├── project.gpkg
│
├── raw/
│   ├── well/
│   ├── seismic/
│   ├── raster/
│   └── vector/
│
├── derived/
│   ├── prediction/
│   ├── single_factor/
│   └── interpolation/
│
├── result/
│   ├── facies/
│   └── validation/
│
├── styles/
│   ├── facies.qml
│   ├── source_line.qml
│   └── well.qml
│
├── layouts/
│   └── paleo_map.qpt
│
└── metadata/
    └── project.sqlite
```

`project.qgz` 是空间显示状态和 QGIS 工程状态的权威来源。
`project.gpkg` 则作为主要矢量成果容器，包括：

- study_boundary
- wells
- source_lines
- distribution_lines
- control_points
- facies_polygons
- facies_boundaries
- annotations
- validation_issues

原始数据继续遵守当前项目的数据生命周期原则：
Raw 数据不可修改。任何编辑必须产生 Working/Derived 数据或新版本。

这一点不建议完全依赖 QGIS 实现，因为"业务版本、标签、来源关系、处理历史"属于 Paleo 的领域逻辑。QGIS 管空间数据和工程状态，Paleo Metadata Store 管版本关系、数据血缘和状态。

## 5. 图层体系

现在界面已经有"主要图层 / 参考图 / 单因素图 / 约束要素"等概念，建议直接映射到 QGIS Layer Tree：

```
C6
│
├─ 01_Base
│   ├─ 工区边界
│   ├─ 井位
│   └─ 地名
│
├─ 02_Prediction
│   ├─ AI沉积相预测
│   └─ 预测置信度
│
├─ 03_Constraints
│   ├─ 物源线
│   ├─ 展布线
│   └─ 控制点
│
├─ 04_SingleFactor
│   ├─ 砂体厚度
│   ├─ 砂地比
│   ├─ 地层厚度
│   ├─ 孔隙度
│   ├─ 渗透率
│   └─ 距井距离
│
├─ 05_PaleoMap
│   ├─ 沉积相面
│   ├─ 相界线
│   ├─ 物源方向
│   └─ 展布方向
│
├─ 06_Reference
│
└─ 07_Validation
```

UI 里的"资源管理器"和右侧"图层面板"不要重新维护两份独立状态。
应当建立：

```
QgsProject
      ↓
QgsLayerTree
      ↓
Paleo LayerTree Adapter
      ↓
左侧资源树 / 右侧图层面板
```

即：
QGIS Layer Tree 是唯一图层状态源。
显示、隐藏、排序、透明度和样式改变最终都落到 QGIS 图层对象。

这样可以从根本上解决过去容易出现的：
- 图层勾选了但不显示；
- 左右两个图层面板状态不一致；
- 删除图层后 Canvas 仍存在；
- 透明度和实际绘制不一致；
- 图层顺序和地图渲染顺序脱节。

## 6. 数据管理模块

对应当前截图中的"数据管理"页面。

### 6.1 QGIS 负责什么

QGIS 负责：
- 文件格式识别；
- 数据 Provider；
- CRS；
- 图层加载；
- 属性读取；
- 几何信息；
- extent；
- spatial index；
- 栅格 Metadata；
- 数据预览；
- 坐标转换。

GIS 文件导入不要自行再开发完整解析器，优先通过 QGIS Provider/GDAL 读取。
QGIS 的 GDAL Processing Provider 已经封装了大量 Raster/Vector 数据处理能力。

### 6.2 Paleo 负责什么

Paleo 增加：

```
DataAsset
 ├── UUID
 ├── name
 ├── type
 ├── role
 ├── layerId
 ├── horizon
 ├── wellId
 ├── version
 ├── source
 ├── checksum
 ├── parentVersion
 ├── status
 ├── createdAt
 └── provenance
```

形成当前 UI 中：
数据列表 / 数据属性 / 版本历史 / 关联关系 / 数据血缘。

建议不要复制 QGIS 已经保存的数据路径、CRS、Provider 等信息，而是保存 QGIS layer UUID，然后通过 LayerService 获取。

## 7. 智能预测模块

当前"智能预测"页面的设计非常适合继续保留：

```
              QGIS MapCanvas
                    │
        ┌───────────┴────────────┐
        │                        │
 Seismic Section            Well Plot
        │                        │
        └──────── Selection ─────┘
```

其中地图必须使用 `QgsMapCanvas`。
地震剖面和测井轨迹继续采用现有 C++ 专业组件，不必强行塞进 QGIS。

关键是建立统一的：

```
GeoSelectionContext
{
    horizonId;
    wellId;
    seismicLineId;

    x;
    y;

    depth;
    twt;

    featureId;
}
```

当地图选择 A12：

```
QGIS井位选中
       ↓
SelectionContext
       ↓
测井窗口 → A12
地震窗口 → 定位对应道
属性面板 → A12
```

反过来点击测井或地震，也通过 SelectionContext 定位 QGIS 地图。

`QgsMapCanvas` 原生提供 Layer、Extent、MapTool、CRS、Feature Flash 等地图交互能力，因此地图定位和高亮应直接依赖 Canvas API。

## 8. 约束与单因素分析

这是后续最应该深度 QGIS 化的部分。

### 8.1 物源线、展布线

不要使用单独的自绘 Graphics Object。
应定义为真正的：

```
QgsVectorLayer
Geometry = LineString
```

例如物源线：

```
id
horizon
direction
type
confidence
source
version
locked
comment
```

展布线同理。
箭头、虚线、颜色由 QGIS Symbol Renderer 完成。

这样它们天然具备：
- GIS 坐标；
- 保存；
- 编辑；
- Undo；
- 捕捉；
- 查询；
- 属性表；
- 输出；
- reprojection；
- spatial analysis。

## 9. QGIS 矢量编辑框架

截图中的：
- 编辑物源线；
- 展布线；
- 控制点；
- 编辑相界；

应尽量使用 QGIS Digitizing。

不要自己处理：
MouseDown / MouseMove / Vertex / Drag / Delete / Snap / Geometry Update

而是通过 `QgsMapTool` 和 QGIS 编辑机制完成。

特别是沉积相 Polygon 编辑，必须使用 QGIS 的：
Snapping + Topological Editing。

QGIS 本身支持点、线、面编辑、顶点/线段捕捉、交叉点捕捉及拓扑编辑，适合维护相邻沉积相多边形之间的公共边界。

例如：

```
三角洲前缘
████████████│
            │ ← 公共边界
────────────┼────────
            │
滨浅湖
████████████│
```

如果拖动公共边界，两侧 Polygon 应同时改变，而不是分别修改两个 Polygon。

## 10. 单因素图计算

截图中的：
- 砂体厚度；
- 砂地比；
- 地层厚度；
- 孔隙度；
- 渗透率；
- 最近井距离；
- 预测置信度；

建议统一组织成：

```
SingleFactorDefinition
        ↓
Input Vector/Raster
        ↓
QGIS Processing
        ↓
Raster
        ↓
QGIS Renderer
        ↓
Single Factor Layer
```

### 10.1 IDW

优先调用 QGIS Interpolation Provider。
QGIS 已经提供正式的 IDW 插值算法，因此普通空间 IDW 无需 Paleo 自己重写。

例如：

```
井点砂厚
   ↓
QGIS IDW
   ↓
C6_sand_thickness.tif
```

## 11. 地质约束插值

这里要区分：

普通 IDW → 直接 QGIS。

Paleo 约束 IDW，如：
井点 + 物源方向 + 展布方向 + 地质边界

这种算法属于 Paleo 的地质专业能力。
但仍应实现成：

```
PaleoConstraintInterpolationAlgorithm
             ↓
QgsProcessingAlgorithm
```

而不是另外建立算法运行系统。
也就是说：

```
QGIS Processing
 ├─ QGIS IDW
 ├─ QGIS TIN
 ├─ GDAL Grid
 ├─ Contour
 ├─ Raster Calculator
 │
 └─ Paleo
      ├─ Constraint IDW
      ├─ Facies Constraint Fusion
      └─ Geological Smoothing
```

从用户角度它们都是"算法"，从开发角度也使用同一个任务、参数和输出体系。

## 12. TIN 与等值线

对于需要保持离散地质控制点关系的场景，可以使用 QGIS TIN。
当前 QGIS 的 TIN Interpolation 可以直接输出：
- 插值 Raster；
- Triangulation Vector。

等值线则继续调用 QGIS/GDAL Processing。
因此截图中的：

```
生成等值线
间距 20 m
```

底层直接映射：

```
Raster
   ↓
Contour Processing Algorithm
   ↓
QgsVectorLayer
```

生成真实的 GIS LineString，而不是 Canvas 临时绘制线。

## 13. 综合古地理编图

这是整个系统的核心。
建议数据组织为：

```
AI Prediction Raster
       +
Single Factor Raster
       +
Source Line
       +
Distribution Line
       +
Well Interpretation
       +
Seismic Interpretation
       ↓
Initial Facies
       ↓
Geologist Editing
       ↓
Facies Polygon
       ↓
Cartographic Styling
       ↓
Paleo Geographic Map
```

其中最终沉积相成果应优先转换成：
`QgsVectorLayer<Polygon>`
而不是只保存最终渲染图。

## 14. 沉积相数据模型

例如：

```
facies_polygon
────────────────────────
fid
horizon
facies_code
facies_name
subfacies
microfacies
confidence
source_type
prediction_ver
edit_ver
review_status
created_at
updated_at
```

如：
- 01 三角洲前缘
- 02 三角洲平原
- 03 滨浅湖
- 04 湖相泥
- 05 深湖

QGIS 使用 Categorized Renderer，根据 `facies_code` 渲染。
这样当前截图中的颜色体系可以成为正式的 QGIS Style。

## 15. 相界编辑

"编辑相界"建议对应以下状态：

```
Facies Polygon Layer
        ↓
Start Editing
        ↓
QGIS Map Tool
        ↓
Vertex / Split / Merge / Reshape
        ↓
Topology Check
        ↓
Save New Version
```

同时使用 QGIS 的：
- Snapping；
- Topological Editing；
- Fix Geometry；
- Snap Geometry to Layer。

QGIS Processing 本身也提供几何捕捉等 Geometry Algorithm，可用于保存前质量处理。

## 16. 综合编图中的参考图

当前综合编图页面下面已经有：
- 厚度图
- 砂地比图
- 坡度图
- 最近井距离图
- 预测置信度

这个设计建议保留。
但不要生成单独 Thumbnail 状态。
每一张都是实际 QGIS Layer。
Thumbnail 只是：

```
QgsMapRenderer
        ↓
Preview Image
```

点击"厚度图"：

```
activeReferenceLayer = thickness
```

然后：

```
MapCanvas
 ├─ 综合沉积相 100%
 └─ 厚度图 40%
```

透明度直接控制 QGIS Layer Renderer。

## 17. 地图联动

所有地图页面应该共享统一的：

```
MapContext
包含：
extent
scale
CRS
horizon
selected well
selected feature
active layer
```

智能预测、约束分析、综合编图和验证页面切换时，原则上不应该丢失用户正在查看的区域。
即：

```
智能预测 C6 / A12
       ↓
进入约束分析
       ↓
仍保持 C6 / A12 / 相同范围
       ↓
综合编图
       ↓
仍保持相同空间上下文
```

这是 Paleo Workbench 作为"专业工作台"与单纯多个独立窗口的重要区别。

## 18. 图件整饰

当前截图中的：
- 标注；
- 图例；
- 指北针；
- 比例尺；
- 模板；
- 纸张；
- 预览；
- 图件输出；

这里应该全面转向 QGIS Layout。

QGIS Layout 可以管理地图、文字、图例、比例尺、图片等图件元素，并支持模板化版式。

因此 Paleo 不建议自己编写另一套：
LegendWidget / NorthArrowWidget / ScaleBarWidget / TitleWidget / PrintCanvas

应改成：

```
QgsPrintLayout
 ├── QgsLayoutItemMap
 ├── QgsLayoutItemLegend
 ├── QgsLayoutItemScaleBar
 ├── QgsLayoutItemLabel
 ├── QgsLayoutItemPicture
 └── ...
```

## 19. 古地理图模板

建议形成真正的 QGIS `.qpt` 模板：

- 标准古地理图.qpt
- 会议汇报图.qpt
- A4横版.qpt
- A3横版.qpt
- 科研论文图.qpt

Paleo 只负责填写变量：

```
@project_name
@horizon
@map_title
@author
@date
@scale
@data_source
@version
```

最终生成：C6层沉积相平面图

QGIS Layout 支持模板加载，适合建立 Paleo 的专业地图模板库。

## 20. 成果导出

对应当前"导出图件"。
统一使用：`QgsLayoutExporter`

支持：
- PDF
- SVG
- PNG
- TIFF

尤其 SVG 对后续论文和科研图件二次编辑非常重要。
QGIS 官方 `QgsLayoutExporter` 提供 PDF、SVG 和 Raster Image 等布局导出接口。

建议成果结构：

```
result/
└── C6/
    └── v5/
        ├── C6_paleo_map.gpkg
        ├── C6_paleo_map.tif
        ├── C6_paleo_map.pdf
        ├── C6_paleo_map.svg
        └── metadata.json
```

即"图"和"数据"同时存在。
不能只有 PNG。

## 21. 验证模块

当前验证页面设计是合理的，应继续发展为：

```
当前成果
      ↕
参考解释
      ↕
井
      ↕
地震
```

验证结果建议形成真正的数据实体：

```
validation_issue
──────────────────────
id
horizon
object_type
object_id
geometry
check_type
severity
status
current_value
reference_value
reviewer
comment
created_at
resolved_at
```

问题不仅显示在右侧列表，也可以作为：
`QgsVectorLayer`
在地图上可定位。

因此点击：
A12 相带不一致

即可：

```
Issue
 ↓
井 A12
 ↓
QGIS Canvas zoomToFeature
 ↓
Map highlight
 ↓
Well Plot depth 640–665 m
 ↓
Seismic Section
```

形成真正的"空间验证工作台"。

## 22. 工作流状态

五大页面不应该只是五个 Tab。
后台应该有明确状态机：

```
DataReady
   ↓
PredictionReady
   ↓
ConstraintReady
   ↓
SingleFactorReady
   ↓
CompositionReady
   ↓
ValidationReady
   ↓
Published
```

例如：
- 没有选择层位：[运行预测] disabled / [计算单因素] disabled / [综合编图] disabled
- 有井数据但没有结果：[运行预测] enabled / [综合编图] disabled
- 已有综合图：[送交验证] enabled

这也是 QGIS 工具必须受 Paleo Domain Workflow 控制的地方。

## 23. QGIS 工具状态管理

建议增加：`ToolAvailabilityService`

综合：
- WorkflowState
- ActiveLayer
- LayerType
- Editable
- Selection
- TaskRunning

计算按钮状态。

例如：
编辑物源线
只有：
- 当前页面 = Constraint
- AND 当前层位 != null
- AND source_line layer exists
- AND 没有运行任务

才可用。
这样可以解决 UI 中大量"按钮什么时候应该可用"的问题。

## 24. 后台任务

插值、Raster 操作、预测结果导入、图件导出等操作不能阻塞 UI。
统一接入：`QgsTask`

QGIS 自带后台任务体系，`QgsTask` 就是长时间后台任务的抽象基类。

Paleo 再封装：

```
PaleoTaskManager
    │
    └── QgsTaskManager
```

UI 中右上角「任务」正好可以直接显示：

```
插值 C6砂体厚度      72%
AI Prediction         Running
Export C6 Map         Queued
```

## 25. 推荐 C++ 模块划分

建议最终形成：

```
src/
├── qgis/
│   ├── QgisRuntime
│   ├── QgisProjectService
│   ├── QgisLayerService
│   ├── QgisCanvasController
│   ├── QgisEditingService
│   ├── QgisProcessingService
│   ├── QgisStyleService
│   └── QgisLayoutService
│
├── domain/
│   ├── Project
│   ├── Horizon
│   ├── DataAsset
│   ├── GeologicalLayer
│   ├── SingleFactor
│   ├── Facies
│   ├── Constraint
│   ├── ValidationIssue
│   └── Version
│
├── workflow/
│   ├── PredictionWorkflow
│   ├── ConstraintWorkflow
│   ├── CompositionWorkflow
│   └── ValidationWorkflow
│
├── algorithms/
│   ├── PaleoProcessingProvider
│   ├── ConstraintIDW
│   ├── FaciesFusion
│   └── GeologicalSmoothing
│
├── linkage/
│   ├── SelectionContext
│   ├── WellMapLink
│   └── SeismicMapLink
│
└── ui/
```

其中：
UI 不允许直接依赖大量 Qgs* 类。
UI 调用：LayerService / MappingService / ProcessingService
这些 Service 再与 QGIS 交互。

## 26. QGIS Vendor 使用边界

这一点对当前 C++ 化尤其重要。
建议 Paleo Workbench 主要依赖：
- qgis_core
- qgis_gui
- qgis_analysis

尽量不要依赖：qgis_app

原因是 `qgis_app` 很多内容属于 QGIS Desktop 应用自身实现，耦合较强。
我们需要的是：QGIS SDK，而不是把 QGIS Desktop 整个嵌到 Paleo Workbench。

因此：
- QGIS Core
- QGIS GUI
- QGIS Analysis
- GDAL
- PROJ
- GEOS

作为 GIS 基础设施。
Paleo Workbench 自己负责主窗口和业务 UI。

## 27. QGIS 版本隔离

建议 Vendor 固定版本后增加：`QgisCompatibility`

任何可能随版本变化的 API 都经过这一层。
例如：

```cpp
class IQgisProcessingAdapter;
class IQgisProjectAdapter;
class IQgisLayoutAdapter;
```

业务代码禁止直接判断：`#if QGIS_VERSION_INT ...`
否则后期升级 QGIS Vendor 会非常困难。
同时应坚持使用 QGIS 公开 API，不依赖 private headers。

## 28. 推荐的古地理编图核心架构

最终可以归纳为：

```
                     Paleo Workbench
                           │
            ┌──────────────┼──────────────┐
            │              │              │
           井             地震            GIS
            │              │              │
            └────── Geological Context ───┘
                           │
                ┌──────────┴─────────┐
                │                    │
          AI Prediction        Geological Rules
                │                    │
                └──────────┬─────────┘
                           │
                    QGIS Processing
                           │
          ┌────────────────┼────────────────┐
          │                │                │
      单因素Raster      约束Vector       预测Raster
          │                │                │
          └────────────────┼────────────────┘
                           │
                    综合沉积相 Layer
                           │
                    QGIS Digitizing
                           │
                    Geologist Edit
                           │
                    Facies Polygon
                           │
                     QGIS Layout
                           │
                PDF / SVG / TIFF / GPKG
                           │
                       Validation
```

## 29. 第一阶段优先重构内容

结合目前截图展示的开发程度，我建议当前不要再次大规模重做 UI，而应首先把 UI 后面的 GIS 状态统一起来。

**P0：QGIS 基础统一**
- QgsProject 成为工程 GIS 状态源
- QgsLayerTree 成为图层状态源
- QgsMapCanvas 替换自维护地图状态
- 统一 CRS / Extent / Selection / LayerId

**P0：QGIS 矢量编辑**
- 物源线
- 展布线
- 控制点
- 沉积相边界

全部真正进入 `QgsVectorLayer`。

**P0：QGIS Processing**
- IDW
- TIN
- Contour
- Raster Calculator
- Rasterize
- Polygonize
- Distance
- Geometry Fix

先使用 QGIS 原生算法。

**P1：专业算法 Provider**
再逐步增加：
- Paleo Constraint IDW
- Paleo Facies Fusion
- Paleo Geological Smoothing

**P1：综合编图**
重点解决：
- 预测 → Vector Facies
- 单因素参考
- 地质约束
- 相界编辑
- 样式

**P1：QGIS Layout**
替换自定义地图排版，实现：
- 图例
- 指北针
- 比例尺
- 标题
- 模板
- 纸张
- PDF/SVG

**P2：Validation**
把当前验证 UI 与：
- QGIS Feature
- 井
- 地震
- 问题记录

真正连接起来。

## 30. 开发验收标准

完成本轮后，一个完整业务场景应当能够做到：

```
01 创建工程 project_area
       ↓
02 导入井、地震、分层
       ↓
03 选择 C6
       ↓
04 运行沉积相预测
       ↓
05 QGIS 显示预测 Raster
       ↓
06 绘制物源线、展布线
       ↓
07 根据井数据运行单因素插值
       ↓
08 QGIS 生成砂厚/砂地比等 Raster
       ↓
09 综合形成初始沉积相
       ↓
10 转为 Facies Polygon
       ↓
11 QGIS 拓扑编辑相界
       ↓
12 图层样式与整饰
       ↓
13 QGIS Layout 成图
       ↓
14 保存 C6 v5
       ↓
15 送交验证
       ↓
16 点击 A12 问题
       ↓
17 地图 + 测井 + 地震同步定位
       ↓
18 完成复核
       ↓
19 PDF/SVG/GPKG 成果发布
```

完成这一流程后，截图中目前展示的五个页面才真正成为一个统一的古地理编图工作流，而不是五套互相连接不够紧密的工具。

## 31. 最重要的架构结论

后续开发建议严格执行一个边界：

**QGIS 已经有的 GIS 能力，原则上不再自行实现。**

具体而言：

交给 QGIS：
Project / Layer / Map Canvas / CRS / Raster/Vector / Symbology / Label / Selection / Digitizing / Snapping / Topology / Processing / Interpolation / GDAL / Geometry / Spatial Index / Layout / Map Export / Task

Paleo Workbench 专注：
工区 / 层位 / 井/震/图业务关系 / 数据生命周期 / 版本与数据血缘 / 地质约束 / 沉积相专业逻辑 / AI预测 / 井震图联动 / 专业单因素定义 / 地质融合算法 / 验证流程 / 专业工作流

这样项目实际上会从现在的：
"Paleo Workbench 内嵌了一些 QGIS 能力"

逐步转变成：
"Paleo Workbench 是建立在 QGIS GIS Engine 之上的古地理专业解释与编图系统。"

我认为这才是当前这套 C++ + QGIS Vendor 架构最合理的长期定位。尤其是截图中的图层管理、物源线/展布线编辑、单因素插值、综合相图编辑、图件整饰以及验证定位，都非常适合建立在 QGIS 的现有能力之上，而不应该再重复造 GIS 基础设施。
