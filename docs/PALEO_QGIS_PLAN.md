# Paleo Workbench 古地理编图开发文档

——基于 QGIS Vendor 的古地理分析、约束编辑、综合编图与成果验证

建议文档版本：V1.0
适用范围：Paleo Workbench C++ 主程序 / QGIS Vendor 集成层
核心原则：QGIS 负责 GIS，Paleo Workbench 负责地质业务。

2026-10-05 增补（用户要求）：数据管理之后增加「地层对比」ribbon 页，以 QtWebEngine 承载独立交付的 Web 工作台。主仓库只做宿主与连接/启动适配；网络与进程编排在功能层，页面显示在视图层。独立源码、模型、井资料与解释数据库保留在外部目录，不进入 CMake、安装包或 GitHub；地址与机器路径仅进本机 QSettings。该工作台使用自己的工程，不要求先打开 Paleo/QGIS 工程。使用与验证见 `docs/STRATIGRAPHIC_WEB.md`。

**2026-10-03 实现对账**：本文的「建议/最终/应该」保留为目标架构；当前落点
以 `AGENTS.md`、`BUILDING.md` 与 `docs/progress/` 交付账本为准。
六页现行文案为「数据管理 → 地层对比 → 智能预测 → 单因素图 → 智能编图 → 验证」
（DESIGN.md / #43），下文旧文案是最初的业务模块名。
QGIS 工程/层树/画布、约束 CRUD、预测/融合/验证、布局导出已有实现；
仍需专家/真工区验收的项继续列在 TODOS.md，不能由计划条目推定已完成。

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

`project.qgz` 是空间显示状态和 QGIS 工程状态的权威来源——**但仅限已实例化图层的渲染态**；图层集合（含未实例化的声明）的权威是 `metadata/project.sqlite` 的声明式清单（§37），.qgz 是其投影而非副本。
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

~~优先调用 QGIS Interpolation Provider。~~（2026-10-01 修订：QGIS
Interpolation Provider 是 Python-only 路径，C++ 仓不可复用——实测见
docs/ALGORITHM_AUDIT.md。实际落线：约束 IDW 自研 `paleo_constraint_idw`
QgsProcessingAlgorithm + 最小曲率 `paleo_min_curvature`
〔goal/gridding-surface-ops〕+ 栅格代数 `src/algorithms/rasteralgebra.cpp`。）
原表述保留为决策历史：QGIS 已经提供正式的 IDW 插值算法，因此普通空间 IDW
曾计划无需 Paleo 自己重写。

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

### 14.1 三级相结果（2026-10-05 用户要求）

MappingWorkbench 的相结果以同一基础面分区及 `facies_code` 分类快照表达
相、亚相、微相父子关系。QGIS 按完整路径分类并合并渲染，三级不保存独立几何。
显示支持自动尺度切换及手动锁定；缺失层级仅在显示时按上下级补齐。
相／亚相改类批量同步所属成员，微相改类同步父级归属；共边拓扑编辑强制开启，
提交前运行 QGIS/GEOS 覆盖验证。证据存入 `facies_evidence`，引用不可变来源版本
并加入版本谱系；分类改变后原证据保留并提示复核。参考 canvas 通过 QGIS
转换同步范围及光标；布局主题随当前层级更新。使用与边界见
[`FACIES_MAPPING.md`](FACIES_MAPPING.md)。

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

**现行实现注记（2026-10-03）**：应用任务由 `services/paleotaskservice`
调度 QThreadPool，`services/jobrunner.{h,cpp}` 承载 prepare/compute/publish
协议（owner 线程准备/发布，worker 计算、协作取消、代次丢弃）。
属性建模与三组单因素作业已接 JobRunner；断层成面、布局导出保留各自三段式，
尚未全部迁入统一 runner。下述 QgsTaskManager 是目标映射，不能作为现行接线说明。

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

现行 workflow 的实现已按预测/约束 CRUD/单因素作业/融合/验证拆为
`predictionworkflow.cpp`、`constraintworkflow.cpp`、`constraintfactorjobs.cpp`、
`compositionworkflow.cpp`、`validationworkflow.cpp`；`workflows.cpp` 保留共用装配。
数据预览的五类正文分别在 `ui/datapreview/datapreviewtab{geojson,horizon,image,seismic,welllog}.cpp`，
共用视图工具在 `datapreviewtabs_internal.h`。具体覆盖对账见任务框架交付账本。

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

---

## 32. CEO 评审补充约束（2026-09-25 /plan-ceo-review 决议）

以下约束在评审中由用户确认，作为全局前提：

1. **C++ only，不内嵌 Python。** Vendor 不包含 PyQGIS 运行时。
   - 影响：QGIS Processing 中 `gdal:*` provider 算法（contour、rasterize、polygonize、gdal grid 等）是 Python 实现，在 C++-only vendor 下不可用。
   - 决议：`native:*` C++ 算法（含 `native:idwinterpolation`、`native:tininterpolation`、fix geometries、snap geometries 等）正常走 Processing；GDAL 系能力由集成层直接调用 GDAL C++ API 封装为 `QgsProcessingAlgorithm` 子类实现，不引入 Python。
   - 待验证项：逐个确认 §10–12 所列每个算法在 `native` provider 下有 C++ 实现；缺的（如 raster contour）走 GDAL C++（`GDALContourGenerate` 等）。
2. **所有依赖库尽量 vendor。** QGIS / GDAL / PROJ / GEOS / Qt 均随产品打包，不依赖系统安装版本。
   - 影响：构建系统需提供 vendored 工具链产物；QGIS prefix path、provider 路径、proj db 路径全部指向 vendor 目录内相对路径。
3. **许可证不构成约束。** 产品本身遵循 GPL，与 vendored QGIS（GPLv2+）兼容；无需商业许可隔离设计，但仍在 vendor manifest 中登记各库许可证备查。

## 33. 自动化测试策略（评审新增；工程评审 D6 补框架）

§30 的验收场景为手工端到端流程，不足以保护集成层。**测试框架 = Qt Test (QTest) + CTest**（随 vendored Qt 零新增依赖；QSignalSpy 覆盖信号断言；QGIS 自身 C++ 测试同框架）。新增测试策略：

