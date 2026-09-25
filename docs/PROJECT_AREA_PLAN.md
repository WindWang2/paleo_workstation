<!-- /autoplan restore point: "/home/kevin/.gstack/projects/paleo_workstation/master-autoplan-restore-20260925-215334.md" -->
## Implementation plan
# project_area 开发计划

日期：2026-09-25。

验收数据是 `/home/kevin/projects/paleo_project/data/project_area`（约 1.4 GB）。目标是用这套工区走通一层古地理编图：井、层位、地震在同一局部坐标里对齐，D61 能编成可编辑的相多边形并导出。

架构仍以 `docs/PALEO_QGIS_PLAN.md` 为准：QGIS 负责渲染、图层、CRS、编辑和布局，Paleo 负责地质对象和导入。界面仍是现有五页壳，视觉以 `DESIGN.md` 为准。

多源数据管理只取 `paleo-merged-main` 的对象和导入规则（ADR 0056 资产目录、ADR 0059 工区—实体—资产、`libs/ingest` 的分类器与 SMI 井分层解析）。不迁移它的导航树、功能区、井位散点页和概览面板。

编图目标层位是数据里的 **D61**。C6 的层位点是齐的，井分层里只有 12/20 口井有 C6。

## 1. 数据事实

工作坐标是局部直角、单位米，范围大约 `x 0–12800`、`y 0–16400`。层位文件头写明 `Projection: Local Rectangular`、`Units: meters`。`project_area.paleo.json` 把 CRS 标成 `EPSG:4326`，井的 `coordinate_status` 是 `untransformed`。这套数不能按经纬度绘制。

| 来源 | 规模 | 对开发的含义 |
|---|---|---|
| `井位/ExportWellHead.dat` | 20 口，A1–A20 | 井名、X、Y、KB、TD。A1 在 (5288.67, 8219.94) |
| `井曲线/*.Las` | 20 个，各约 2 MB | LAS 2.0。曲线 DEPT、AC、DEN、GR 及 `_S`。NULL 为 -99999 |
| `井分层/DC.dat` | 516 行 | 井名、层名、MD、TVD。Time(ms) 全部是 -99999 |
| `时深/TD/*.dat` | 20 口井 | TIME(ms)、TVDSS、TVD、MD。depth↔TWT 用这张表 |
| `层位/*.dat` | 8 个，各约 26.3 万点 | x、y、z(ms)、Inline、Crossline。网格 411×641。Inline 1315–1725，Crossline 4165–4805 |
| 层序 | 8 个界面 | C3、C6、D53、D61、D62、D63、D71、D72。体系域字段是 LST/TST/HST，没有对应数据 |
| `地震体/200P_seismic.sgy` | 966 MB，约 263451 道 | 三维体。901 样点，2 ms，IBM 浮点（format 1）。道数与层位网格一致 |
| `参考相图/*.geojson` | 相 50、亚相 188、微相 397 | 经纬度约 105–125°E、20–40°N，`period=J3`。与局部测网不是同一空间 |
| `参考资料/` | PNG、PPTX、PDF、XML | 扫描相图、构造图、单井图、编图规范、HZ28-6-1 柱状图。没有地理配准 |

`paleo.json` 里有一条 D61 的 mock 预测，多边形用的是局部米坐标。参考 GeoJSON 不能叠到这张图上。

## 2. 已有代码里要改的行为

下面这些已经写进本仓库，接到 `project_area` 时会错。改这些行为，不另起产品。

| 现况 | 改成 |
|---|---|
| 工程可能把 JSON 里的 EPSG:4326 写成图层 CRS | `coordinate` 仍是 CRS 权威。工作坐标记为局部测网、米。4326 只留作标签，不参与绘制 |
| `DataImportService` 把非 tif/img 都声明成矢量 | 按第 3 节的分类、实体和受管复制导入。`.dat` 不再交给 OGR |
| `SegyReader::open` 对文件 `readAll()`，样本全部进内存 | 按道偏移索引。只解码一条 inline 或 crossline。补上 crossline 道头（默认字节 193） |
| 层位若变成点要素，一张图 26 万个点 | 按文件头的 411×641 网格装箱成时间栅格，空道为 nodata |
| 时深转换若用常速 | 先用该井 TD 表。没有 TD 的井才用常速，并在剖面上标明 |
| ONNX 结果可以落成 1×1 栅格 | 接到本工区时，范围是测网 `0–12793 × 0–16406`，层位是 D61 |
| 参考 GeoJSON 若按矢量图层打开 | 入库为未配准辅助资产，不生成地图图层 |

