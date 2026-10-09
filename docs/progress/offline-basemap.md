# offline-basemap — 离线底图链路（实战系补账立账）

本文为补账文档：3e8988cd「离线底图叠加与工程真实坐标转换」+
10cdb8cb（vendor/basemap/fetch-tiles.py）落地了完整离线底图链路，
但 progress 主文档缺失（vendor/basemap/README.md 只有合规与复现节，
源管理/缓存策略/坐标系语义散记）。行号为 origin/master `f31ee461` 口径。

## What（交付面）

| 面 | 内容 |
|----|------|
| 抓取脚本 | vendor/basemap/fetch-tiles.py（179 行，纯标准库）：Esri 公开 XYZ 瓦片按 JOBS 计划表抓取 → tiles/ 暂存树（断点续传，已存在即跳过）→ 合并打包 MBTiles（TMS y 翻转） |
| 产物 | basemap_topo.mbtiles（World_Topo_Map，中国 z2-8 + 鄂尔多斯 demo 区 z9-11）/ basemap_hillshade.mbtiles（World_Hillshade，中国 z2-7），.gitignore 不入库（fetch-tiles.py:34-56 SERVICES/JOBS 定义） |
| 加载 | `mapreference::offlineBasemap(path, title, parent)`（projectmapreference.cpp:163-173）：MBTiles 走 wms provider `type=mbtiles&url=<URL 编码>` URI（:157-161）；图层钉 `paleoBasemap`/`paleoBasemapPath`/`paleoBasemapAttribution` 自定义属性 |
| 署名 | attribution 从 MBTiles metadata 表读（:33-50），`mapreference::attribution` 聚合画布上全部底图层署名（:175-183）——Esri 条款的随图显示义务 |
| 配置持久化 | project.paleo map 节（mapCrs/basemapTopo/basemapHillshade/basemapEnabled）；`.qgz` 自定义属性 paleo/mapConfiguration 同形状双持久化（qgisprojectservice_map.cpp:15-28） |
| 编辑入口 | 「文件 → 工程坐标与底图…」→ `updateMapConfiguration`（qgisprojectservice_map.cpp:38-80）：独立 QgsProject 试 configure（坏参数不动正在显示的工程）、底图启用前必须有有效配准、底图可读性预检、控制点残差重算 |

## 口径（语义决策）

1. **坐标系链**：底图 CRS EPSG:3857（球面墨卡托），工程局部 ENGCRS 网格
   经配准管线（见 project-file-mapreference.md）OTF 显示到 mapCrs 画布；
   **无配准锚点则底图不可启用**（qgisprojectservice_map.cpp:56-57
   「启用底图前请设置有效的工程坐标配准」）——底图不反向驱动配准。
2. **瓦片校验**：抓取时 magic `ff d8`（JPEG）硬校验，非 JPEG 拒收
   （fetch-tiles.py fetch_one）；metadata format=jpeg 如实记录——
   GDAL/QGIS 读取格式不靠猜。
3. **缓存策略**：三层——vendor tiles/ 暂存树（重跑只补缺失，增量友好）、
   MBTiles 单文件产物（QGIS/GDAL 原生离线读）、`paleoBasemapPath` 属性
   记绝对路径（图层重建免二次解析，datapreviewtabs.cpp:476 按属性重建）。
4. **放置语义**：底图层放图层树最底，走 organizer 无声明层路径不参与
   摆放（vendor/basemap/README.md「QGIS 接线」节）；preview 画布
   fullExtent/zoomToLayer 排除底图层（3e8988cd 提交说明）。
5. **合规边界**（README「合规边界」节）：免密钥 Esri 服务按需范围离线
   缓存，不重新分发原始瓦片、不做全量镜像；换天地图只需 SERVICES 加
   URL 模板 + QSettings 密钥（勿硬编码）。

## 证据

- 脚本可复现性：`--dry-run` 只看瓦片计划；JOBS 表驱动（新增区域/层级
  只补增量）；UA 自报 `paleo-workstation-basemap-fetch/1.0`
  （fetch-tiles.py:27-31）。
- 加载侧测试锚：projectmapreference 往返校验测试（本方向补，
  tst_projectsvc）覆盖 configure 注册链；MBTiles 文件本身无合成夹具
  （递延，见下）。
- 设计文档：PROJECT_FILE_DESIGN.md map 节（:122-137）、
  PROJECT_AREA_PLAN.md:8-13（配准后坐标系语义注记）。

## 对账与递延

- 与工程配准（projectmapreference）：**依赖**——底图是配准管线的最大
  消费者；配准失败降级为无底图纯局部网格，不阻塞工程打开。
- 与方向 12 时深：**正交**——底图纯平面显示层，不参与 TWT/深度域。
- 递延（记 TODOS）：CI 无底图资产——离线底图渲染自动化测试需要合成
  单瓦片 MBTiles 夹具（gdal python 或 sqlite3 手写 metadata+tile 表），
  本方向不补；hillshade/topo 双层叠加顺序的视觉验证走人工
  （l10n-shots 同款截图流程可复用）。