- **集成层单元测试**：`QgisProjectService` / `QgisLayerService` 等 service 的契约测试——图层增删、透明度、顺序、CRS 变更后 `QgsProject` 状态与 UI 一致（对应 §5 的五类历史 bug，每类一个回归测试）。
- **算法测试**：Paleo 约束插值、相融合等 `QgsProcessingAlgorithm` 走 QGIS 算法测试模式（固定输入 fixture → 断言输出 raster/vector 的统计量与空间范围）。
- **渲染回归**：相图样式（`.qml`）、Layout 模板输出做 render-comparison 测试（参考 QGIS 自身的 `QgsRenderChecker` 思路），防止 vendor 升级后出图漂移。
- **拓扑正确性**：相界编辑后的公共边界一致性测试——移动共享顶点，断言两侧 polygon 几何保持重合。
- **Golden fixture 工区**：一个小型 project_area（含井、震、分层）作为所有测试与 §30 冒烟验收的固定输入。
- **脊线专项（工程评审）**：① 懒加载 save/restore 往返测试——声明清单→保存→重开→占位节点完整、按需实例化正确；② gpkg 写串行化并发测试——后台任务写 + 编辑提交并发触发，断言无 SQLITE_BUSY、结果完整；③ vendor boot 测试走 CI headless（offscreen）。
- **Vendor 升级门禁**：升级 QGIS vendor 版本时，以上测试套件全量运行；`QgisCompatibility` 层的 API 适配改动必须伴随测试通过。

## 34. 统一 Undo / 编辑会话模型（评审新增）

文档同时存在 QGIS 图层编辑 undo 栈与 Paleo 版本体系，需明确桥接语义：

- **会话级撤销**：进入"编辑物源线/编辑相界"等编辑会话后，所有顶点/要素修改进入该 `QgsVectorLayer` 的 undo 栈，Ctrl+Z 在会话内有效。
- **版本提交边界**：「保存版本」= 编辑会话 commit → Paleo 版本号递增 + provenance 记录；commit 后 undo 栈清空，**undo 不可跨越版本边界**。
- **放弃会话**：roll back buffer，不产生新版本。
- **版本回退**：回退到历史版本 = 从该版本重新检出为新的 working 副本（衍生新分支版本），而不是 undo 回放。
- UI 语义：工具栏撤销只作用于当前未提交编辑；版本历史面板只作用于已提交版本。两者视觉分离，避免"undo 到上一个版本"的误解。

## 35. 工作流门控的可解释性（评审新增，D8）

§22 的硬门控保留，但补充约定：

- **每个被禁用的工具必须能解释原因**：tooltip/状态提示说明"为什么不可用 + 需要什么条件"（如"选择层位后可运行预测"）。禁用但无解释的按钮会被用户当成 bug。
- **硬门控只用于真前置条件**（无层位 → 无法预测；无数据 → 无法插值）。非硬性的工作流建议用**警示**而非禁用（如"未做井验证仍可导出，但导出记录标记未验证"）。
- `ToolAvailabilityService` 除可用性外返回 `reason` 字符串，UI 统一渲染。
- **按图层任务锁（工程评审 D8）**：服务维护 per-layer busy map（task → 写入图层集）；编辑工具对忙图层返回 reason "图层正被任务 N 写入"。任务须在注册时声明写出图层集——替代全局"有任务则禁编辑"的粗粒度门控。

## 36. 沉积相 Raster→Vector 派生管线（评审新增，D9；E5 修订为边界图方案）

§13 "综合形成初始沉积相 → 转为 Facies Polygon" 的管线规格。

**关键修正（E5）**：逐要素 smooth/simplify 会在公共边界上重新引入缝隙/重叠，步骤 6 的"缝隙/重叠=0"在逐要素方案下不可达成；QGIS native 无 coverage-preserving generalize（该能力在 GRASS provider，C++-only 不可用）。因此管线以**共享边界弧（boundary graph）**为简化对象：

```
预测/融合 Raster
   ↓ 1. 分类重编码 (facies_code 离散化)
离散分类 Raster
   ↓ 2. Polygonize (GDAL C++ GDALPolygonize，封装为 QgsProcessingAlgorithm)
初始 Polygon 要素
   ↓ 3. Dissolve by facies_code
   ↓ 4. 构建边界图：提取共享边界弧 (arc) + 结点；polygon = 弧序列引用
   ↓ 5. 弧级平滑/简化：每条弧只简化一次，两侧 polygon 共用结果（保拓扑）
   ↓ 6. 碎屑剔除（逐 part，post-dissolve）：面积 < 阈值的 part 并入最大相邻相
   ↓ 7. 约束要素修正（conflation）：边界弧向物源线/展布线/控制点吸附对齐
   ↓ 8. 由边界图重建 polygon，拓扑检查（缝隙/重叠 = 0）
Facies Polygon Layer (可编辑)
```

质量契约（写入验收）：
- 无面积小于阈值的碎屑 part（阈值按 part 判定，dissolve 后检查）；
- 相邻相区公共边界**由构造保证重合**（同一弧），而非事后断言；
- 无未解释的空洞；
- 每步参数进 provenance（阈值、容差、输入版本）。
- 步骤 7 的 conflation 是定制算法（边界弧的部分几何替换），属护城河组件，见 §40。

## 37. 按层位懒加载（评审新增，D10；工程评审 D4/D10 修订）

- 一个工区一个 `.qgz` 不变；但**只有当前活动层位组的图层实例化**，其他层位在 Layer Tree 中显示为占位节点，切换时按需加载。
- **声明式清单机制（D4）**：Paleo metadata 持有全量图层声明（id/层位/类型/数据源/样式引用）；`Paleo LayerTree Adapter` 把声明投影进 Layer Tree；**保存时 .qgz 由声明集写出，而非实例化集**——未实例化图层序列化为占位引用，下次打开按需解析。manifest 同时供资源树使用。这是脊线设计决策：lazy instantiation 永远不可能丢层。
- `Paleo LayerTree Adapter` 的契约从第一天按懒加载设计：layer 节点 ≠ 已实例化 `QgsMapLayer`。
- **跨层位验证（D10）**：验证任务读声明清单，按层位 instantiate-on-demand、用后释放；实例化图层进入 §35 per-layer busy map，验证写操作走 §41 序列化写队列——懒加载不变式在验证路径上同样成立。
- 内存/打开时间目标随层位数线性而非随总图层数线性（量化预算见 §41.6）。

## 38. 诊断与可观测性（评审新增，D11；同时关闭 Section 2 的 error-surface 关键缺口）

- **运行操作日志**：每次算法/任务/导出运行记录一条结构化日志——输入（id/hash）、参数、时长、输出 id、结果状态。与 provenance 互补：provenance 记"数据血缘"，操作日志记"代码做了什么"。
- **错误呈现契约**：错误分级与去向统一——任务级错误进任务条目+日志页；阻断性错误（工程打不开、保存失败）模态提示；可恢复降级走状态栏。任何错误不得静默。
- **会话诊断包**：一键导出（日志 + 工程状态 + 版本元数据 + vendor 版本），用于现场问题回溯。
- **崩溃报告**：已决——本地转储方案（`.running-<pid>` 会话旗标 + 崩溃报告目录 + 恢复告知，不上报外网）。规格见 docs/CRASH_REPORTING.md。