LAS 2.0 解析和 SEG-Y 的 IBM 浮点解码保留。相多边形算法 `paleo:paleo_facies_polygonize`、连井面板、布局导出保留，本计划只给它们接上这套数。

## 3. 数据管理契约

对象链：

```
工区
 └─ 地质实体或辅助实体
      └─ 显式关联（角色、是否主版本、是否未决）
           └─ 数据资产
                └─ 不可变版本（RAW / DERIVED / INTERMEDIATE / OUTPUT）
```

文件不是井。一口井是稳定 id、井名、UWI 和别名。地震体是一条 `SeismicSurvey`，打开时从道头冻结角点、inline/crossline 范围、采样间隔和起始时间。每个层序界面是一个地质实体。扫描图、PPT、PDF 和未配准 GeoJSON 是辅助实体。

关联写在 `entity_asset_links`，字段是实体类型、实体 id、资产 id、角色、是否主版本、是否未决。角色用已有名字：`well_head`、`well_log`、`tops`、`time_depth`、`horizon`、`seismic_volume`。关系不从标签推断。同名冲突不合并，链接标 `unresolved`。身份顺序是已有 id、UWI、规范化井名、别名。文件名不作身份。测井曲线先读 LAS `~W` 的 WELL。对得上已有井就挂上；对不上再用文件名主名（`A1.Las` → A1）。仍对不上就建未决链接，不新建一口同名井。时深和分层用文件里的井名列，规则相同。层位文件 `D61.dat` 挂到层序界面 D61；文件名不在 8 个层序界面里时，建未决层位实体，不进编图 chip。

井同时保存原始 `surface_x/y` 和 `project_x/y`，以及 `coordinate_status`：`ok`、`untransformed`、`invalid`、`missing`。本工区 20 口井都是 `untransformed`。地图用局部坐标绘制，状态保持未变换，直到出现真正的投影参数。

`catalog.json` 是资产生命周期的主存储。受管文件路径是 `{stage}/{asset_id}/{version_id}/{filename}`。默认导入是受管 RAW：边复制边算 SHA-256，落盘后只读。用户明确选择链接外部时不复制；966 MB 的 SEG-Y 走外部链接。由层位文件装箱得到的时间栅格是 DERIVED，父版本指向该 RAW。`catalog.json` 在 20 口井的规模上直接当查询源。`catalog.sqlite` 仍定义为可重建索引，但不进第一段实现；等资产数量或查询变慢再补。现有图层清单只登记要画进 QGIS 的结果，不兼任文件目录。

分类沿用 `paleo-merged-main/libs/ingest/src/classifier.cpp`：

| 路径或扩展名 | 类型 | 资产角色 |
|---|---|---|
| `井位/`，或文件名含 wellhead | `well_head` | input |
| `井分层/` | `well_stratification` | input |
| `时深/`，或路径段 `td` | `time_depth` | input |
| `层位/` | `horizon` | input |
| `.las` | `well_log` | input |
| `.sgy` / `.segy` | `seismic` | input |
| `.geojson` | `geojson` | input，未配准则不进地图 |
| `.pdf` `.ppt` `.pptx` `.doc` `.docx` | `document` | reference |
| `.png` `.jpg` `.tif` | `image_reference` | reference |
| `.xml` | 再看内容 | 井口或测井；判不出则作参考 |

井分层解析与 `parse_well_tops_text` 一致：`#` 行跳过，列是井名、层名、MD、X、Y、Z、TVD、Time(ms)。

## 4. 数据页用标签页预览

预览只出现在数据管理页，不泄漏到预测、约束、编图、验证。顶部工作流标签栏仍然是唯一的签名元素。预览用页内普通的 `QTabWidget`，样式走 `DESIGN.md` 的 dock 面板，不用工作流标签的蓝色下划线。

布局：右侧仍是资产列表。地图留在中央。列表下方或地图下方放预览标签栏，只在数据管理页可见。从列表选中一条资产时，若已有同资产标签则切过去，否则新开一个可关闭标签。

空态文案是「还没有打开的预览 — 在列表中选择一条数据」。复制或解析还在进行时，标签显示「正在读取」和文件名，不显示半份曲线。失败时标签内给出原因和文件名，不留白面板。外部链接的文件如果路径不存在，地震或文档标签写「找不到源文件」和那条路径。

