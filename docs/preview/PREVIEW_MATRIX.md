# 数据预览资产矩阵（Phase 0 侦察 · P2 数据预览全面地图化）

日期：2026-09-29。分支：`wave/preview-map-canvas`（worktree `../pw-preview-maps`）。
基线：master `93b4195`。侦察对象：`src/ui/datapreview/datapreviewtabs.cpp`（3939 行）
全部预览分支 + `src/ui/decorations/` 装饰件盘点。

## 1. 资产类型 × 当前预览形态 × 缺口

| 资产类型 | 当前形态（`buildContent` 分支） | 可地图化 | 缺口（本轮补齐的 D 项） |
|---|---|---|---|
| `well_log`（LAS） | CurvePanel 多曲线叠合 + ResFormStar 综合柱状图（双视图切换） | 否（一维曲线） | 无画布缺口；不纳入地图化 |
| `well_head`（井位表） | 多井下拉 → 单井信息卡（X/Y/KB/TD/BottomX/Y/WellType） | **是** | D2.6/D2.8：井位打点地图（全井 + 高亮当前井 + 名称标注开关） |
| `well_stratification`（DC.dat 分层表） | 多井下拉 → 分层表（层名/MD/TVD/X/Y/Time） | **是** | D2.8：行有坐标（X/Y）时井位落图打点 |
| `time_depth`（时深表） | CurvePanel 单曲线 TIME–TVD | 否（一维曲线） | 无画布缺口 |
| `horizon`（层位散点） | 元数据卡 + 派生栅格就地 `QgsMapCanvas` 伪彩预览（私有层不进 QgsProject） | **是** | D1.x 框架化；D2.1 等值线 overlay；D2.2 拉伸/色带控制；D2.3 直方图小图；D2.9 版本切换；D5.1–5.8 剖面/统计/极值/直方图 |
| `seismic`（SEG-Y） | 二维测线剖面 + 三维立体 + 水平时间切片（转码双通道） | （P5 领地） | **本分支函数体不动**（并行任务持有）；仅其外围（quickInfo/modeTabs 容器之外）不受影响 |
| `image_reference`（PNG/JPG） | QLabel 图片查看器 + 「未配准，不加入地图」 | **是（有配准时）** | D2.7：world file/四参数配准 → 栅格上图；未配准 → 查看器 + 「去配准」引导 |
| `document`（PDF/office） | QPdfView 渲染（office 经 soffice 转 PDF） | 否 | 无画布缺口 |
| `geojson` / boundary.geojson | `QgsVectorLayer` + 分类渲染（相字段）+ 画布 + 属性表（双视图） | **是** | D2.4 图例侧栏；D2.5 identify；D4.x TOC/符号快调；D3.x 工具 |
| 辅助/参考/未知 | 文件名 + 类型 + 系统打开 + 「未配准」；XML 走综合柱状图 | 部分 | D2.12 统一「不支持预览」态 + 可支持类型列表 |
| （测区全景）`openSurveyArea` | 自建 `QgsMapCanvas` + 工区边界橡皮带 + 装饰件 + 工具条 | **是** | D1.x 框架化（保 objectName 兼容面） |

多版本/组图/大图横切缺口：D2.9 RAW/DERIVED 版本下拉切换、D2.10 同目录组图叠加、
D2.11 >50MB 栅格金字塔提示降级。

## 2. decorations/ 可复用清单

`src/ui/decorations/paleodecorations.{h,cpp}`（唯一文件）已有：

| 装饰件 | 类 | 状态 |
|---|---|---|
| 比例尺 | `PaleoScaleBarDecoration` | 可复用；已挂 horizon/geojson/survey 画布 |
| 指北针 | `PaleoNorthArrowDecoration` | 可复用 |
| 坐标网格 | `PaleoGridDecoration` | 可复用（survey 工具条已给开关） |
| 临时配准水印 | `PaleoWatermarkDecoration` | 可复用 |
| 相图图例 | `PaleoFaciesLegendDecoration` | 可复用（facies.geojson 预览） |
| 管理器 | `PaleoDecorationManager` | 挂 `renderComplete` 统一绘制；本轮扩「注册式按名开关 + 暗色适配」（D1.5/D1.10） |