## 39. Greenfield 定位修正与 Phase 0（外部评审新增，E1/E3/E4/E2）

**仓库事实核查（E1 已定）**：本仓库当前仅有 docs/、TODOS.md、CLAUDE.md、`prototype/` 设计稿 PNG——**无既有代码**。原稿中"现有 C++ 专业组件""不要再次大规模重做 UI"等表述作废：测井曲线、地震剖面、五页 UI 均为**新建项**，原型图仅作设计意图参照。本项目是 greenfield-on-QGIS，不是迁移。

**Embed vs Plugin 决策（E2 已定）**：维持 embed 架构（qgis_core/gui/analysis 嵌入自有 Qt6 shell），不采用"完整 QGIS + C++ 插件"路线——理由：五页工作流 UX 无法在 QGIS dock 外壳中成立，差异化在地质语义不在编辑器复用。代价已确认：`src/app` 层功能（高级数字化面板、顶点编辑器、形状数字化工具、布局设计器 chrome、Processing 对话框）需自研。

**Phase 0 — go/no-go 三个 spike（E3 已定 + 工程评审 D3/D7 修订，先于 P0）**：

0. **Vendor 策略对比（DX 评审 D7 + ET0 已决 → `docs/phase0/et0-vendor-comparison.md`）**：评估完成，**批准 binary vendoring（混合源）**——Windows = OSGeo4W `qgis`+`qgis-devel-4.2.x`+dep 闭包（URL/SHA512 pin）；Linux = qgis.org deb 闭包（`libqgis-dev`+依赖，发行版钉 resolute/trixie）解包进 vendor prefix；ONNX Runtime = 官方 GitHub release（自建 SHA256）。**Linux 宿主 floor = Debian 13/Ubuntu 25.04+ 级 glibc**（QGIS 4.x 需 Qt≥6.6，qgis.org 不为更老发行版出包）——产品约束已接受。Windows ABI 钉 MSVC v14x + /MD；Qt 用 vendor 同源（OSGeo4W qt6-devel / distro qt6-base-dev）。**回退条款**：`apt-get download libqgis-dev=4.2.*` 验证失败或须支持老宿主 → ExternalProject superbuild-on-oldest-target。**2026-10-01 政策修订**：优先级反转——尽量不依赖系统库、尽量自编译 vendored，superbuild 升首选、binary 闭包降加速档、系统包仅兜底（BUILDING.md「依赖来源策略」）；本条 D7 记录保留为历史底账。
1. **Vendor boot spike**：vendored QGIS 在进程内启动。**验收标准（二值判定）**：目标平台矩阵 = Linux x86_64 + Windows x86_64（最低集）；通过 = `QgsApplication` 初始化返回 + provider registry 非空 + `srs.db`/`proj.db`/`GDAL_DATA` 解析成功 + `QgsMapCanvas` 渲染一个 GPKG 图层且输出像素非纯色 + 无阻塞对话框。产出：vendor superbuild 骨架（ExternalProject/构建 flags/依赖清单）、**初始化顺序文档**（env vars → prefixPath → QgsApplication 构造次序，即 P0 boot 规范）、启动自检（§38 第一实例）、CI headless 模式（`QT_QPA_PLATFORM=offscreen`）。**Windows leg 环境**：本机 POSIX 工具链（bootstrap.sh）默认 Linux；Windows 验收在 CI runner（windows-latest + MSVC）上跑，本地 Windows 开发用 git-bash 兼容层或 `paleo-dev.ps1` 对等脚本（spike 交付物之一）。**本地开发机基线**：Linux x86_64、8 核、≥60GB 空闲磁盘 —— TTHW ≤2h 预算以此为准，Windows CI 另计。
2. **算法封装 spike**：一个 `QgsProcessingAlgorithm` 子类封装 `GDALPolygonize` 跑通——证明 C++-only 算法路径成立。
3. **AI 运行时 mini-spike（工程评审 D7 新增）**：候选运行时（默认 ONNX Runtime）编入 vendor 树，进程内跑一个 toy 模型——使 §40 "运行时由 Phase 0 钉定"成为实证而非假设。

三个 spike 任一失败 → 架构层重新评估，不进入 P0。**App-only 审计表（ET9）与 vendor 策略对比（spike-0）同为 go/no-go 输入**——go/no-go 决定建立在主导自研成本已知的基上，而不是事后才量。

**Spike 实测结果（2026-09-25，Arch Linux x86_64, qgis 4.2.2-1 系统包, qt6 6.11.2, gdal 3.13, glibc 2.44, gcc 16.2, c++20）**：
- spike 1 **PASS**：`paleo_selfcheck` 全绿——QgsApplication init、providers=34、srs.db、fixture.gpkg 2 features、渲染非均匀像素断言、`vendor/logs/map.png` 400×300。`tst_boot` QTest 全过（102ms）。注意：QGIS 4.2 头文件要求 **C++20**。
- spike 2 **PASS**：`PaleoProcessingProvider`+`PaleoPolygonizeAlgorithm`（`spikes/polygonize/`）经 `QgsProcessingRegistry::addProvider` 注册（**4.x 已移除 `addAlgorithm` 直挂**），GDALPolygonize 4×4 栅格→≥2 多边形，`tst_polygonize` PASS。
- spike 3 **PASS**：ONNX Runtime 1.30.0 官方 release vendored（`vendor/onnxruntime/`, sha256 pinned in `vendor/manifest.json`, glibc floor 2.28）；`ort_check` 进程内 toy 推理 2.0→42.0 确定性输出。AI 运行时选定 **ONNX Runtime**。
- Windows leg 未实测（按设计走 CI runner）；Linux leg 二值验收全过 → **可进 P0**。

**QGIS 版本钉（E4 + 工程评审事实修正）**：QGIS 4.2 LTR 已发布（4.2.0, 2026-07-03；当前 4.2.2）——**钉死 4.2.x LTR**，不再是"发布后切换"的预期姿态；原"pre-LTR churn 风险"缓解为正常的 LTR 点升级跟踪。API 适配仍走 §27 兼容层，vendor 升级纪律不变（§33 vendor gate）。

**Vendor 依赖清单（评审补全 + DX 评审追加）**：除 QGIS/GDAL/PROJ/GEOS/Qt 外，实际牵连 QCA、QtKeychain、libspatialindex、exiv2、libzip（.qgz 是 zip）、libxml2、sqlite3/spatialite、**OpenSSL**（QCA 默认 provider 与 Qt Network HTTPS 需要），及运行时数据文件（proj.db、GDAL data、srs.db、SVG symbols、Qt platform/imageformat 插件）。**Qt 子模块清单**须在 manifest 中枚举（qtbase/qttools/svg/imageformats 等），禁止 qt-everywhere 整块编译（直接爆 2h 预算）；Linux 端需 X11/GL dev 头文件（qtbase 依赖）。裁剪开关（WITH_3D/WITH_MESH/WITH_PDAL 等）在 Phase 0 决定。**manifest 对每个源包/二进制 pin SHA256**——TTHW 不依赖上游 URL 存活。Qt 为 LGPL：**必须动态链接或提供可重链目标文件**——写入 vendor manifest 一条即可满足。