| 资产类型 | 标签里显示什么 |
|---|---|
| `well_log` | 单井曲线。默认 GR，可换 AC、DEN。用现有单道绘制，不把多井连井面板搬进这个标签 |
| `well_stratification` | 该井的分层表：层名、MD、TVD。Time 列为空就显示空，不填假时间 |
| `time_depth` | 该井的 TIME–TVD 曲线 |
| `well_head` | 井名、X、Y、KB、TD、`coordinate_status`。选中时地图同时高亮该井 |
| `horizon` | 网格尺寸、Z 的单位和范围、派生栅格是否已生成。提供「在地图上显示」。标签内不画 26 万个点 |
| `seismic` | 现有地震预览。标签内选择一条 inline 或 crossline，只解码这一条 |
| `image_reference` | 按面板宽度缩放的图片 |
| `document` | 文件名、类型，以及「用系统程序打开」。这一阶段不做 PDF 内嵌翻页 |
| `geojson` | 要素个数、坐标范围、相名字段。未配准时标明不加入地图 |

导入向导最后一步的确认预览仍是向导里的一步。确认入库之后，才在数据页打开对应标签。

## 5. 分阶段计划

每段都用 `project_area` 里的文件验收。966 MB 的 SEG-Y 和 8 个层位点文本不提交进本仓库。测试夹具是从中切出的一条 inline、一口井的 LAS/TD，以及 D61 栅格。

### 阶段 A — 实体、受管原文和预览标签

导入 `project_area` 后：

- A1 有四条主关联：井口、LAS、分层、时深。受管副本只读，SHA-256 与源文件一致。
- D61 有层位关联。派生时间栅格登记到图层清单，能在地图上打开。
- A1 落在 (5288.67, 8219.94) 附近，并压在 D61 栅格上。
- 图层 CRS 不是 EPSG:4326。井的 `coordinate_status` 仍是 `untransformed`。
- 在数据页依次打开 A1 的井口、LAS、分层、时深和 D61 层位，得到五个可关闭标签，来回切换不丢内容。LAS 标签能看到 GR。分层表里 Time 为空。D61 标签能把派生栅格显示到地图上。

这一阶段不读地震道样本。地震资产可以出现在列表里，打开标签时说明剖面在下一阶段才可用。

### 阶段 B — 一条地震剖面

- SEG-Y 建立 inline/crossline 到文件偏移的索引，不把体读进内存。
- 打开地震资产时使用数据页上的地震标签。标签内选择一条 inline 或 crossline，只解码这一条（约 411 或 641 道，901 样点，2 ms）。
- A1 的 D61 分层用 TD 表换成毫秒，标到这条剖面上。没有 TD 的井用常速，并标注。
- 验收夹具是这一条 inline，不是整个 `.sgy`。切换到 A1 的时深标签再切回地震标签，剖面仍在。

### 阶段 C — 只编 D61

- 结构面是 D61 时间栅格。
- 单因素先算井上 D61 到 D62 的 TVD 厚度，再在测网网格上做现有的约束 IDW。IDW 仍按凸包裁剪。砂地比、距井距离、屏障距离不在本阶段。
- 预测或融合栅格使用测网范围。相多边形走已有的 `paleo:paleo_facies_polygonize`。
- 验证项是井上 D61 时间（由 TD 表得到）与 D61 栅格在井位处的差。超过阈值成为一条问题。点开后地图缩放到该井，连井滚到该分层，地震滚到对应测线和时间。
- 布局导出一张含井位和相多边形的 PDF。

完成标准：20 口井里凡有 D61 分层的井都有一条时间残差；其中一口井走完「问题 → 三视图」。

### 阶段 D — 辅助资料

- PNG、PPTX、PDF 作为 `document` 或 `image_reference` 受管入库，角色 `reference`，挂到辅助实体。在数据页打开后各自成为一个标签：图片直接显示，文档提供「用系统程序打开」。
- 三份 GeoJSON 入库并标明未配准，不生成地图图层。标签里能看到要素个数和相名。相、亚相、微相名称收成图例字典。
- HZ28-6-1 的 XML 不并进 A1–A20。打开它时标签标明这是参考资料，不写成 A1 的曲线。
- 完成后，地图上的井和 D61 位置与阶段 A 相同。

### 阶段 E — 八个层位，然后才是版本状态机

- 编图 chip 只有 C3、C6、D53、D61、D62、D63、D71、D72。切换沿用已有的按层位懒加载。
- 井分层里其余名字只出现在连井。
- D61 能保存并导出之后，再实现「保存版本 / 发布 / Published」。在那之前不为状态机单开一段。