QGIS 4.x 边界（文件头已记）：app 层 QgsDecorationItem 不可达，唯一装饰面是
core 的 `QgsMapDecoration` + `renderComplete(QPainter*)` 钩子——自研件沿此契约。

## 3. 已知地雷（写进实现，防复发）

1. **`QgsColorRampShader` 空 itemList**：Continuous 模式也必须先
   `classifyColorRamp()`，否则 `shade()` 全返回 false、像元全透明——画布一片
   白（datapreviewtabs.cpp:2500 已有注释）。本轮统一走
   `previewrasteranalysis.h` 的 `applyPseudoColorRenderer()` 安全出口。
2. **预览层一律不注册 QgsProject**：私有 `QgsRasterLayer`/`QgsVectorLayer`
   父子挂在预览页宿主上，画布只引用裸指针（horizon 分支既有约定，
   datapreviewtabs.cpp:2517 注释）。预览绝不污染主图图层树/实例表。
3. **工程 CRS 是 datum-free 局部网格**：
   `DataCatalog::localGridCrsWkt()`（qgiscanvascontroller.cpp:91 同源）——
   预览画布 destinationCrs 钉同一 WKT；栅格层 CRS 与画布 CRS 不一致时
   先转（或如实报 CRS 异常态，D1.7），绝不允许 QGIS 隐式 EPSG:4326。
4. **测量/距离**：局部网格无椭球——`QgsDistanceArea` 平面米制即可，
   不 `setEllipsoid`（设了反而走球面改正）。
5. **异步渲染悬挂**：预览页关闭时必须 `QgsMapCanvas::stopRendering()` +
   `unsetMapTool`（D1.9）；renderComplete 到达晚于页销毁是真实竞态
   （QPointer/父子树托管双保险）。
6. **Qt6 functor + UniqueConnection 静默拒绝**（mapping-editing wave 坑）：
   重连服务信号时用显式 disconnect 或 Qt::UniqueConnection 配旧式指针。
7. **objectName 兼容面**（tst_datapreview 断言，重构不得断）：
   `horizonMapCanvas`/`faciesMapCanvas`/`surveyMapCanvas`（QgsMapCanvas）、
   `faciesDecorManager`/`surveyAreaDecorManager`（装饰管理器，须是画布子对象）、
   `btnSurvey*`/`btnToggle*`/`btnSwitchToMainCanvas`/`btnViewFacies*`/`faciesViewStack`/
   `geoJsonFeatureTable`/`showOnMapBtn`/`wellCombo`/`topsTable`/`curveCombo` 等。
8. **seismic 分支函数体禁改**（并行 P5 持有）：`buildContent` 内
   `asset.type=="seismic"` 分支整体不动；其前的 survey 几何、tie marker
   读取保持在分支外只读不改语义。

## 4. 本轮架构落点

- `src/qgis/`（仅新增，`// 层：QGIS 封装`）：
  `previewmapcanvas.{h,cpp}`（D1.1 画布封装）、`previewmaptools.{h,cpp}`（D1.2
  工具注册表 + 量测/identify/剖面工具）、`previewrasteranalysis.{h,cpp}`（D2.2/
  D5.5–5.8 栅格统计/拉伸/直方图/剖面采样/极值/色带预设）、
  `previewidentify.{h,cpp}`（D7/D6.4 空间索引 identify 核心）。
- `src/ui/datapreview/`（视图）：`previewmappage.{h,cpp}`（D1.3/1.4/1.6/1.7/1.8
  + D3.x + D4.1 组合页）、`previewtocpanel.{h,cpp}`（D4.x）、
  `previewidentifypanel.{h,cpp}`（D7.x 面板）、`previewprofilepanel.{h,cpp}`
  （D5.1–5.4 剖面图）、`previewhistogramwidget.{h,cpp}`（D2.3/D5.8）、
  `previewmapstates.{h,cpp}`（D1.7/D2.11/D2.12 状态页与组图/版本辅助）。
- `src/ui/decorations/`：`PaleoDecorationManager` 扩按名注册开关 + 暗色 token
  适配（D1.5/D1.10）。