**App-only 功能审计（ET9 已完成 → `docs/phase0/et9-app-only-audit.md`，release-4_2 @ 53d73a8f）**：结论比预期乐观——4.x 中 `src/app` 构建为 `libqgis_app` 共享库且有 `APP_EXPORT`，但**头文件不安装、无 CMake export config，属非支持链接目标**，仍按源码移植对待。关键事实：①Processing 执行 UI（`QgsProcessingAlgorithmWidgetBase`/batch/toolbox/wrappers）4.x 已全部入 gui——**零移植**；②图层属性对话框、高级数字化面板、QgsMapToolCapture、map tips、devtools 均已在 gui；③唯一深度纠缠项 = `QgsLayoutDesignerDialog`（5k 行、32 处 QgisApp 引用），但 gui 层的 `QgsLayoutDesignerInterface` 就是为自研外壳嵌入设计——D12「嵌完整设计器」落为「实现该接口+组装 gui 部件的自研 designer shell」，成本中高但路径是官方意图；④`QgsVertexEditor` 0 QgisApp 依赖为意外之喜，vertex tool 9 处引用可 shim；⑤24 个 shape 工具、16 个 decorations、11 个 locator filter 均为薄移植（各 ~100-250 行）。

## 40. 护城河组件最小契约（外部评审新增，E6/E7）

以下为一段式契约，完整算法规格留给工程评审：

- **FaciesFusion（相融合）**：输入 = 预测栅格 + 单因素栅格组 + 约束要素（物源线/展布线/控制点）+ 相序规则表；计算 = 以约束要素为硬边界/加权场，对候选相按规则表+证据权重逐像元裁定（规则优先于权重，权重缺省取预测置信度）；输出 = 离散 facies_code 栅格 + 置信度栅格。置信度沿 provenance 记录每个像元的裁定依据。
- **ConstraintIDW（约束插值）**：输入 = 井点值 + 约束线/点（断层、边界、控制点）；计算 = IDW 基础上按约束要素做屏障/方向各向异性修正（跨越物源线的距离按障碍惩罚）；输出 = 连续栅格。屏障语义与方向场参数为待工程评审细化项。
- **边界 conflation（§36 步骤 7）**：输入 = 边界弧集 + 约束要素；计算 = 弧与约束要素的空间关联判定（距离/角度阈值），命中段用约束几何替换弧段，未命中段保留；输出 = 修正后边界图。阈值与关联规则进 provenance。
- **SeismicMapLink（井-震-图联动）**：输入 = GeoSelectionContext（well/层位/深度）；计算 = depth↔TWT 需速度模型（作为工区级 DataAsset，缺省用常速近似并标注），map↔地震剖面定位需 line-geometry↔CDP 映射服务；输出 = 各视图的目标定位参数。**速度模型与 CDP 映射为新建依赖**，此前"现有组件"表述作废（E1）。
- **AI 智能预测（E7 已定契约）**：目标 = **进程内、vendored 推理运行时、模型作为版本化 DataAsset**；运行时选型（ONNX Runtime / libtorch / 其他）由 Phase 0 vendor spike 结果钉定；模型版本与预测输出一并进 provenance。输入=工区数据（井+地震+层位），输出=相概率/预测栅格。离线可用为硬要求（野外工区场景），排除服务端推理。

## 41. 正确性规则补充（外部评审新增，E8 批量）

1. **Raw 不可变强制执行**：导入层加载即 `setReadOnly(true)` + 编辑工具门控双重保护，不只是文档约定。
2. **project.gpkg/.qgz 写契约**（工程评审 D5 细化 + DX 评审补机制）：启用 WAL；**`PaleoProjectStore` 为唯一写汇聚点**。执行机制（不只是纪律）：(a) 服务层边界设全局写互斥——任何要落 gpkg 的调用必须经 `PaleoProjectStore::enqueueWrite()` 取锁；(b) Processing/GDAL 算法输出**先写临时层/临时文件，再由队列合并入 gpkg**（OGR provider 内部写不直接打库）；(c) 编辑提交走 `QgsVectorLayer` 事务前先经同一队列持锁；(d) 写入 gpkg 的栅格（GDAL GPKG driver）同样过队列。任何绕过路径在 WAL 单写者下会以 SQLITE_BUSY 暴露——§33 脊线测试断言此并发压力场景无 BUSY。写次序 = gpkg commit → .qgz 备份 → .qgz 原子写（临时文件+rename）。.qgz 写失败不破坏 gpkg 权威数据态（显示态可再生）；.qgz 同样受备份保护（zip 中途损坏即工程文件损坏）。
3. **SelectionContext 防抖**：广播链（map→well→seismic→map）设 re-entrancy 守卫 + 去抖，禁止 ping-pong 选择风暴。
4. **facies_boundaries 派生规则**：boundary 表永远由 facies_polygons 派生（单一几何事实源），禁止双写漂移；boundary 类型语义沿用 TODOS P2。
5. **reason 字符串 i18n**：服务层可解释性字符串从第一天走 Qt 翻译机制（tr()/qsTr()），不写死中文。
6. **最小 NFR**（工程评审 D9 量化）：工区规模假设（井数 ~10²、层位 ~10¹、栅格 ~10⁷ cell/层）、画布刷新预算（交互操作 <100ms、整图重渲 <2s）。**补充**：>50MB 派生栅格写出时必须建 GDAL overviews（GDALAddo）；工程打开 ≤10s（10 层位 golden fixture）；`PaleoTaskManager` 并发重任务上限 `max(2, cores/4)`；派生栅格默认 cell size = 工区范围/2048（可按图层覆盖）。数字在 Phase 0 实测后可校准。
7. **保存/发布语义**（工程评审任务）：`保存版本`（gpkg 提交+版本记录）、`发布`（导出 result/ 快照）、`Published` 状态三者关系及"发布后能否再编辑"需在工程评审定义状态机。

## 42. UX 与交互规范（设计评审新增，D2–D14；视觉 tokens 以 DESIGN.md 为准）

**信息架构**
1. **启动页（D2）**：首次/无工程打开时显示 `QStackedWidget` 启动页——最近工程列表 + `新建工程`/`打开工程` + 示例数据集入口；加载工程后进入主窗口态。
2. **每页面板清单（D3）**：工程评审前补一张 5 页 × {左面板, 中央, 右 dock, 底部 tab, 默认可见性} 表；连井剖面属 约束与单因素 页 canvas 下方 dock，地震剖面预览属 数据管理 页——防止面板跨页泄漏。
3. **QgsLocator 域过滤器（D4）**：除 QGIS 原生 layers/actions/features 外，注册井名、层位、验证问题编号三个 `QgsLocatorFilter`——命中即跳转地图/记录。

