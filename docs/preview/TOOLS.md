# 预览交互工具（P2 D1.2/D3.x/D7 · wave/preview-map-canvas）

工具注册表 `PreviewMapToolManager`（src/qgis/previewmaptools）+ 组合页
`PreviewMapPage` 的交互面。测试证据：`tst_previewmap_tools`（21 函数）+
`tst_previewmap_page`（24 函数）。

## 1. 内建工具（七件，单例互斥）

| id | 名称 | 行为 |
|---|---|---|
| pan | 漫游 | QgsMapToolPan（默认在岗） |
| zoomIn | 框选放大 | QgsMapToolZoom(zoomOut=false) |
| zoomOut | 框选缩小 | QgsMapToolZoom(zoomOut=true) |
| identify | 识别要素 | 点选/拖框（>5px 成框）→ 查询核心 → 结果面板 |
| measureLine | 距离测量 | 左键加点、移动实时读数、双击结束、右键/Esc 清除 |
| measureArea | 面积测量 | 同上（≥3 点成面，周长+面积读数） |
| profile | 层位剖面线 | 拖线（<8px 微拖拒绝）→ 累计多线（D5.4）；右键/Esc 清空 |

**Esc 语义（D1.8）**：任一工具按 Esc → 清自身状态 → 回 pan。
**量测口径**：工程 datum-free 局部网格 → `QgsDistanceArea` 平面测量，
不 `setEllipsoid`。读数 `PreviewMapFormat::length/area`（m/km、m²/km²）。
**节流（D6.5）**：move 读数 ≤30Hz；**press/finish 是离散事件强制发帧**
（节流误杀首帧/加点帧是实测过的真 bug）。

## 2. 视图导航（D3.1–D3.3/D3.10）

- 平移拖拽（pan 工具）+ 键盘方向键平移 20%（D3.1）。
- 框选放大/缩小（zoomIn/zoomOut 工具，D3.2）。
- 全图复位（工具条 + 快捷键 0）、缩放至图层（右键菜单/工具条）。
- 上一视图/下一视图历史栈（D3.3）：`zoomBack/zoomForward`，去重、上限 100。
- 滚轮缩放以光标为中心（D3.10）：QgsMapCanvas 原生行为。

## 3. 读数与复制（D3.6/D3.8）

- **坐标读数条**：鼠标处工程坐标 `X 1234.56  Y 789.01`（JetBrains Mono，
  tnum）；跟踪节流 ≤30Hz。
- **比例尺读数**：`1:50,000`（qRound64——无层画布尺度 ~1e10 溢出 int 是
  实测过的 assert）。
- **复制坐标**：工具条/右键菜单 → 剪贴板 `x, y`。
- **复制画布截图**：工具条/右键菜单 → 剪贴板 pixmap。

## 4. 书签（D3.7）

命名视图范围：保存当前视图（QInputDialog 命名）→ 菜单跳转（精确复位
`setViewExtent`，不另加边距）/ 删除。会话级记忆
（`PreviewStateMemory::bookmarks`，按 assetKey——标签重建后书签仍在）。

## 5. 鹰眼（D3.9）

右下角 200×150 导航小图：全图 + 主视口框（QgsRubberBand）；点击/拖动把
主画布中心带过去；滚轮/右键被事件过滤器吃掉（不与主画布抢交互）；
工具条开关（显式旗标语义——页未显示时 isVisible 恒假不承载开关语义）。

## 6. identify（D2.5/D7.x）

- 查询核心 `PreviewIdentifyCore`：矢量按层缓存 QgsSpatialIndex（D6.4，
  层析构自动清）；栅格最近邻 + 双线性插值（D7.4，
  `QgsRasterDataProvider::sample` + 2×2 邻域加权）。
- 结果面板：要素列表（层名 · #fid / 栅格取值）→ 属性卡（字段/值表，
  右键复制行 D7.1）；「定位闪烁」走 `QgsMapCanvas::flashFeatureIds`
  （D7.2）；导出 CSV（D7.5）；空命中「未命中任何要素」（D7.6）。
- **全表模式（D7.3）**：`PreviewAttributeTableDialog`——分页（100/页）、
  点列头排序、列过滤（选列+包含文本，表达式下推 provider）。

## 7. 触摸板手势（D3.12）

QgsMapCanvas 原生 pinch zoom 在合成触屏/平台支持时可用（QEvent::NativeGesture
处理在 QGIS 侧）；offscreen 测试环境不可测，未加自定义手势代码——
**递延说明**：若实测平台（Wayland 触控板）不触发原生手势，后续在
PreviewMapCanvas 事件过滤器补 NativeGesture 处理。