## 6. 本计划不做

- 把参考 GeoJSON 或扫描相图配准到局部测网。
- 地震体渲染、任意测线、三维相机。`geo3d_workspace.json` 不迁。
- 体系域 LST/TST/HST。
- 砂地比、距井距离、TIN、等值线、屏障 IDW、相序规则融合、多 realization、相界地质类型、暗色模式。
- 把 SEG-Y 全本或层位点文本提交进本仓库。
- `paleo-merged-main` 的数据管理界面。

## 7. 第一段实现

阶段 A 的最小切片和阶段 B 的一条剖面一起做：

1. 局部测网 CRS，不写 4326。
2. A1 的井实体，以及井口、LAS、分层、时深四条主关联和只读 RAW。
3. D61 层位实体、其 RAW，以及父版本指向该 RAW 的时间栅格。
4. `200P_seismic.sgy` 外部链接，抽出一条 inline。
5. 用 A1 的 TD 表把 D61 标到这条剖面上。
6. 数据页预览标签栏：至少能同时打开 A1 的 GR、A1 的时深和这一条地震剖面，三个标签互不覆盖。

阶段 C 的残差要等这三样对齐之后再算。

<!-- autoplan-accepted:ceo -->
- Work coordinates are a local meter grid. EPSG:4326 stays a label and is not written onto map layers. Wells keep `coordinate_status=untransformed` until a real transform exists.
- Import classifies by the `libs/ingest` path rules, parses metadata, resolves or creates an entity, writes an explicit `entity_asset_link`, and stores a read-only RAW with SHA-256. The 966 MB SEG-Y is an external link.
- LAS binds through the WELL mnemonic, then the filename stem. Tops and TD bind through the well-name column, then the stem. A miss creates an unresolved link and does not create a duplicate well. A horizon filename in {C3,C6,D53,D61,D62,D63,D71,D72} binds to that boundary; any other name is an unresolved horizon and stays off the map chips.
- D61.dat becomes a RAW whose child is a 411×641 time raster with nodata on empty bins. The raster is what the map shows.
- Data-page preview is a closable QTabWidget, not the workflow tab bar. Re-selecting an open asset focuses its tab. Loading, empty, and error states use the copy in section 4, including a missing external path.
- Phase A acceptance includes five open tabs for A1 well head, LAS (GR visible), tops (Time blank), TD curve, and D61 (grid stats plus show-on-map).
- Phase B decodes one inline or crossline only. A1 D61 is posted in time using that well's TD table.
- `catalog.sqlite` is specified but not in the first slice.
- Verification: QTest for classify, well bind, unresolved name, horizon binning, checksum of a copied LAS, missing external path, and tab focus-on-reselect. Manual check: A1 at (5288.67, 8219.94) on the D61 raster.
<!-- /autoplan-accepted:ceo -->
## Review record

Mode: SELECTIVE EXPANSION. Authority: /autoplan override, not a user menu. Base branch: `main` (no git remote; the only local branch is `master`).
UI scope: yes. The keyword counter returned matchCount 0 because the plan is Chinese; section 4 names `QTabWidget`, closable preview tabs, and per-type empty/error states. Design review will run.
DX scope: no. The product is a geologist's workbench. `paleo-dev` is not what this plan changes. Phase 2.5 will be skipped.
Outside voice: Codex CLI is installed but `gpt-6-astra` returns HTTP 400 ("requires a newer version of Codex"). Prior learning `codex_exec_network_flaky` and `native_fallback_timeouts` (2026-09-25). This phase will try the native fallback and will not treat a timeout as a clean review.
Search: Aside is not installed. Landscape from web search plus in-distribution knowledge is in 0C.
Prior learning applied: `paleo_qgis_native_widgets` (10/10, 2026-09-25). Preview tabs stay a normal `QTabWidget`. The numbered workflow tab bar stays the only bespoke chrome.
Cross-project learnings were already enabled. No config write this run.

### 0A. Premise

The pain is that `project_area` cannot be opened honestly by the current importer: coordinates would be treated as EPSG:4326, `.dat` would be declared as vectors, and the 966 MB SEG-Y would be read into memory. The plan attacks that directly, then uses the aligned well, D61 grid, and one seismic section to edit one facies map. Doing nothing leaves the geologist with a workbench that cannot show their own工区. The premises (local meters, D61 as the target horizon, entity-then-file, no old UI, tabbed preview) match the files and the user's last three instructions. Accepted.