**交互状态**
4. **FEATURE×状态表（D5）**：对 12 个核心特性（资源树、图层树、画布、导入、预测、约束插值、连井剖面、编图、验证表、任务 tab、日志 tab、状态栏）各定义 LOADING/EMPTY/ERROR/SUCCESS/PARTIAL 五种可见状态；空态 = 温暖文案 + 主动作按钮 + 上下文（如资源树空 → "还没有井数据 → [导入井数据]"），禁止空白面板。
5. **峰终时刻（D6）**：预测任务完成 → canvas 自动缩放至结果范围 + 图层短暂高亮 + 任务 tab 出现"查看结果"跳转行；发布 → 确认对话框展示版本号 + 打包内容 + 输出路径。克制、专业、可验证，不做庆祝动画。

**组件决策**
6. **图标（D7）**：GIS 动作用 vendored QGIS 图标集；域名词（井/地震线/层位等 ~15–20 个）绘制同笔画风格 16px 自定义集；禁止多来源图标混用。
7. **导入（D11）**：LAS/SEG-Y 导入用模态 `QWizard`（文件→字段映射→CRS→预览→确认），对齐 QGIS 对话框惯例。
8. **编图 composer（D12）**：嵌入完整 QgsLayout 设计器。**注意**：`QgsLayoutDesignerDialog` 位于 QGIS `src/app`（非 qgis_gui）——与画布装饰同属 §39 app-only 审计清单，实现成本按源码内嵌计，不是免费控件。
9. **层位 chip 溢出（D13）**：chip 条可横向滚动 + `»` 溢出弹层（全量 checklist + 过滤框），保持原型观感并支持 30+ 层位。
10. **Undo 可见性（D14）**：Ctrl+Z/Y 作用于当前画布编辑会话（跨层操作原子撤销）；编辑菜单提供"编辑历史"面板按会话列条目，兼作编辑审计。

**可达性与窗口规则（D10）**
11. 最小窗口 1280×800；低于此宽 dock 自动折叠为 tab。
12. Tab 序 = 阅读序（canvas→左→右→底部→状态栏）；所有自定义控件设 `accessibleName`/`accessibleDescription`。
13. 焦点环 = `focus-ring` token（2px primary 描边），禁用文本 = `text-disabled`，占位 = `placeholder`——禁用控件仍须 §35 reason tooltip。
14. 应用级"减少动画"设置（Qt 无 OS 级 prefers-reduced-motion）：开启后 §1.1 动画全部替换为即时切换。
15. 快捷键表：Ctrl+K 定位器、各页 F1–F5 直达、Esc 取消当前 MapTool；完整表随工程评审定稿。
16. **对比度**：`text-muted` 已调整为 `#5D6E80`（在 surface 与 surface-alt 上均 ≥4.5:1）；语义色永远与文本/图标配对，不做唯一状态载体。

## 43. 工程评审任务清单（/plan-eng-review 2026-09-25，Phase 0 + P0 脊线）

- [x] **ET0 (P1)** — vendor 策略对比（§39 spike-0）→ `docs/phase0/et0-vendor-comparison.md`，批准 binary vendoring（Linux 宿主 floor glibc≥2.41 已接受）
- [ ] **ET1 (P1)** — vendor 构建实现：binary 路下载+SHA256 校验脚本产出可复现 vendor 树。**partial**：`vendor/bootstrap.sh` 已含 preflight + ORT 下载 + 系统包检测；qgis.org deb 闭包提取未实现（本机 pacman 已装，无需求触发；非 Arch 开发机/发行版打包时需要）。
- [x] **ET2 (P1)** — Vendor boot spike **PASS（Linux leg）**：`paleo_selfcheck` 全绿 + `tst_boot` QTest 通过（init/providers=34/srs.db/渲染非均匀像素/map.png）；init 顺序文档 = src/selfcheck/main.cpp + BUILDING.md。Windows leg 待 CI runner。
- [x] **ET3 (P1)** — GDALPolygonize `QgsProcessingAlgorithm` 封装 spike **PASS**：`PaleoProcessingProvider` 注册路径（4.x 无 addAlgorithm 直挂），`tst_polygonize` 通过。
- [x] **ET4 (P1)** — AI 运行时 mini-spike **PASS** → **选定 ONNX Runtime 1.30.0**（vendored，sha256 pinned）；`spikes/onnx/ort_check` 推理 2.0→42.0 确定性。
- [x] **ET5 (P1)** — 脊线四服务 + PaleoProjectStore 写队列 **DONE**（`paleo_core` 库；tst_runtime 8/8 + tst_projectsvc 4/4 绿；saveAll 次序 gpkg→bak→qgz 实测）
- [x] **ET6 (P1)** — 声明式图层清单 **DONE**（LayerManifest→project.sqlite；instantiateHorizon/setActiveHorizon 懒实例化 + 往返测试 tst_layerservice 绿）；LayerTree Adapter 占位投影 UI 挂接待 P1 界面层
- [x] **ET7 (P1)** — ToolAvailabilityService per-layer busy map **DONE**（taskId—reason 契约，tst_canvas_tools 6/6 绿）；§42.2 面板清单表属文档项
- [x] **ET8 (P2)** — **partial**：QTest+CTest harness 骨架就位（`paleo-dev test` + offscreen + 2 测试）；golden fixture 工区与 5 类回归测试待 P0。
- [x] **ET9 (P1, Phase 0)** — app-only 功能审计表 → `docs/phase0/et9-app-only-audit.md`（结论：Processing UI/图层属性已在 gui 零移植；layout designer 走 QgsLayoutDesignerInterface 自研 shell；vertex editor/shape tools/decorations/locator filters 薄移植）。
- [x] **ET10 (P2)** — 性能用例 **DONE**（tst_perf 6/6：project open 18ms vs 10s 预算；并发上限=max(2,cores/4)；GDALBuildOverviews 路径验证；默认像元=工区/2048→0.0049° on golden）
- [x] **ET11 (P1)** — `paleo-dev bootstrap`（preflight 含 glibc floor/磁盘/工具链 + 幂等 vendor 获取 + 尾跑 selfcheck）。
- [x] **ET12 (P1)** — `paleo-dev` 五 verb 入口 + `testdata/fixture.gpkg` 内置最小 fixture + `map.png` 渲染产出。
- [x] **ET13 (P2)** — README.md + BUILDING.md（平台矩阵/依赖/常见失败表/升级流程/实测值）。
- [x] **ET14 (P2)** — 增量构建实测：依赖已装时 3 目标 <1min；记录于 BUILDING.md。

