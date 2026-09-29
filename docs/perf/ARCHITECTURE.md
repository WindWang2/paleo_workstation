# IO/服务层性能架构（wave/io-perf-cache P4）

> 范围：`src/io` / `src/catalog` / `src/services`（除 seismictaskservice）/ `src/metadata`
> 的数据层性能与缓存体系。零 UI；分层纪律见 `docs/UI_LAYER_PLAN.md`。

## 1. 解析器清单与调用方

| 解析器 | 格式 | 读取方式（改前） | 缓存（改前） | 主要调用方 |
|---|---|---|---|---|
| `LasParser::parse` | LAS 2.0 | QTextStream 逐行（语义全量） | 无 | previewdoc.lasAt / requestLas、dataimportservice、wellpredictionreview、ingestplan |
| `LasParser::parseHeader` | LAS 头段 | 头部截断读（T1 已解阻） | 无 | previewdoc.lasHeaderAt |
| `LasParser::parseDoc`（新） | LAS 全量快路径 | 一次性字节级 | **LasCache 两级**（D1.1） | LasCache / lasAt / requestLas |
| `LasParser::parseRange/parseDepthRange`（新） | LAS 区间流式 | 8MB 块 | 无（按需读段） | 大文件浏览（D1.4/D1.8） |
| `wellfileparsers` | 井口/分层/时深文本 | readAll + fromUtf8 | 无 | dataimportservice、ingestplan、projectdata、previewdoc |
| `segyreader.open` | SEG-Y 道头索引 | 顺序 seek 逐道 | 仅会话内（previewdoc m_segyReaders） | previewdoc.requestSection、dataimportservice |
| `segyreader.openCached`（新） | 同上 | 命中免扫 / 并行 ≤4 分片 | **SegyIndexStore 磁盘**（D2.1） | previewdoc.requestSection |
| `geojsonaffine` | GeoJSON | QJsonDocument 全量 DOM | 无 | registration、previewdoc、entitypanel |
| `Streaming::geoJsonBoundsStreaming`（新） | GeoJSON 坐标 | 256KB 增量扫描 | 无 | 包围盒类查询（D7.6） |
| `horizonbinner` | 层位散点→GeoTIFF | readAll | 无 | dataimportservice |
| `wellcompositexml`（P1 领地，未动） | 井综合 XML | QXmlStreamReader | 无 | wellcomposite 面板 |
| `constraintstore` | GeoPackage 约束 | OGR 逐次全量 open | 无 | constraint workflow |
| `DataCatalog` | catalog.json | 全量 QJsonDocument | 无索引（线性扫） | 27 处（io/services/qgis/ui/workflow） |

## 2. 缓存点拓扑（改后）

```
CacheBudgetManager（D6，进程级，默认 512MB / QSettings cache/budgetMiB）
 ├── las-docs          LasCache 内存 LRU（64MB 默认）        [D1.1]
 ├── pyramid-tiles     RasterPyramidService 瓦片 LRU（128MB） [D3.4]
 └── （未来任何实现 EvictableCache 的缓存自动入册）

磁盘层（<project>/artifacts/index/）
 ├── las/<sha256(fp)>.plc     LasCache 磁盘层（版本化 + zstd）  [D1.1/D1.2]
 ├── segy/segyidx_<sha256>.psx  SEG-Y 道头索引（身份 + zstd）   [D2.1-D2.4]
 └── sha.json                 ShaCache 摘要表                   [D7.7]

金字塔存储（<cacheRoot>/<sha256(src)>/z<N>.bin + meta.json）      [D3.1-D3.7]
catalog 邻接索引（CatalogIndex，内存派生结构，随 mutator 增量维护）[D5.1-D5.4]
```

接线点：`DataImportService::setProjectDir` 统一落缓存根（工程私有、随工程
迁移）；损坏一律「删除自愈 + 重建」（D1.2/D2.2/D2.3），绝不让坏数据当命中。

## 3. 任务服务拓扑（D4）

- `PaleoTaskService`：**专用 QThreadPool ≤4 工作线程**（D4.5，不再用
  globalInstance 默认无上限）；`start(title, work, layerId, Priority)` 三档
  优先级（D4.6，QThreadPool 数值大者先出队——预取 Low 不挡用户点击 High）。
- 进度两面：`reportBytes`（字节 + 10s 窗口线性 ETA）与 `reportStage`
  （阶段词表 scan/parse/index/decode/hash/build/publish + 百分比，D4.3）。
- 取消：协作式 `cancelRequested()`；`openCached` 取消时落 checkpoint（D2.8），
  `m_lastScanPartial` 保留部分索引。
- 请求合并（D4.7）：`InflightCoalescer`（io/inflight.h）——LasCache 以
  「规范化路径+mtime+size」指纹为键，同文件并发 load 只解析一份，等待方共享
  future 拷贝（QVector COW，浅拷）。
- 结果投递（D4.8）：worker 任意线程 → `QMetaObject::invokeMethod(obj, fn,
  QueuedConnection)` 统一排队回对象线程（PaleoTask.applyFinish/applyProgress/
  applyStage 与 previewdoc 的世代号 apply 闭包均走此口径）。

### D4.1 同步解析入口盘点（>5MB 文件在 UI 线程同步解析的路径）

| 入口 | 现状 |
|---|---|
| `PreviewDocService::lasAt`（同步） | 改后走 LasCache（命中亚毫秒；冷解析字节级提速 8×）；重路径仍建议经 requestLas 异步 |
| `PreviewDocService::wellCompositeAt`（XML） | 同步保留——井综合 XML 均 <1MB（P1 领地，未动） |
| `PreviewDocService::geoJsonDocumentAt` | 同步保留（DOM 面向属性编辑）；包围盒走流式 |
| `seismicTieMarker` | 同步保留——遍历资产每会话一次，量级小 |
| catalog 保存 | BatchSave 已收敛为一次落盘（既有）；D5.6 轮转 |
| 剖面解码 / LAS 数据填充 / 文件夹导入 | 已异步（PaleoTaskService，前浪）；P4 加优先级与缓存 |

## 4. 关键设计约束

- **指纹即身份**：`PathCanon::fingerprint(canon, mtimeMs, size)`——mtime/size/
  inode 任一变化即失效；写路径后调 `invalidate`（D1.10）。
- **无缓存仍可用**：任何缓存发布失败（目录只读/盘满）降级为无缓存继续
  （D2.1 的「Publishing failed」教训固化在 `writeCacheFileAtomic`：mkdir -p +
  重试一次 + 如实返回 false）。
- **不改 open() 语义**：`SegyReader::open` 保持逐字节不变（兼容路径）；
  缓存/并行/坏道跳过全部收敛在 `openCached`。ordinal 方言（inline 字恒定）
  自动识别并走顺序路径。
- **vendor 边界**：vendor/sbm SgyIndexCache 的全局缓存目录由
  `SegyIndexStore::ensureLegacyGlobalCacheDir()` 在外面预建（D2.1 的仓外
  根治），vendor 代码不动。