### 0B. Existing code

| Sub-problem | Reuse | Do not rebuild |
|---|---|---|
| LAS 2.0 | `src/io/lasparser.cpp` | a second LAS parser |
| IBM float SEG-Y | `src/io/segyreader.cpp` sample decode | the `readAll()` ingest |
| Single-well curve drawing | correlation track widgets | a new plot library |
| Seismic section pixels | `SeismicPreviewPanel` | a volume viewer |
| Facies polygons | `paleo:paleo_facies_polygonize` | a second polygonize |
| Horizon→raster | new binning on the stated 411×641 grid | IDW of 263k points |
| SMI tops columns | same columns as `parse_well_tops_text` | a new tops dialect |
| Layer declarations | `LayerManifest` for map results only | using it as the file catalog |

### 0C. Dream state

```
CURRENT STATE                         THIS PLAN                         12-MONTH IDEAL
Importer copies files and calls       Entity, link, RAW, one D61 grid,  Eight horizons, residuals,
unknown types vectors. SEG-Y is       one inline, preview tabs.         versions, published maps.
loaded whole.                         --->                              --->
```

Landscape: Seequent Central and GeoticMine both keep geological entities and attach files with lineage (gitnux.org geological data management 2026; GeoticMine "lineage links ingested records to interpretation outputs"). OSDU-style well-then-dataset is the same shape as ADR 0059. A knowledge-graph program (DDE, Journal of Geographical Sciences 2025) is the 12-month-plus direction, not this slice. First principles: 20 wells do not need a graph database. The previous app's entity and RAW catalog is the right size because this folder was produced by that app.

### 0E. Mode

SELECTIVE EXPANSION. The plan adds a catalog and a preview surface, and it already cuts volume rendering, registration, and the old data-manager UI. Cherry-picks below are the only scope changes. No new approach decision was needed.

### 0G. Scope decisions (auto-decided)

| # | Proposal | Effort | Decision | Reasoning |
|---|---|---|---|---|
| 1 | Bind LAS, tops, and TD with header or name column, then filename stem; unknown names stay unresolved | S | ACCEPTED | The identity rule otherwise cannot be implemented. P5 explicit. |
| 2 | Preview tab shows "正在读取" until copy and parse finish, then content or a named error | S | ACCEPTED | Blast radius of section 4. P1 completeness. |
| 3 | Missing external SEG-Y or document path shows "找不到源文件" plus the path | S | ACCEPTED | Same blast radius. |
| 4 | First slice lists four A1 links, including LAS, matching stage A | S | ACCEPTED | Fixes an internal contradiction. |
| 5 | Build `catalog.sqlite` in the first slice | M | DEFERRED | 20 wells fit in `catalog.json`. A second store is new infra. P3 pragmatic. |

Deferred item 5 is recorded in TODOS.md during this review.

<!-- autoplan-accepted:ceo -->
- Work coordinates are a local meter grid. EPSG:4326 stays a label and is not written onto map layers. Wells keep `coordinate_status=untransformed` until a real transform exists.
- Import classifies by the `libs/ingest` path rules, parses metadata, resolves or creates an entity, writes an explicit `entity_asset_link`, and stores a read-only RAW with SHA-256. The 966 MB SEG-Y is an external link.
- LAS binds through the WELL mnemonic, then the filename stem. Tops and TD bind through the well-name column, then the stem. A miss creates an unresolved link and does not create a duplicate well. A horizon filename in {C3,C6,D53,D61,D62,D63,D71,D72} binds to that boundary; any other name is an unresolved horizon and stays off the map chips.
- D61.dat becomes a RAW whose child is a 411×641 time raster with nodata on empty bins. The raster is what the map shows.
- Data-page preview is a closable QTabWidget, not the workflow tab bar. Re-selecting an open asset focuses its tab. Loading, empty, and error states use the copy in section 4, including a missing external path.
- Phase A acceptance includes five open tabs for A1 well head, LAS (GR visible), tops (Time blank), TD curve, and D61 (grid stats plus show-on-map).
- Phase B decodes one inline or crossline only. A1 D61 is posted in time using that well's TD table.
- `catalog.sqlite` is specified but not in the first slice.
- Verification: QTest for classify, well bind, unresolved name, horizon binning, checksum of a copied LAS, missing external path, and tab focus-on-reselect. Manual check: A1 at (5288.67, 8219.94) on the D61 raster.
<!-- /autoplan-accepted:ceo -->