## 44. 开发者体验（DX）规格（/plan-devex-review 2026-09-25，DX POLISH 模式）

目标开发者 = 本项目内部工程师（单人/1-2 人小团队）。本产品的「开发者面」是构建与迭代环境本身 —— vendored QGIS superbuild 是全项目最大 DX 风险面。无公共 SDK/API/社区维度（Pass 7 按设计不适用）。

### 44.1 TTHW 目标与魔法时刻（DX2/DX3）

- **TTHW 目标**：干净机器 clone 之后**仅一条人工命令** `./paleo-dev bootstrap` —— 它跑 vendor 下载+解包（binary 加速档；superbuild 自编译——2026-10-01 起为政策首选，见 BUILDING.md「依赖来源策略」），尾部自动执行 `selfcheck` 收尾输出全绿。**binary 加速档 ~10min；superbuild 首选路 ≤2h**（基线机：Linux x86_64、glibc≥2.41 级发行版、8 核、磁盘 binary 路 ≥15GB / superbuild 路 ≥60GB；Windows 走 CI runner 另计）。「干净机器」= 发行版 + 编译工具链已装（preflight 兜底）。编译耗时是依赖树的物理下限；DX 杠杆压在「人工步骤 = 1」与「失败可诊断」上。
- **Magical moment**：`selfcheck` 不止输出 checklist，还把一个 GPKG 图层（repo 内自带的最小 fixture，见 ET12）离屏渲染到 `vendor/logs/map.png` —— 工程师亲眼看到一张真地图。该 PNG 同时充当：§39 spike 验收证据（非均匀像素断言）、CI artifact、视觉确认。
- selfcheck checklist 输出格式：`✓ providers=N ✓ srs.db loaded ✓ GPKG layer loaded ✓ rendered → <path>`，并打印各阶段耗时。

### 44.2 统一开发入口 `./paleo-dev`（DX5）

repo 根单一可发现入口脚本（薄壳转发，不遮蔽底层工具；逃生舱 = 直接调 cmake/ninja/ctest）：

```
./paleo-dev bootstrap     # 唯一公开入口：preflight → vendor 构建 → 尾跑 selfcheck
./paleo-dev build         # 增量构建（ninja + ccache；主构建 unity 是显式实验档）
./paleo-dev test          # QT_QPA_PLATFORM=offscreen ctest，本地/CI 同一路径
./paleo-dev selfcheck     # §44.1 自检 + map.png（通常由 bootstrap 尾跑，单独跑用于诊断）
./paleo-dev clean-vendor [dep]  # 删 dep 的 stamp+build 子目录；无参 = 清全部 vendor 构建缓存（保留下载源包）
./paleo-dev --help        # 列出全部 verb
```

`paleo-dev` 集中管理易忘环境变量（`QT_QPA_PLATFORM`、vendor prefix、`LD_LIBRARY_PATH`），避免本地/CI 漂移。

### 44.3 Bootstrap 预检与可恢复契约

- `paleo-dev bootstrap` 先做 **preflight**（随路线分档）：通用 = 磁盘空闲（binary ≥15GB / superbuild ≥60GB）、cmake ≥ 最低版本、编译器（Linux GCC14+/Windows MSVC v143 /MD）、ninja、pkg-config、OpenSSL 头文件、Linux 端 X11/GL dev 头、**glibc ≥ 2.41 检查**（binary 路必需）、网络可达性；superbuild 首选路追加 = flex+bison、nasm/python。失败项逐项输出「问题 + 原因 + 修复命令」后退出，不允许进行到一半才爆。
- **断点续跑**：binary 路 = 已下载+校验过的包跳过（URL/SHA256 清单为幂等依据）；superbuild 首选路 = ExternalProject stamp 目录（`ninja` 天然增量）。**不手写 `.done` 标记**。`clean-vendor <dep>` = 删该 dep 的 vendor 子目录（binary）或 stamp+build 子目录（superbuild）即可触发单项重建。
- 构建日志统一落 `vendor/logs/<dep>.log`；失败时 bootstrap 尾部输出最后 40 行 + 完整日志路径。

### 44.4 文档面（Pass 4）

- `README.md`：面向人类工程师的 quickstart —— 2 步（clone → `./paleo-dev bootstrap`，自检由其尾跑），平台矩阵，指向 BUILDING.md。现有 `AGENTS.md`/`CLAUDE.md` 是 agent 路由，不替代人类入口文档。
- `BUILDING.md`：平台 × 版本矩阵、依赖清单（Phase 0 产物回填）、常见失败表（每个失败模式：现象/原因/修复）、如何从干净状态重建。

### 44.5 升级路径（Pass 5）

依赖版本 manifest（每源包/二进制 pin SHA256）是 Phase 0 交付物；补充升级流程契约：bump 依赖版本 = 改 manifest 一处 → `paleo-dev clean-vendor <dep>` → `paleo-dev bootstrap` 重编（自检自动尾跑）。QGIS 4.2.x patch bump 预期无人值守完成；minor/LTR 换线按 §39 重新跑三 spike 做 go/no-go。

### 44.6 迭代环境（Pass 6）

- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` 默认开 → clangd/IDE 即插即用。
- 增量构建预算：改动单个 app 源文件 → 链接完成 ≤ 60s（P0 脊线代码量下）。
- 渲染测试统一 `QT_QPA_PLATFORM=offscreen`（经 `paleo-dev test`），headless CI 无需 X。
- 构建/测试并行度均不超过 8（`cmake --build build -j8`、`ctest -j8`）；
  Windows ctest 串行。独立 worktree 的 vendor 接线见 BUILDING.md。

### 44.7 DX 度量（Pass 8）

- selfcheck 输出各阶段耗时（vendor 编译不计入日常指标）。
- bootstrap 失败时保留完整日志于 `vendor/logs/`；CI 将其作为 artifact 上传。
- 不测遥测 —— 内部单人项目，度量只服务于「本次构建为什么慢/为什么挂」。

### 44.8 DX Journey Map（修正后）

```
STAGE          | DEV DOES                          | STATUS
---------------|-----------------------------------|--------------------------
1. Discover    | clone → README quickstart          | fixed (§44.4)
2. Install     | ./paleo-dev bootstrap（preflight+stamp续编）| fixed (§44.3)
3. Hello World | bootstrap 尾跑 selfcheck → map.png | fixed (§44.1)
4. Real usage  | paleo-dev build/test，增量≤60s      | fixed (§44.2/44.6)
5. Debug       | 失败输出 问题+原因+修复+log 路径     | fixed (§44.3)
6. Upgrade     | manifest 一处 bump → clean-vendor → selfcheck | fixed (§44.5)
```

### 44.9 DX 交付物清单（并入工程任务）

- ET11 (P1) — `paleo-dev bootstrap` + vendor 构建：preflight（含 flex/bison/pkg-config/X11-GL/OpenSSL/磁盘≥60GB）+ ExternalProject stamp 续编 + 失败诊断输出（§44.3）。验证：干净容器跑通。
- ET12 (P1) — `paleo-dev` 入口脚本 + `selfcheck` verb 含 map.png 渲染（§44.1/44.2）；**含 repo 内置最小 GPKG fixture**（不依赖 ET8 golden 工区）。验证：selfcheck 全绿且 PNG 非均匀像素。
- ET13 (P2) — `README.md` + `BUILDING.md`（§44.4）。验证：按文档在无先验机器走通 TTHW。
- ET14 (P2) — 增量构建预算检查：单文件改动增量 ≤60s 记录进 BUILDING.md 实测值（§44.6）。

## 45. 层边界契约（docs/UI_LAYER_PLAN.md 落地，wave/ui-layer-separation）

视图只发信号不干活，功能只编排不画像素，数据只问答不管谁来问。

- **数据层** `src/domain` `src/catalog` `src/io` `src/metadata` `src/services`
  `src/algorithms`：解析、目录、工程持久化、算法。禁 QtWidgets 与 `ui/`。
  视图读侧唯一门 = `services/previewdoc.h`（`PreviewDocService`：catalog
  只读透传 + 静态解析门面 lasAt/wellHeadAt/wellTopsAt/timeDepthAt/
  geoJsonBounds/geoJsonDocument/seismicTieMarker/decodeSectionAsync/
  wellCompositeAt + absolutePathForVersion/relocateVersionSource）。
- **功能层** `src/workflow` `src/linkage` `src/ai`：编排服务与操作。
  `folderimport`（导入编排）/`projectopen`（打开/新建工程）/`registration`
  （临时配准）；禁 QtWidgets 与 `ui/`——联动器改发意图信号由壳订阅
  （threewaylocator/seismicmaplink）。
- **QGIS 封装** `src/qgis`：QtWidgets 豁免（Qgs* 接口所需），仍禁 `ui/`。
  `layoutexport`（PDF/PNG 导出核心，ui/layout 壳委托）。
- **视图** `src/ui/**`：渲染 + 输入 + 意图信号。`io/*` 白名单仅
  `io/lasdoc.h`；`metadata/*` 白名单六头（layermanifest/paleoprojectstore/
  mapversionstore/releasestore/wellsectionstore/faultsetstore，
  单点事实在 `tools/layering_vocab.json`）；`algorithms/*` 全禁。
  `PaleoMainWindow` 是壳：attachWorkflows 按页拆 `attach*Page` +
  attachShellSurfaces（`paleomainwindow_attach.cpp`），attachMapping 三段
  （发布门/导出接线/版本状态机），ribbon 命令组在 `ribbonpanels.cpp`；
  DataPage 分家 = DataListPanel + EntityPanel + 兼容薄壳 `datapage`。
- **组装根** `src/app`：唯一允许 include `ui/` 的非视图目录；
  `src/selfcheck`（测试壳）同豁免。
- **机械执行**：`tools/check_layering.py`（include 归一化 + io/metadata
  白名单 + QtWidgets 词表禁令含 `class Q…;` 前向声明 + 头三行 `// 层：`
  标记硬检查），ctest 项 `layering`/`layering_strict`/`layering_selftest`；
  另有两档 opt-in 诊断（方向 49，默认关、不进 strict 闸、不扰三档输出）：
  `--transitive` 报「功能层直接 include 的 repo qgis 封装头经 include 闭包
  （repo 边 + QGIS 自带头）传递引入 QtWidgets」黄牌链——QGIS 头目录按
  `--qgis-include` > `$QGIS_PREFIX/include/qgis` > vendor 默认序定位；
  `--symbol-audit` 对非视图层 `new`/`std::make_*` 构造 ui Q_OBJECT 类直接
  红（符号级，当前零报告）。ui token 例外清单计数入
  `tools/ui-token-baseline.txt` 收缩门（ctest `ui_tokens_strict`，只降不
  升，方向 49 以 356 条起步——清理例外属 DESIGN 域递延）。
  `tools/layering-baseline.txt` 已归零，strict 下非空或可收缩均失败。
  各模块编为 `paleo_<模块>` 静态库，`paleo_core` 是 INTERFACE 兼容伞。
  已知上限：护栏只挡 include 层，不能识别不带 include 的违规调用
  （`--symbol-audit` 已补上 new/make_* 构造这一子面的抽检）。

## NOT in scope（本次评审决议）

- **License/vendor 合规章节** — 用户确认产品遵循 GPL，vendored QGIS 兼容，无需专章。（D3）
- **多 realization / 不确定性支持** — 推迟至 TODOS.md（P2）。保留要求：`DataAsset` 与 `facies_polygon` schema 预留可空 `realization_id` 字段，避免日后 schema 迁移。（D6）
- **暗色模式** — DESIGN.md 已定 token 结构支持，V1 不交付。（设计评审）
- **简化版编图 composer** — 字段试用后再评估；先嵌完整 QgsLayout 设计器。（D12）
- **庆祝式动画/品牌化视觉** — 工具调性为克制专业；峰终时刻只做"结果可见"处理。（D6）

## 评审决策台账（Decision Ledger）

| ID | 议题 | 结论 | 依据 |
|----|------|------|------|
| D1 | 项目 CLAUDE.md 加 skill 路由 | 采纳 A | 用户选择 |
| D2 | 评审模式 | SELECTIVE EXPANSION | 用户两次以约束作答未选模式，按推荐默认；可随时切换 |
| D2a | 不使用 Python QGIS | 已确认约束 | 用户明确："C++ 的版本" |
| D2b | 所有依赖尽量 vendor | 已确认约束 | 用户明确 |
| D3 | License 合规章节 | 跳过 | 用户："软件就是遵循 GPL" |
| D4 | 自动化测试策略 | 采纳 → §33 | 用户选择 A |
| D5 | 统一 Undo/编辑会话模型 | 采纳 → §34 | 用户选择 A |
| D6 | 多 realization | 推迟 → TODOS.md P2 | 用户选择 B |
| D7 | 修订后计划+CEO摘要 | 批准 | 用户选择 A |
| D8 | 门控可解释性 | 采纳 → §35 | 用户选择 A |
| D9 | Raster→Vector 派生管线 | 采纳 → §36 | 用户选择 A |
| D10 | 按层位懒加载 | 采纳 → §37 | 用户选择 A |
| D11 | 诊断与可观测性 | 采纳 → §38 | 用户选择 A |
| E1 | 仓库为空，无既有代码 | Greenfield 重定位 → §39 | 用户确认 |
| E2 | Embed vs 完整QGIS插件 | 维持 embed + app-only 审计 → §39 | 用户选择 A |
| E3 | Phase 0 go/no-go spikes | 采纳 → §39 | 用户选择 A |
| E4 | QGIS 版本 | 4.x + Qt6，跟踪 4.2 LTR → §39 | 用户选择 A |
| E5 | §36 逐要素简化破拓扑契约 | 改边界图方案 → §36 | 用户选择 A |
| E6 | 护城河算法只有标签 | 最小契约 → §40 | 用户选择 A |
| E7 | AI 推理运行时黑盒 | 契约先定+Phase0 钉运行时 → §40 | 用户选择 A |
| E8 | 7 项正确性规则批量 | 全部采纳 → §41 | 用户选择 A |
| DR2 | 首次运行/空工程体验 | 启动页（最近工程+新建/打开） → §42.1 | 用户选择 B |
| DR3 | 每页面板清单 | 采纳 → §42.2 | 用户选择 A |
| DR4 | 定位器搜索范围 | 域过滤器（井/层位/问题） → §42.3 | 用户选择 A |
| DR5 | 特性级状态表 | 采纳 → §42.4 | 用户选择 A |
| DR6 | 峰终时刻处理 | 克制+结果可见 → §42.5 | 用户选择 A |
| DR7 | 图标策略 | QGIS 集+自定义域集 → §42.6 | 用户选择 A |
| DR8 | 状态 tokens | 采纳 → DESIGN.md | 用户选择 A |
| DR9 | text-muted 对比度 | 调深为 #5D6E80 → DESIGN.md | 用户选择 A |
| DR10 | a11y/窗口规则块 | 采纳 → §42.11–15 | 用户选择 A |
| DR11 | 导入 UX | 模态 QWizard → §42.7 | 用户选择 A |
| DR12 | 编图 composer | 嵌完整 QgsLayout 设计器（app-only 成本注记） → §42.8 | 用户选择 A |
| DR13 | 层位 chip 溢出 | 滚动+溢出弹层 → §42.9 | 用户选择 A |
| DR14 | Undo 可见性 | 会话级撤销+历史面板 → §42.10 | 用户选择 A |
| ER1 | 工程评审单元 | Phase 0 + P0 脊线深审；P1/P2 契约级 | 用户选择 A |
| ER2 | qgis/ 层结构 | 4 服务脊线+具名 stub，非 14 服务齐建 | 用户选择 B |
| ER3 | Phase 0 spike 验收 | 二值判定标准 + init 顺序文档 + 平台矩阵 → §39 | 用户选择 A |
| ER4 | 懒加载↔qgz 机制 | 声明式清单为事实源 → §37 | 用户选择 A |
| ER5 | 写路径 | PaleoProjectStore 串行队列 + gpkg→qgz 次序 → §41.2 | 用户选择 A |
| ER6 | C++ 测试框架 | QTest+CTest → §33 | 用户选择 A |
| ER7 | AI 运行时 | Phase 0 第三 mini-spike → §39 | 用户选择 A |
| ER8 | 按图层任务锁 | per-layer busy map → §35 | 用户选择 A |
| ER9 | NFR 量化 | overviews/打开预算/并发上限/cell size → §41.6 | 用户选择 A |
| ER10 | 跨层位验证 | instantiate-on-demand → §37 | 用户选择 A |
| ER11 | QGIS 4.2 LTR 已发布 | 事实修正 → §39（钉 4.2.x） | 证据：version.qgis.org |
| DX1 | 目标开发者 persona | 内部单人/小团队工程师 → §44 | 用户选择 A |
| DX2 | TTHW 目标 | 1 命令 + ≤2h 无人值守 → §44.1 | 用户选择 B |
| DX3 | Magical moment | selfcheck checklist + map.png → §44.1 | 用户选择 A |
| DX4 | 评审模式 | DX POLISH | 用户选择 B |
| DX5 | 开发入口 | `./paleo-dev` 统一入口 → §44.2 | 用户选择 A |
| DX6 | vendor 策略未评估替代方案 | spike-0 对比评估前置 → §39/ET0 | 用户选择 A（native review F1） |
| DX7 | ET9 审计时点 | P2 → Phase 0，作 go/no-go 成本输入 → §43 | 用户选择 A（native review F2） |

## 外部评审说明

Codex 外部评审因网络故障超时（5 分钟上限，websocket TLS 失败）——**outside coverage 记录为 unavailable，不计入外部模型覆盖**。按工作流启用 native fallback（同 harness 新上下文子代理），其发现已全部经用户裁定（E1–E8）。该 fallback 结果不构成独立模型审查。

## GSTACK REVIEW REPORT

| Review | Trigger | Why | Runs | Status | Findings |
|--------|---------|-----|------|--------|----------|
| CEO Review | `/plan-ceo-review` | Scope & strategy | 1 | clean | SELECTIVE EXPANSION；D1–D11+E1–E8 全裁定；§33–41 采纳；Phase 0 spikes 为 P0 前置门 |
| Outside Review | codex via `/plan-ceo-review` | Independent 2nd opinion | 1 | unavailable | Codex 网络超时；native fallback completed（meta-finding+9 组发现已裁定），不计外部覆盖 |
| Eng Review | `/plan-eng-review` | Architecture & tests (required) | 1 | issues_open→resolved | Phase0+P0 脊线评审；8 发现全部采纳 → §33/35/37/39/41；QGIS 4.2 LTR 事实修正 |
| Design Review | `/plan-design-review` | UI/UX gaps | 1 | clean | 6/10 → 9/10；13 项决策采纳 → §42 + DESIGN.md tokens |
| DX Review | `/plan-devex-review` | Developer experience gaps | 1 | issues_open→resolved | 3/10→7/10；内部构建栈 DX；TTHW=1命令+≤2h；map.png 魔法时刻；paleo-dev 入口；§44+ET0/ET9/ET11–14；native 迟交 9 发现全裁定（DX6/DX7 + 7 项规格修正） |

- **OUTSIDE COVERAGE:** codex（plan phase）= unavailable（TLS/超时）；claude-code（design phase）= skipped；eng-phase = unavailable（native fallback 300s 超时）；dx-phase = unavailable（native fallback 300s 超时；其子代理迟交结果——9 项发现已裁定采纳，但为同 harness 非独立覆盖，不计外部评审）。无完成的外部评审记录。
- **VERDICT:** CEO CLEARED + DESIGN CLEARED（9/10）+ ENG REVIEWED（8 发现全采纳）+ DX REVIEWED（7 决策全采纳，0 未决）。可进入 Phase 0 实现 —— 三 spike 为 P0 前置 go/no-go 门；ET11/12 使 TTHW 目标可验收。

NO UNRESOLVED DECISIONS
