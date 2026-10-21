# 公开网络底图预设（2026-10-21）

主地图入口为「文件 → 网络底图预设」；数据预览下拉复用同一清单。
常用选项优先列 OSM 街道与 Esri 影像、街道、地形、浅灰、深灰、晕渲；
NASA 图层作为补充来源排在后面。Esri 是公开网络服务，不标为开源数据。

Esri 的服务标识分别为 `World_Imagery`、`World_Street_Map`、`World_Topo_Map`、
`Canvas/World_Light_Gray_Base`、`Canvas/World_Dark_Gray_Base`、`World_Shaded_Relief`。
均读取官方 MapServer 元数据核验；晕渲最高 13 级，其余预设出于兼容与请求
规模考虑使用到 19 级（不把服务目录声明的 23 级直接带入 GDAL 大尺寸栅格）。

访问验证：OSM 和六个 Esri 服务均返回 HTTP 200 及有效 PNG/JPEG 签名。
本机请求关闭了显式 HTTP 代理，但出口探测仍为 US；这些结果仅证明该线路
可达，不能标注为已通过中国大陆直连测试。大陆可用性需在实际运营商线路
验证，代码和界面没有写入“大陆保证可用”的承诺。
另尝试 ITDOG 大陆运营商多节点 HTTP 检测，提交 Esri 影像瓦片 URL 后要求
验证码，未继续，因此不采用页面默认示例结果作为可达性证据。

Esri 官方链接：
[访问域名与 CDN](https://support.esri.com/zh-cn/knowledge-base/faq-what-are-the-required-firewall-domain-exceptions-to-000012925)、
[服务使用条款](https://doc.arcgis.com/zh-cn/arcgis-online/reference/access-use-constraints.htm)、
[传统栅格服务生命周期](https://blog.esrij.com/archives/69487)。街道、地形、灰底
属于传统服务，官方计划在 2029 年底退出；晕渲计划在 2028 年 3 月退出，
后续应迁移到提供者支持的替代服务。

新增以下无需 API key 的 NASA GIBS 图层，均为 EPSG:3857 XYZ 形式：

| 预设 | 稳定 ID | 官方图层标识 | 原生最高级 |
|---|---|---|---|
| 全球真彩影像（概览） | nasa_blue_marble | BlueMarble_NextGeneration | 8 |
| 全球地形与海底地貌（概览） | nasa_relief_bathymetry | BlueMarble_ShadedRelief_Bathymetry | 8 |
| ASTER 彩色地形 | nasa_aster_color | ASTER_GDEM_Color_Shaded_Relief | 12 |
| ASTER 灰度地形 | nasa_aster_gray | ASTER_GDEM_Greyscale_Shaded_Relief | 12 |

全球概览不是实时或高分辨率影像；ASTER 覆盖范围外可无数据。现有 OSM 街道、
OpenTopoMap 地形及 Esri 影像稳定 ID 保留，Esri 明确标为受商业条款约束。
名称、提示、署名与条款链接来自清单；已有版面导出继续通过
`paleoBasemapAttribution` 收集来源。未默认叠加所有来源或自动改动当前工程。

## 来源与取舍

- [NASA GIBS 接入规则](https://nasa-gibs.github.io/gibs-api-docs/access-basics/)：
  使用官方 WMTS ResourceURL 的 XYZ 映射，行列顺序为 `{z}/{y}/{x}`。
- [官方 EPSG:3857 服务目录](https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml)：
  核验图层、GoogleMapsCompatible_Level8/12、图像格式及静态 URL。
- [NASA 使用说明与致谢](https://nasa-gibs.github.io/gibs-api-docs/#data-use-guidance-and-acknowledgements)：
  来源标注保留 NASA GIBS / ESDIS；ASTER 另标 METI / NASA。
- [OSM tile policy](https://operations.osmfoundation.org/policies/tiles/)：
  使用 HTTPS、应用标识、来源署名与缓存；不预取或批量下载。
- [CARTO 官方说明](https://github.com/CartoDB/basemap-styles/blob/master/README.md)：
  当前要求 API key，因此未作为免配置预设加入。

## 实现与验证

主图预设由 QGIS 封装负责建层、去重、置底和底图互斥显示，不改变地质层显隐。
图层及署名通过原生 .qgz 持久化。QGIS WMS provider 优先；本机裁剪版缺少该
插件，回退到 QGIS GDAL provider 的原生 WMS/TMS 驱动。XML 使用 writer 转义，
配置顶端行原点、投影、原生级数、应用 User-Agent 和 7 天缓存，不自行请求或
拼接瓦片。GDAL 的实现依据见 [WMS 驱动文档](https://gdal.org/en/stable/drivers/raster/wms.html)。

新增 `tst_onlinebasemap` 并经 `add_paleo_test(... LIBS paleo_qgis)` 注册。
本机独立 worktree 构建：7 pass、0 fail、0 skip（含 init/cleanup），覆盖预设
元数据、URI 编码和请求标识、本地瓦片像素读取、工程重开、绘制顺序、去重、
地质层保持可见及工程坐标门控。测试不依赖公网。

另以应用标识逐项核验四个 NASA 全球 0 级瓦片：HTTP 200、image/jpeg、JPEG
签名有效。独立联网 smoke 程序还通过 QGIS GDAL provider 对四个预设分别
读取全球 0 级瓦片，四项均 `rasterRead=1`，退出 0，未做批量预取。
OSM 与六个 Esri 预设也逐项经同一 QGIS GDAL provider 读取，七项均
`rasterRead=1`，退出 0；这些联网读取同样仅代表当前境外出口线路。
主窗菜单与预览工具条通过 MSVC /Zs；layering、Qgs 审计、QGIS 散布、
文件规模、UI 规模、不变量、UI token 和 i18n 闸门通过，无基线变更。
尚未替换用户本机可执行文件或完成全量应用链接与桌面交互验收。
