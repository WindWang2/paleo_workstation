# 预览画布框架（P2 D1.x · wave/preview-map-canvas）

日期：2026-09-29。实现落点与契约速查。侦察矩阵见 PREVIEW_MATRIX.md，
资产覆盖见 ASSET_COVERAGE.md，工具见 TOOLS.md，性能见 PERFORMANCE.md。

## 1. 组件总览

```
src/qgis/（// 层：QGIS 封装，仅新增）
  previewmapcanvas.{h,cpp}    D1.1 画布封装：私有层容器/CRS/历史栈/渲染状态/渐进 overlay
  previewmaptools.{h,cpp}     D1.2 工具注册表 + 量测/identify/剖面工具（QgsMapTool 族）
  previewrasteranalysis.{h,cpp} D2.2/D5.5–5.8 栅格读侧分析（统计/直方图/拉伸/剖面/极值/色带）
  previewidentify.{h,cpp}     D7/D6.4 identify 查询核心（矢量空间索引缓存 + 栅格双线性）
  previewrendercache.{h,cpp}  D6.2/D6.6 渲染结果缓存（内存 LRU 16 + QTemporaryDir PNG）

src/ui/datapreview/（// 层：视图）
  previewmappage.{h,cpp}      组合页：工具条/画布/侧栏/状态条/鹰眼/书签/错误态/缓存接线
  previewtocpanel.{h,cpp}     D4.x 迷你 TOC + 符号快调 + 图例（D2.4）
  previewidentifypanel.{h,cpp} D7.x 识别结果面板 + 全表模式（分页/排序/列过滤）
  previewprofilepanel.{h,cpp} D5.1–5.4 剖面图（双轴/悬停读数/多线/导出）
  previewhistogramwidget.{h,cpp} D2.3/D5.8 直方图（分箱/对数/拉伸界着色）
  previewmapstates.{h,cpp}    D1.7/D2.11/D2.12 状态页 + world file 探测 + 会话记忆
```

## 2. PreviewMapCanvas 契约（D1.1）

- **私有层容器**：`addLayer/insertLayer/removeLayer/moveLayer` 记账层序（index 0 =
  渲染最上）；可见性/透明度/混合是本类状态，不可见层不进 `setLayers`。
  **层一律不注册 QgsProject**（预览不污染主图实例表——既有约定）。
- **CRS**：缺省钉 `DataCatalog::localGridCrsWkt()`（datum-free 局部工程网格，
  米）；经纬度 GeoJSON 等层自带 CRS 的场景用 `setOverrideCrs` 让画布跟随层。
  绝不允许 QGIS 隐式 EPSG:4326。
- **视图历史栈**（D3.3）：程序式缩放与交互平移都入栈（去重相邻同范围，
  上限 100 步）；`zoomBack/zoomForward`；书签跳转走 `setViewExtent`（精确
  复位，不加边距）。
- **渲染状态**（D1.6）：`renderStarting/mapCanvasRefreshed` →
  `renderStarted/renderCompleted(ms, layerCount, elementCount)`；图元数 =
  矢量要素数 + 栅格像元数（`estimateElements`）。
- **渐进渲染**（D6.1/D6.2）：`renderSnapshot(maxPx)` 低清整图（
  QgsMapRendererCustomPainterJob 同步渲）；`showPreviewOverlay(img)` 上屏
  覆盖层，`renderCompleted` 自动让位。
- **销毁安全**（D1.9）：析构 `stopRendering()`（无悬挂 job）。**不调
  `unsetMapTool`**——它构造 QCursor，进程收尾（平台拆除后）即 qFatal。
- **键盘**（D1.8）：`+/-` 以视口中心缩放、`0` 全图复位、方向键平移 20%。
- **已知边界**：`fullExtent()` 不做跨 CRS 变换——层必须与画布同 CRS
  （分支构造层时保证）；单点/共线层（退化范围）自动扩 10 m 防零视口。

## 3. PreviewMapPage 组合（D1.3–D1.10）

```
┌ previewMapToolBarRow ─────────────────────────────────────┐
│ [QAction 工具条: 漫游/框选放大/框选缩小/识别/距离/面积/剖面/  │
│  全图/上一视图/下一视图/复制坐标/复制截图]  [扩展条: 书签▾  │
│  装饰▾ 同目录叠加…]                                        │
├──────────────────────────────────────────┬───────────────┤
│ previewMapStack                          │ previewSideTabs│
│  ├ PreviewMapCanvas（含鹰眼右下角）       │  图层 | 识别 | │
│  └ previewMapErrorPage（D1.7 错误态）     │  剖面          │
├──────────────────────────────────────────┴───────────────┤
│ previewAnalysisTabs（统计/直方图/等值线/极值——分支挂载）    │
├──────────────────────────────────────────────────────────┤
│ previewMapStatusBar：渲染状态 · 量测读数 ‖ 比例尺 · 坐标   │
└──────────────────────────────────────────────────────────┘
```

- **D1.3 工具显隐按内容类型**：`setToolVisible(id, on)`（矢量页关剖面
  `setProfileEnabled(false)`；隐藏激活中工具自动回 pan）。
- **D1.4 右键菜单**：缩放至图层（子菜单）/复制坐标/复制截图/存书签。
- **D1.5 装饰件**：`setDecorationEnabled("scaleBar|northArrow|grid")` 注册式
  开关，挂 `PaleoDecorationManager`（renderComplete 钩子）。
- **D1.7 错误态**：`setError(title, detail)` 换页到原因页（含重试回调出口）。
- **D1.10 主题**：装饰件色走 `PaleoDecorationTheme::setDark()`（DESIGN dark
  token）；画布本体保持纸面白底（数据符号不随暗色——wellcomposite 同口径）。
- **工具条实现注意**：弹出按钮（书签/装饰/同目录叠加）**不用
  `QToolBar::addWidget`**——QWidgetAction 在 ~QToolBar 子链里对已删按钮
  releaseWidget（实测 SIGSEGV）；统一挂 `previewMapToolBarExt` 扩展条。
- **objectName 兼容面**（既有测试断言，勿改）：`horizonMapCanvas`/
  `faciesMapCanvas`/`surveyMapCanvas`/`imageMapCanvas`/`wellHeadMapCanvas`/
  `topsMapCanvas`（QgsMapCanvas），`*DecorManager`（画布子对象）等。

## 4. 地雷登记（实现时踩过、已钉）

| 地雷 | 症状 | 防线 |
|---|---|---|
| QgsColorRampShader 空 itemList | Continuous 不 classify → 画布全白 | 统一走 `applyPseudoColorRenderer` |
| QgsMapTool 析构碰橡皮带 | scene 先死 → hide() 悬空 SIGSEGV | 工具析构一律不碰 band |
| 画布析构调 unsetMapTool | QCursor 构造在平台拆除后 → qFatal | 析构只 stopRendering |
| QToolBar::addWidget | QWidgetAction 销毁序悬空 SIGSEGV | 扩展条直挂按钮 |
| QGIS4 `createSimple` 返回 unique_ptr | 隐式转 QgsSymbol* 编译错 | `.release()` |
| 画布事件是 QgsMapMouseEvent | QMouseEvent 签名 override 不匹配 | 五参 ctor 合成 |
| 隐藏画布 resize 延迟 | mapToPixel 用陈旧尺寸 | show + qWaitForWindowExposed |
| qRound(scale) 溢出 int | 无层画布尺度 ~1e10 → assert | qRound64 |
