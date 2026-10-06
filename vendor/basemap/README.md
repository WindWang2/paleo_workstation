# vendor/basemap — 离线底图瓦片（MBTiles）

粗略底图的离线瓦片缓存：从 Esri 公开瓦片服务按范围/层级抓取 XYZ 瓦片，
打包成 MBTiles（单文件、QGIS/GDAL 原生离线读取）。无网络环境也能出图。

## 产物（不入库，.gitignore 已忽略）

| 文件 | 内容 | 覆盖 | 层级 |
|------|------|------|------|
| `basemap_topo.mbtiles` | World_Topo_Map（地形+道路+地名，JPEG） | 中国 + 鄂尔多斯 demo 区 | z2-8（中国）/ z9-11（demo 区 104-112°E, 32-40°N） |
| `basemap_hillshade.mbtiles` | World_Hillshade（纯地形晕渲，JPEG） | 中国 | z2-7 |

## 复现 / 增量补抓

```sh
python3 vendor/basemap/fetch-tiles.py --dry-run   # 只看瓦片计划
python3 vendor/basemap/fetch-tiles.py             # 抓取（断点续传）+ 打包
```

改脚本顶部 `JOBS` 即可新增范围/层级——已下载瓦片落在 `tiles/` 暂存树，
重跑只补缺失部分，然后重新打包。纯标准库，无第三方依赖。

## QGIS 接线（组装根/封装层）

```cpp
// 瓦片 MBTiles 走 wms provider 的 type=mbtiles URI；url 需 URL 编码
const QString uri = QStringLiteral(
    "type=mbtiles&url=file%3A%2F%2F%2Fvendor%2Fbasemap%2Fbasemap_topo.mbtiles");
auto *basemap = new QgsRasterLayer(uri, QStringLiteral("底图 Topo"), QStringLiteral("wms"));
```

- 图层 CRS 为 EPSG:3857（球面墨卡托），画布 CRS 不同会自动 OTF 重投影；
  局部工程网格工区需先有地理配准锚点（见 demo 工区 7 点相似变换）。
- 放到图层树最底（organizer 的无声明层路径，不参与摆放）。
- **署名义务**：metadata 里带 Esri 署名串，画布角标需随图显示
  （Topo: "Sources: Esri, HERE, Garmin, USGS, NGA, EPA, USDA, NPS"）。

## 合规边界

Esri 服务免密钥可用，条款允许应用内使用与有限缓存；本目录只做
「按需范围的离线缓存」，不重新分发原始瓦片、不做全量镜像。
若未来换天地图（中国更细、CGCS2000 无偏移），只需在 `SERVICES` 加
URL 模板 + `tk` 密钥（放 QSettings，勿硬编码）。
