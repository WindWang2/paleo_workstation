<!-- /autoplan restore point: "/home/kevin/.gstack/projects/paleo_workstation/master-autoplan-restore-20260926-132529.md" -->
<!-- /autoplan restore point: "/home/kevin/.gstack/projects/paleo_workstation/master-autoplan-restore-20260925-215334.md" -->
## Implementation plan
# project_area 开发计划

日期：2026-09-25。

验收数据是 `/home/kevin/projects/paleo_project/data/project_area`（约 1.4 GB）。目标是用这套工区走通一层古地理编图：井、层位、地震在同一局部坐标里对齐，D61 的厚度和井震残差能导出。相多边形只在已有相编码栅格时导出，不从厚度栅格生成。

架构仍以 `docs/PALEO_QGIS_PLAN.md` 为准：QGIS 负责渲染、图层、CRS、编辑和布局，Paleo 负责地质对象和导入。界面仍是现有五页壳，视觉以 `DESIGN.md` 为准。

多源数据管理只取 `paleo-merged-main` 的对象和导入规则（ADR 0056 资产目录、ADR 0059 工区—实体—资产、`libs/ingest` 的分类器与 SMI 井分层解析）。不迁移它的导航树、功能区、井位散点页和概览面板。

编图目标层位是数据里的 **D61**。C6 的层位点是齐的，井分层里只有 12/20 口井有 C6。

## 1. 数据事实

工作坐标是局部直角、单位米，范围大约 `x 0–12800`、`y 0–16400`。层位文件头写明 `Projection: Local Rectangular`、`Units: meters`。`project_area.paleo.json` 把 CRS 标成 `EPSG:4326`，井的 `coordinate_status` 是 `untransformed`。这套数不能按经纬度绘制。

| 来源 | 规模 | 对开发的含义 |
|---|---|---|
| `井位/ExportWellHead.dat` | 20 口，A1–A20 | 井名、X、Y、KB、TD。A1 在 (5288.67, 8219.94) |
| `井曲线/*.Las` | 20 个，各约 2 MB | LAS 2.0。曲线 DEPT、AC、DEN、GR 及 `_S`。NULL 为 -99999 |
| `井分层/DC.dat` | 516 行 | 表头是井名、层名、MD、X、Y、Z、TVD、Time(ms)。Time(ms) 全部是 -99999。缺 X、Y、Z 时忽略这三列，不把文件判为解析失败 |
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
| 工程可能把 JSON 里的 EPSG:4326 写成图层 CRS | 图层 CRS 是无基准工程坐标，单位米，authid 留空，不能反投到 EPSG:4326。JSON 里的 EPSG:4326 只留在源标签上 |
| `DataImportService` 把非 tif/img 都声明成矢量 | 按第 3 节的分类、实体和受管复制导入。`.dat` 不再交给 OGR |
| `SegyReader::open` 对文件 `readAll()`，样本全部进内存 | 按道偏移索引，不在界面线程上做。只解码一条 inline 或 crossline。本文件实测 1012709244 字节、901 样点、2 ms、format 1、道距 3844、263451 道。道头偏移 188 的 inline 和偏移 192（SEG-Y 1-based 字节 193）的 crossline 都是 0。CDP 在偏移 20，每 641 道从 4165 排到 4805。源坐标道 0 为 (0,0)，道 640 为 (12793,0)，末道为 (12793,16406)。索引：inline = 1315 + 道号/641，crossline = 该道 CDP。道数、CDP 顺序、角点有一项对不上就停止并显示读到的数，不改去扫别的字节。删掉 `SegyReader::open` 在偏移 188 不变时改读偏移 8 和 20 的回退。本文件偏移 188 全是 0，CDP 在偏移 20 会变化，那条回退会盖掉按道号算出的 inline。角点用道头偏移 72 和 76（Source X/Y，整数米；偏移 180/184 的 CDP X/Y 在本文件恒为 0，初稿写错位）；比例因子 0 或 1 都表示用这个整数。对不上就并排写出期望值和读到的值 |
| 层位若变成点要素，一张图 26 万个点 | 按文件头的 411×641 网格装箱成时间栅格，空道为 nodata |
| 时深转换若用常速 | 只用该井 TD 表做线性插值。没有 TD 的井标「无时深表」，不使用常速 |
| ONNX 结果可以落成 1×1 栅格 | 接到本工区时使用 D61 的 geotransform。挤成二维后不是 411×641 就失败，不写栅格，预测页写「结果不是 411×641，没有写入栅格」，并写出实际行列数 |
| 参考 GeoJSON 若按矢量图层打开 | 入库为未配准辅助资产，不生成地图图层 |

LAS 2.0 解析和 SEG-Y 的 IBM 浮点解码保留。扩展名忽略大小写，`A1.Las` 也按测井分类。连井面板和布局导出保留，并接到这套工区。相多边形算法保留，只接收相编码栅格，不接收厚度栅格。

## 3. 数据管理契约

对象链：

```
工区
 └─ 地质实体或辅助实体
      └─ 显式关联（角色、是否主版本、是否未决）
           └─ 数据资产
                └─ 不可变版本（RAW / DERIVED / INTERMEDIATE / OUTPUT）
```

文件不是井。一口井在本计划里只存 id、规范化井名、surface_x/y、KB、TD 和 coordinate_status。不建 UWI 列，也不建别名列。地震体是一条 `SeismicSurvey`，打开时从道头冻结角点、inline/crossline 范围、采样间隔和起始时间。每个层序界面是一个地质实体。扫描图、PPT、PDF 和未配准 GeoJSON 是辅助实体。

关联写在 `entity_asset_links`，字段是实体类型、实体 id、资产 id、角色、是否主版本、是否未决。角色用已有名字：`well_head`、`well_log`、`tops`、`time_depth`、`horizon`、`seismic_volume`、`reference`。表里的 input 和 reference 是资产角色，关联角色是这一列。关系不从标签推断。井名比较前去掉首尾空白、连字符和空格，并忽略大小写。井口文件带 UTF-8 BOM。读表头前去掉 U+FEFF。`trimmed()` 去不掉它。列是 Name、X、Y、KB、TotalDepth、BottomX、BottomY、WellType。TotalDepth 记为 TD。BottomX、BottomY、WellType 不新增井表列。井口预览从这份井口资产的行里读。没有 UWI。匹配键是规范化井名。井口文件是建井来源：规范化后的井名还没有已有井，就新建一口井并挂 `well_head`。井口文件里同一个规范化名字出现两行，或这一行同时匹配两口已有井，该行标 `unresolved`，不新建，也不合并。测井、分层和时深不新建井。它们先用 LAS `~W` 的 WELL 或文件中的井名列匹配已有井；零个匹配再用文件名主名（`A1.Las` → A1）。仍然零个，或两个已有井都匹配，资产仍然保留，链接标 `unresolved`，实体 id 留空，不新建井，也不合并。现有 `DataCatalog::addLink` 会拒绝空的实体 id。放行空实体 id，并增加备注字段，`catalog.json` 能往返。两个候选时，链接备注写下两个规范化井名。井口行恰好匹配一口已有井时，挂上 `well_head`，不另建井。SHA-256 已经入库时不新建版本。若未决关联这时能按规范化井名挂上，可以补这一条主关联，不合并两口井。界面说明字节已在库，并说明这次有没有补上关联。SHA-256 是新的才追加不可变版本，该角色只保留这一条主关联。层位文件 `D61.dat` 挂到层序界面 D61。文件名不在 C3、C6、D53、D61、D62、D63、D71、D72 里时，建未决层位实体，不进编图 chip。

`井位/ExportWellHead.dat` 和 `井分层/DC.dat` 各是一份多井文件，各登记为一个资产。每个井名一条关联，指向同一资产。多井文件不拆成 20 份。每口井的过滤留在该预览标签自己的「井」下拉框里，不跟另一张标签走。

井保存 `surface_x/y` 和 `coordinate_status`：`ok`、`untransformed`、`invalid`、`missing`。本工区 20 口井都是 `untransformed`。在出现真正的投影参数之前不写 `project_x/y`，地图读 `surface_x/y`。图层 CRS、工程 CRS 和画布 CRS 都是无基准的工程坐标，单位米，authid 留空，不能反投到 EPSG:4326。不用 `+proj=eqc`。现有 `DataCatalog::localGridCrsProj()` 就是 `+proj=eqc +ellps=WGS84 +units=m +no_defs`，会把局部米反投到 EPSG:4326。删掉这串。图层、工程和画布共用一个无大地基准的工程米坐标。`mapUnits` 是米，authid 为空，`isGeographic` 为假，转到 EPSG:4326 必须失败。

`catalog.json` 是资产生命周期的唯一主存储，也是这一阶段的查询源。写入走工程写队列，先临时文件再改名。`asset_id`、`version_id`、`filename` 各只占一段路径，不允许斜杠、`..`、换行、NUL 和其他控制字符。文件夹导入只收普通文件，不跟随指向所选目录之外的符号链接。一行失败不中断其余文件。受管路径是 `{stage}/{asset_id}/{version_id}/{filename}`。逻辑阶段仍是 `RAW`、`DERIVED`、`INTERMEDIATE`、`OUTPUT`。目录段用小写，跟现有 DataCatalog 一致。catalog.json 在工程目录的 `artifacts/metadata/catalog.json`。默认导入是受管 RAW：边复制边算 SHA-256，落盘后只读。用户明确选择链接外部时不复制；966 MB 的 SEG-Y 走外部链接。链接时流式计算 SHA-256。打开时摘要不一致就写「源文件与入库时的 SHA-256 不一致」，不解码。由层位文件装箱得到的时间栅格是 DERIVED，父版本指向该 RAW。`catalog.sqlite` 不在本计划的实现里。触发条件写在 `TODOS.md` 的「P3 — catalog.sqlite 查询索引」：catalog.json 能往返并且列表查询变慢。现有图层清单只登记要画进 QGIS 的结果，不兼任文件目录。

分类沿用 `paleo-merged-main/libs/ingest/src/classifier.cpp`：

| 路径或扩展名 | 类型 | 资产角色 |
|---|---|---|
| `井位/`，或文件名含 wellhead | `well_head` | input |
| `井分层/` | `well_stratification` | input |
| `时深/`，或路径段 `td` | `time_depth` | input |
| `层位/` | `horizon` | input |
| `.las`，扩展名忽略大小写 | `well_log` | input |
| `.sgy` / `.segy` | `seismic` | input |
| `.geojson` | `geojson` | input，未配准则不进地图 |
| `.pdf` `.ppt` `.pptx` `.doc` `.docx` | `document` | reference |
| `.png` `.jpg`，以及没有地理变换的 `.tif` | `image_reference` | reference |
| `.xml` | 再看内容 | 井口或测井；判不出则作参考。`参考资料/` 里 HZ28-6-1 的 XML 固定为辅助参考，不挂到 A1–A20 |

井分层解析与 `parse_well_tops_text` 一致：`#` 行跳过，列是井名、层名、MD、X、Y、Z、TVD、Time(ms)。值为 -99999 的时间、TVD 或 MD 视为空，不参加计算。

D61 栅格只使用层位文件头，不另设一套范围。网格 411×641。P1（inline 1315，crossline 4165）= (0, 0)，P2（1315，4805）= (12793, 0)，P3（1725，4805）= (12793, 16406)。X 随 crossline 增加，Y 随 inline 增加。像元 `dx = 12793/640` 米，`dy = 16406/410` 米。北向上的 geotransform 六个系数是 (0, dx, 0, 16406, 0, -dy)。原点是左上角像元的外角，不是像元中心。层位点按列号、行号写入该像元。残差和 IDW 用像元中心，中心比节点向网格内侧偏半个像元。本计划不改这组原点。空道 nodata 为 -9999。列号 = crossline - 4165，行号 = 1725 - inline。越界的点不写入，只计入拒绝数。同一像元多点时按文件顺序保留最后一点，并在栅格元数据里记下碰撞次数。A1 的 D61 TVD 是 1935 m，D62 TVD 是 1971 m。A1 (5288.67, 8219.94) 落在这个网格内，验收用这个精确坐标，容差半个像元。

时深转换只用 TD 表。分层的 TVD 对 TD 的 TVD 列线性插值得到 TIME(ms)。该井 TVD 为空时改用 MD 对 TD 的 MD 列。-99999 不参加插值。20 口井都有 TD 文件。若某口井没有 TD 表，剖面上标井名和「无时深表」，不使用常速。`docs/PALEO_QGIS_PLAN.md` 里缺省常速的写法不用于这套工区。深度落在 TD 表可用样点的范围之外时不外推，该层标「超出时深表」，不进入残差。用来查找的那一列不是严格递增，或去掉 -99999 之后不足两个有限样点时，这口井标「时深表无序」或「无时深表」：不是严格递增用前者，样点不足两个用后者。两种都不插值。现有 `TimeDepthTool::interpolateTimeMs` 会按深度排序，并在范围外夹到端点。改成按文件顺序、不排序、不夹取。剖面和残差共用这一个结果。同步改 `tst_timedeptool`。

井在剖面上的位置、最近层位采样和残差都用该层分层点的 X、Y。分层点没有坐标时才退回井口。井口和分层点相差超过一个像元时，剖面上标出这个偏移。第一段默认打开的测线是 A1 对应的那条 inline。

## 4. 数据页用标签页预览

预览只出现在数据管理页，不泄漏到预测、约束、编图、验证。顶部工作流标签栏仍然是唯一的签名元素。预览用页内普通的 `QTabWidget`，样式走 `DESIGN.md` 的 dock 面板，不用工作流标签的蓝色下划线。

布局：右侧 dock 仍放导入和资产表。地图留在中央。`DataPreviewTabs` 现在嵌在右侧 dock 的资产表下面。本计划把它移到地图下方的中央列，只在数据管理页出现。没有打开的标签时，这里只有一行次级文字，颜色 #5D6E80，字号 9pt：「还没有打开的预览 — 在列表中选择一条数据」。第一个标签打开后，地图和预览用竖向 QSplitter 分开。预览一开始约占中央列高度的三分之一，可以拖。预测、约束、编图、验证不放这根分割，也不放预览标签。底部 dock 继续放日志、任务、连井剖面，已有的发布和属性页保留。单线地震只出现在数据页的预览标签里。底部不再同时放一条地震预览。验证行的双击不再调用底部地震页。离开数据页时藏起预览分割条，不另造一块地图。

从列表选中一条资产时，已有同资产标签就切过去，否则新开一个可关闭标签。关掉标签就取消读取。离开数据页只隐藏预览，不拆掉。索引、LAS 解析、层位装箱和单条解码不在界面线程上做。超过 1 秒的进度出现在底部「任务」页，超过 10 秒按已读字节线性估计剩余时间。标签关掉之后晚到的结果丢弃。

标签标题是「文件名 · 井或测线」，例如「DC.dat · A1」「200P · IL1315」。曲线名不进标题。状态句只在正文，不替换标题。正文里才写「正在读取」和文件名，不显示半份曲线。失败时正文写「读取失败」、原因和文件名，并给「重试」。外部链接的源文件不存在时，正文写「找不到源文件」和那条路径。

多井文件（井口表、DC.dat）每个预览标签自带一个标为「井」的下拉框，只列出挂到这份资产上的井。下拉框不改其他标签里已选的井。还没选井时，正文是「先选择一口井」，不是空表。选中后标题写成「DC.dat · A1」。打开 A1 的 LAS 只聚焦 LAS 标签，不改已经打开的分层标签。

资产表列是名称、类型、关联。关联显示井名，或警告标签「未决」。标签用浅底 #FFF4E0，字用 #24303E，边用 #F29900，并且始终带「未决」二字。tooltip 写出两个规范化井名。参考资产显示「参考」。没有合并动作。未决行上有一个井下拉框，列出已有井，默认空。「挂到这口井」只有选中一口之后才可点。确认时同时写出资产名和井名。这一步可以撤销：撤销只把关联改回未决，不删除仍被别的关联用着的井。同一角色的旧版本可以用「将此版本设为主版本」，不复制字节。名称的 tooltip 保留受管或外部路径。

数据页在现有「导入井数据」「导入地震数据」「导入边界数据」旁边增加「导入工区文件夹」。单文件按钮仍选一个文件，确认后只打开这一条的标签。文件夹按钮选 `project_area` 目录。一次确认里先处理井口行，再关联测井、分层、时深和层位。LAS 排在井口文件前面时，A1 仍然得到四条主关联。下一步是分类表：路径、类型、实体、未决或失败。不让用户重排列。确认前可以改这一行的类型，规范化匹配键照常显示。HZ28-6-1 这一行的类型不能改，始终是参考。同目录其他 XML 默认显示为参考；改成井之后如果对不上 A1–A20，保持未决，不用文件名去挂井。预览步只显示口数、井名和失败数，不展开层位散点，也不解地震道。确认步的三个数只按这张表计，然后只打开井口标签。失败：文件没有落盘，原因是读写、解析或路径不合法，行上写原因和「重试」。未决：资产已保存，实体 id 为空；tooltip 用「无匹配」「两个候选: 名字, 名字」或「井口重名」。入库：写成了一条主关联。确认文案仍是「入库 n，未决 n，失败 n」。其余资产等列表点击。SMI 文本的列是固定的，向导展示识别出的列。同一 SHA-256 在确认步写「字节已在库」，以及「已补上关联」或「没有新的关联」，然后聚焦已有标签，不新开第二个。CRS 步不是坐标系下拉框。正文是「局部工程坐标，单位米。源文件里的 EPSG:4326 只是标签，不会画到地图上。」不能改成会反算到经纬度的 CRS。与 `docs/PALEO_QGIS_PLAN.md` §42.7 的 CRS 选择器冲突时，以这句和空 authid 为准。状态栏用同一事实加一条次级文字：「工程坐标 · 米 · 未投影」，颜色 #5D6E80，不用警告色。这 20 口井都是这个状态。`invalid` 写「坐标无效」，`missing` 写「没有坐标」，仍用 #5D6E80。

| 资产类型 | 标签里显示什么 |
|---|---|
| `well_log` | 单井曲线。曲线用下拉框，默认 GR，可换 AC、DEN。缺的曲线项禁用，tooltip「这条曲线不在文件里」。整条都是 -99999 时写「这条曲线没有有效样点」，不绘制。深度和曲线值用 JetBrains Mono 9pt，右对齐。不把多井连井面板搬进这个标签 |
| `well_stratification` | 该井的分层表：层名、MD、TVD、X、Y。Time 列为空就显示空，不填 -99999，也不填假时间。数字列用 JetBrains Mono 9pt，右对齐 |
| `time_depth` | 该井的 TIME–TVD 曲线。没有可用样点时写「无时深表」，不画假线 |
| `well_head` | 井名、X、Y、KB、TD、BottomX、BottomY、WellType、`coordinate_status`。选中时地图同时高亮该井。数字用 JetBrains Mono 9pt |
| `horizon` | 网格尺寸、Z 的单位和范围、派生栅格是否已生成、拒绝点数、碰撞次数。提供「在地图上显示」。点下去缩放到栅格并闪一下，按钮变成「已在地图上」。隐藏仍用左侧图层树的勾选。栅格还没有时按钮禁用，tooltip「还没有这个层位的栅格」。标签内不画 26 万个点 |
| `seismic` | 数据页里的唯一地震预览。纵测线 / 横测线切换，数值框的范围来自打开时冻结的测网。本文件是 inline 1315–1725、crossline 4165–4805。初始值是 A1 所在 inline，旁注「A1 所在测线」。索引没建好时数值框禁用，tooltip「正在建立道索引」。换测线先清掉上一张剖面。标定写「A1 D61」和时间，或「无时深表」「超出时深表」「时深表无序」之一，没有数值就不标时间 |
| `image_reference` | 按面板宽度缩放的图片，并写「未配准，不加入地图」 |
| `document` | 文件名、类型，以及用本地文件 URL 打开。打不开时写「系统没有打开这个文件」和原因。并写「未配准，不加入地图」。这一阶段不做 PDF 内嵌翻页 |
| `geojson` | 要素个数、坐标范围、相名字段。注明「经纬度，与本测网不是同一空间」，不加入地图 |
| `xml` 参考 | HZ28-6-1 写「未配准，不加入地图」和「不对应 A1–A20」 |

键盘焦点是 2px #1B73D0 描边。禁用控件带原因 tooltip。资产表、预览标签、井下拉框、验证表的 accessible name 等于各自的可见标题。线框里的搜索是现有定位器，不在这一段新做。窗口最小仍是 1280×800。本计划不做手机版。dock 可以浮动。

```
工作流标签    层位 chips                 搜索
图层 dock | 地图                        | 导入 + 资产表
          | 预览 QTabWidget（仅数据页）  |
底部 dock：日志 | 任务 | 连井剖面
状态栏：坐标  比例尺  层位  工程坐标 · 米 · 未投影
```

## 5. 分阶段计划

每段都用 `project_area` 里的文件验收。966 MB 的 SEG-Y 和 8 个层位点文本不提交进本仓库。测试夹具是从中切出的一条 inline、一口井的 LAS/TD，以及 D61 栅格。

### 阶段 A — 实体、受管原文和预览标签

导入 `project_area` 后：

- A1 有四条主关联：井口、LAS、分层、时深。受管副本只读，SHA-256 与源文件一致。
- D61 有层位关联。派生时间栅格登记到图层清单，能在地图上打开。
- A1 落在 (5288.67, 8219.94)，容差半个像元，并压在 D61 栅格上。
- 图层、工程和画布都是无基准工程坐标，单位米，authid 为空，不能反投到 EPSG:4326。井的 `coordinate_status` 仍是 `untransformed`。
- 在数据页依次打开 A1 的井口、LAS、分层、时深和 D61 层位，得到五个可关闭标签，来回切换不丢内容。LAS 标签能看到 GR。分层表里 Time 为空。D61 标签能把派生栅格显示到地图上。

第一段实现把阶段 A 和阶段 B 一起交付，地震标签解码一条测线。

### 阶段 B — 一条地震剖面

- SEG-Y 建立 inline/crossline 到文件偏移的索引，不把体读进内存。
- 打开地震资产时使用数据页上的地震标签。标签内选择一条 inline 或 crossline，只解码这一条（约 411 或 641 道，901 样点，2 ms）。
- A1 的 D61 分层按第 3 节的 TVD 插值换成毫秒，标到 A1 所在的那条 inline 上。插值没有得到数值时，剖面上标第 3 节选出的原因：「无时深表」、「超出时深表」或「时深表无序」。只有得到数值时才标时间。
- 验收夹具是这一条 inline，不是整个 `.sgy`。切换到 A1 的时深标签再切回地震标签，剖面仍在。

### 阶段 C — 只编 D61

- 结构面是 D61 时间栅格。
- 单因素先算井上 D61 到 D62 的厚度，厚度用已经装箱的 D62 与 D61 时间栅格相减，得到双程时间等厚。一口井要同时有 D61 和 D62 的 TVD，才提供间隔速度，不把 MD 厚度混进同一张栅格。缺 D61 或 D62 的 TVD，或两层时深插值有一层没有数值，这口井不提供样本，并写明原因。井上 dt_ms 是 D62 时间减 D61 时间，单位毫秒，双程，时间来自时深表而不是层位栅格。Vint = (TVD_D62 − TVD_D61) / (dt_ms / 2000)，单位 m/s。栅格上等厚米数 = isochron_ms / 2000 × 该像元的 Vint。等厚若就用这口井自己的 dt_ms，米数等于两层 TVD 之差。A1 的插值约 16.23 ms，对应 36 m，只核对公式，不是栅格像元的期望值。IDW 插的是 Vint，power 为 2，权重与 `paleo:paleo_constraint_idw` 相同。测试用一对已知数按这个式子算期望米数，采样在 D61 像元中心，不用它默认的外扩网格，不传约束线。能提供样本的井不足 3 口时不写假曲面。约束页面板写「厚度样本不足以成面」。一口都没有时，同一面板写「没有厚度样本」。这两句不弹对话框。同一面板列出每口井的 D61 TVD、D62 TVD、间隔速度或原因。像元中心落在这些井的分层点凸包之外时为 nodata -9999。现有 `MappingWorkflow::runThicknessChain` 用井点 TVD 差做 IDW，再调用相多边形。本计划的等厚不走这条路径，也不调用 `deriveFaciesPolygons`。D62 与 D61 的尺寸、geotransform 或 nodata 不一致就不写等厚。dt_ms 小于或等于 0，或 TVD 差不是正数，这口井不提供样本。控制点的凸包如果不是有面积的多边形，按不足 3 口处理，不写栅格。只有层位栅格缺失时，才退回对井点厚度本身做 IDW。正常图层名是「D61–D62 等厚（米）」。值是米，名称里不再写时间。这条退回才用「井点厚度（米，无层位栅格）」。砂地比、距井距离、屏障距离本计划不做。
- 厚度栅格不是相编码。`paleo:paleo_facies_polygonize` 只吃整数相编码栅格，空值为 -9999。本文件夹没有这样的栅格，参考 GeoJSON 也不在这个网格上，所以不从厚度栅格造相，也不新设厚度分档。已有的优先级融合只接受已经带相编码的栅格，输入顺序就是优先级。两张或以上才融合，一张时直接多边形化，一张都没有时，编图页写「没有相编码栅格，这一工区不从厚度生成相」，并且不调用融合。这不是第 6 节排除的相序规则融合。
- 验证项是井上 D61 时间（由 TD 表得到）减去 D61 栅格在井位处的时间。残差保留符号。绝对值大于 10 ms 成为一条问题。没有 D61 分层的井写「无 D61 分层」，仍然占一行。没有算出时间的井列出原因，不算一条数值残差。绝对值等于 10 ms 记为通过。采样用包含该点的像元，左闭右开。x 等于 12793 算最后一列，y 等于 0 算最后一行。落在这个像元矩形之外列出「井位不在测网内」，不算数值残差。该像元是 nodata 时列出「井位落在空道」，不算数值残差。验证页已有的问题表，在这次 D61 残差里使用列：井名、残差或原因、阈值。20 口井都在，不只有超限的井。标签用 DESIGN.md 的状态色，并且带字：通过用 #43A047，超过阈值用 #F29900，未计算用 #5D6E80。表头计数「n 口超过 10 ms」。还没跑时表为空，面板写「还没有计算 D61 残差」，旁边是现有的「运行验证」。「井位落在空道」和「井位不在测网内」是警告行，不是数值 0。双击留在验证页，不新建预览标签栏。这一行已经能看见井名、带符号的残差或原因、10 ms 阈值。共享地图移到该井。底部 dock 先切到已有的「连井剖面」，再滚到该分层。按钮「在数据页看这条剖面」才离开验证页：切到数据管理，聚焦数据页地震标签，并按阶段 B 解码或滚到这一条。验证页不新增标签。
- 「导出 D61 图件」用现有布局填一张模板再写 PDF，不把用户放在空白版面上。图上有标题「D61 厚度」、井名、米制图例、不画 nodata、比例尺、指北针，以及「工程坐标 · 米 · 未投影」。成功和失败都用对话框。成功写出路径和 SHA-256。失败写「导出失败」、原因和「重试」。图内仍含井位和厚度栅格。相多边形进 PDF 只在 `paleo:paleo_facies_polygonize` 已经跑过相编码栅格之后。`paleo.json` 里的 mock 多边形和参考 GeoJSON 不进 PDF。

完成标准：凡有可用 D61 时间的井都有一条时间残差；其中一口井走完「问题 → 地图、连井、地震剖面」；PDF 已导出。相多边形出现在 PDF 里只在相编码栅格已经多边形化之后。

### 阶段 D — 辅助资料

- PNG、PPTX、PDF 作为 `document` 或 `image_reference` 受管入库，角色 `reference`，挂到辅助实体。在数据页打开后各自成为一个标签：图片直接显示，文档用 QDesktopServices 打开本地文件 URL，不拼 shell。
- 三份 GeoJSON 入库并标明未配准，不生成地图图层。标签里能看到要素个数和相名。
- 文档、未配准图片、未配准 GeoJSON，以及 `参考资料/` 里 HZ28-6-1 的 XML，都挂到辅助实体，关联角色是 `reference`。这份 XML 不按内容去挂井，也不并进 A1–A20。其他 XML 用 QXmlStreamReader 区分井口或测井，不解析 DTD，不取外部实体。
- 完成后，地图上的井和 D61 位置与阶段 A 相同。

### 阶段 E — 八个层位，然后才是版本状态机

- 编图 chip 只有 C3、C6、D53、D61、D62、D63、D71、D72。切换沿用已有的按层位懒加载。
- 井分层里其余名字只出现在连井。
- 8 个层序界面的 chip 切换属于本阶段。编图页现有的发布按钮走 `MapVersionStore`，不走底部 `ReleaseStore`。只在这一条门上记下 D61 的 PDF 资产 id、SHA-256，以及 20 口井的残差或原因。`ReleaseStore` 仍只做清单快照，不成为第二道发布门。导出登记一个 OUTPUT 资产，不改已经冻结的版本行。下一次保存把该资产 id 和 SHA-256 抄进新的未发布版本。发布只读这一行。旧的 `map_versions` 和 `releases` 行读成未发布，不回写。缺 PDF 时 tooltip 写「导出 PDF 后再保存」。保存和发布是两个按钮。发布另开确认，列出版本、PDF 名和井数。每口井都有残差或原因，并且这份 PDF 已经在快照里，发布才可点。缺任何一条时按钮禁用。tooltip 写缺的口数和下一步：先在验证页运行验证，或先导出 PDF。缺哪些井以验证表为准。八个 chip 不挡住发布。没有栅格的 chip 禁用，tooltip 是「还没有这个层位的栅格」。已发布的行标「已发布」，不更新。下一次保存标成新版本，不写成在改这一行。不单独再开一段产品。

## 6. 本计划不做

- 把参考 GeoJSON 或扫描相图配准到局部测网。
- 地震体渲染、任意测线、三维相机。`geo3d_workspace.json` 不迁。
- 体系域 LST/TST/HST。
- 砂地比、距井距离、TIN、等值线、屏障 IDW、相序规则融合、多 realization、相界地质类型、暗色模式。
- 把 SEG-Y 全本或层位点文本提交进本仓库。
- `paleo-merged-main` 的数据管理界面。

## 7. 第一段实现

开发入口沿用 README：`./paleo-dev build`，测试 `./paleo-dev test`（offscreen ctest）。catalog.json 在工程目录的 `artifacts/metadata/catalog.json`。第一段夹具用已有的 `testdata/project_area/`，CMake 变量是 `PROJECT_FIXTURE_DIR`，不读 966 MB 的体。默认测线取 D61 分层点所在的 inline；分层点没有坐标时用井口。夹具打印这个整数。打开 SEG-Y 时新增内存要小于一条剖面加索引，不随文件大小线性增长。第一段沿用已有的 `tst_import`、`tst_segy`、`tst_segy_lines`、`tst_datapreview`。残差、厚度、发布和 ONNX 分属后面的阶段，不挡第一段。

阶段 A 的最小切片和阶段 B 的一条剖面一起做：

1. 局部测网 CRS，不写 4326。
2. A1 的井实体，以及井口、LAS、分层、时深四条主关联和只读 RAW。
3. D61 层位实体、其 RAW，以及父版本指向该 RAW 的时间栅格。
4. `200P_seismic.sgy` 外部链接，抽出一条 inline。
5. 用 A1 的 TD 表把 D61 标到这条剖面上。
6. 数据页预览标签栏：至少能同时打开 A1 的 GR、A1 的时深和这一条地震剖面，三个标签互不覆盖。阶段 A 的井口、分层和 D61 三个标签同时仍在，地震标签是第六个。

阶段 C 的残差要等这三样对齐之后再算。

测试：分类、单井匹配、两候选未决、多井分层文件产生多条关联、D61 装箱的 geotransform 和 nodata、复制 LAS 的 SHA-256、缺失外部路径、再次点选聚焦已有标签、打开 SEG-Y 时内存不随文件大小线性增长、图层 authid 为空。为本工区写出的 ONNX 栅格使用同一套 D61 geotransform，且不是 1×1，这是行为约束的测试，不是第一段交付物。井口恰好匹配一口已有井时不新建井。相同 SHA-256 再次导入不增加第二条主关联。TD 超出范围标「超出时深表」且不外推。时深表无序标「时深表无序」。缺 D62 的井不提供厚度样本。LAS 的 -99999 不绘制。失败文案是「读取失败」。`A1.Las` 能分类为测井。道头范围不对时不解码。加载文案含「正在读取」和文件名。相同 SHA-256 再次导入不新建版本，只允许补上已能匹配的未决关联。不足两个 TD 样点标「无时深表」。残差用包含该点的像元，空道不是数值残差，绝对值大于 10 ms 才成为问题。手工：A1 在 (5288.67, 8219.94)，容差半个像元，压在 D61 栅格上。界面：文件夹确认只打开井口标签；未决行显示「未决」；多井标签在选井前显示「先选择一口井」，并且选择另一条资产不改这个井；发布在缺残差或缺 PDF 时禁用，tooltip 写出原因。

<!-- autoplan-accepted:ceo -->
- Work coordinates are a local meter grid. EPSG:4326 stays a label and is not written onto map layers. While `coordinate_status=untransformed`, the map reads `surface_x/y` and does not write `project_x/y`. Layer, project, and canvas CRS are an engineering meter CRS with an empty authid and no transform to EPSG:4326. Do not use +proj=eqc.
- Import classifies by the `libs/ingest` path rules, parses metadata, resolves the entity, creates a well only from a new well-head name, creates a horizon or survey only when that name is new, writes an explicit `entity_asset_link`, and stores a read-only RAW with SHA-256. The 966 MB SEG-Y is an external link. File extensions are matched without case, so `A1.Las` is a well log. An ungeoreferenced `.tif` is `image_reference`. A derived GeoTIFF stays a map raster.
- Well names are compared after trim, dropping hyphens and spaces, and case-folding. This work area well-head file has no UWI column, so the match key is the normalized well name. This plan does not add UWI or alias columns. Well-head rows are the only creator of well entities. A normalized well-head name with no existing well creates one well and a `well_head` link. Two well-head rows with the same normalized name, or one row that matches two existing wells, stay unresolved and do not merge. LAS, tops, and TD never create a well. LAS reads the `~W` WELL mnemonic first. Tops and TD read the well-name column first. Zero matches then try the filename stem (`A1.Las` to A1). Zero after that, or two existing matches, keeps the asset, marks one link unresolved, leaves the entity id empty, creates no well, and does not merge. Two candidates are named in the link note. A well-head row that matches exactly one existing well links `well_head` and does not create another well. A SHA-256 that is already stored does not create a version. It may attach one missing primary link when an unresolved link now matches one well. It does not merge two wells. The UI says the bytes are already stored and whether a link was attached. A new SHA-256 appends an immutable version and that version becomes the only primary link for that role. `ExportWellHead.dat` and `DC.dat` are each one asset with one link per well name. Preview tabs filter that asset by the selected well. A horizon filename in {C3,C6,D53,D61,D62,D63,D71,D72} binds to that boundary; any other name is an unresolved horizon and stays off the map chips.
- D61.dat becomes a RAW whose child is a 411 by 641 time raster. P1 (inline 1315, crossline 4165) is (0, 0). P2 (1315, 4805) is (12793, 0). P3 (1725, 4805) is (12793, 16406). dx is 12793/640 m and dy is 16406/410 m. The north-up geotransform origin is x=0, y=16406, pixel width +dx, pixel height -dy. Empty bins are nodata -9999. Duplicate bins keep the last sample and record the collision count. ONNX rasters for this work area use this same geotransform. A1 at (5288.67, 8219.94) falls inside the grid within half a cell.
- Time ties interpolate TIME(ms) linearly from the top TVD against the TD table TVD column. If that TVD is empty, use MD against the TD MD column. The sentinel -99999 never enters the interpolation. A well with no TD table is posted with the well name and 「无时深表」. This work area does not invent a constant velocity, including the fallback named in PALEO_QGIS_PLAN. Section position, the nearest horizon sample, and the residual use the top X,Y. Fall back to the wellhead only when the top has no coordinates. The default first section is A1 inline. A depth outside the usable TD samples is not extrapolated. That top is marked 「超出时深表」 and does not enter the residual. If the depth column used for that lookup is not strictly increasing, the well is marked 「时深表无序」 and is not interpolated. If fewer than two finite samples remain on that column, the well is marked 「无时深表」 and is not interpolated.
- `catalog.json` is the only lifecycle store and the query source for this plan. The managed path is `{stage}/{asset_id}/{version_id}/{filename}` where stage is RAW, DERIVED, INTERMEDIATE, or OUTPUT. `catalog.sqlite` is not implemented in this plan. TODOS.md holds the rebuild trigger: after catalog.json roundtrips and list queries get slow.
- Data-page preview is a closable QTabWidget under the map on the data page only, not the workflow tab bar. Re-selecting an open asset focuses its tab. The filter well changes only when the selected asset link resolves to exactly one well. Otherwise an open multi-well tab keeps its filter. Loading copy is 「正在读取」 plus the filename. Failure copy is 「读取失败」 plus the reason plus the filename. LAS samples equal to -99999 are gaps and are not drawn. Any missing external path shows 「找不到源文件」 plus the path. Empty copy is 「还没有打开的预览 — 在列表中选择一条数据」. Import confirm stays inside the existing wizard in PALEO_QGIS_PLAN §42.7. SMI columns are shown and are not rearranged. The CRS step shows the engineering meter CRS, does not offer EPSG:4326, writes an empty authid, and cannot switch to a CRS that transforms into degrees.
- Phase A acceptance includes five open tabs for A1 well head, LAS (GR visible), tops (Time blank), TD curve, and D61 (grid stats plus show-on-map). The first delivery decodes one seismic line. That first delivery keeps the five phase A tabs and adds the seismic tab as a sixth.
- Phase B decodes one inline or crossline only. Opening the SEG-Y does not load the whole 966 MB file. A1 D61 is posted in time on A1 inline using the TVD interpolation above.
- Phase C thickness is the D62 time raster minus the D61 time raster, scaled to meters by power-2 IDW of per-well interval velocity from TVD pairs only. MD thickness is not mixed in. Sampling is at D61 cell centers, not the default expanded grid, and no constraint lines are passed. Fewer than three wells shows 「厚度样本不足以成面」. Zero wells shows 「没有厚度样本」. Outside the hull of those tops is nodata -9999. Well-only IDW of thickness is the fallback only when a horizon raster is missing. The thickness raster is not a facies code. paleo:paleo_facies_polygonize receives only an integer facies raster with nodata -9999. This folder has no such raster, and the reference GeoJSON is not on this grid, so thickness is not classified into facies and no thickness breaks are invented. Existing priority-stack fusion accepts only rasters that already carry facies codes. Two or more are fused in input order. One is polygonized directly. Zero shows 「没有相编码栅格」 and does not call fusion. That is separate from the excluded facies-order rule table. The phase C PDF contains the wells and the thickness raster. Facies polygons are added only after paleo:paleo_facies_polygonize has run on a coded facies raster. Mock polygons in paleo.json and the reference GeoJSON stay off the PDF. The residual is computed D61 time minus the time at the D61 cell whose center is nearest the top. The signed value is kept. Absolute value greater than 10 ms opens an issue. A nodata cell is listed as 「井位落在空道」 and is not a numeric residual. Issue fields are well name, signed residual in ms or the reason, and the 10 ms threshold. The click stays on the validation page, moves the shared map, and scrolls the existing correlation panel. It scrolls the seismic view only when that view is already open. Otherwise that one line is decoded into the existing data-page seismic tab. No QTabWidget is created on the validation page.
- Documents, ungeoreferenced images, ungeoreferenced GeoJSON, and the HZ28-6-1 XML under 参考资料 use link role `reference` on an auxiliary entity. That XML is never linked as a well. Other XML is still classified by content.
- Phase E keeps the eight horizon chips C3, C6, D53, D61, D62, D63, D71, and D72. Versions use the existing ReleaseStore. Save calls createRelease and the snapshot is immutable. ReleaseStore gains a published flag and stores the D61 PDF asset id and SHA-256. Publish requires that PDF and a residual row or a reason for every well. The eight chips do not gate publish. A published row is not updated. The next save is a new release. Phases C, D, and E stay in this plan and are sequenced after the first slice.
- Verification: QTest for classify, one-match bind, two-candidate unresolved, multi-well tops file yielding many links, D61 geotransform and nodata, checksum of a copied LAS, missing external path on any type, tab focus-on-reselect, SEG-Y open not loading the whole file, layer authid is empty, well-head one-match does not create a well, same SHA-256 re-import does not add a second primary link, TD outside range posts 「超出时深表」 and does not extrapolate, a non-monotonic TD posts 「时深表无序」, a well missing D62 contributes no thickness sample, LAS -99999 is not drawn, and failure copy is 「读取失败」. An ONNX raster written for this work area uses the D61 geotransform and is not 1 by 1. That ONNX check is a behavior constraint, not a first-delivery deliverable. Manual check: A1 at (5288.67, 8219.94) within half a cell on the D61 raster.
<!-- /autoplan-accepted:ceo -->

<!-- autoplan-accepted:design -->
- The data-page preview QTabWidget moves from the right dock to a vertical splitter under the map, on the data page only. With no tab open it is one muted line in #5D6E80 at 9pt: 「还没有打开的预览 — 在列表中选择一条数据」. The first tab gives the preview about one third of the center column, and the user can drag the splitter. Predict, constraint, mapping, and validation do not show it. The bottom dock stays 日志, 任务, and 连井剖面. The only seismic viewer is the data-page preview tab.
- 「导入工区文件夹」 sits beside the existing single-file import buttons. The folder step shows a classification table of path, type, entity, and 未决 or 失败, then counts, then confirm. Confirm reads 「入库 n，未决 n，失败 n」 and opens only the well-head tab. A single-file import opens only that asset's tab. The same SHA-256 says 「字节已在库」 and either 「已补上关联」 or 「没有新的关联」, then focuses the existing tab.
- Asset columns are 名称, 类型, 关联. 关联 shows the well name, a warning tag 「未决」 with tooltip of both candidate names, or 「参考」. There is no merge action. 「挂到这口井」 enables only after the user picks one existing well.
- Each multi-well preview tab has its own 井 combo and does not rewrite another tab. With no well selected the body is 「先选择一口井」. The title includes the filename and the well.
- Tab titles stay the asset name. The body shows 「正在读取」 plus the filename, 「读取失败」 plus the reason plus the filename and a 「重试」 button, or 「找不到源文件」 plus the path. Task progress uses the bottom 任务 tab: a bar after 1 second and an estimate after 10 seconds.
- The status bar shows 「工程坐标 · 米 · 未投影」 in #5D6E80. The wizard CRS step is that sentence, not a CRS picker, and the same sentence is printed on the D61 PDF.
- The seismic tab has a 纵测线 / 横测线 control and a spin box clamped to inline 1315–1725 or crossline 4165–4805. The initial line is A1 inline, captioned 「A1 所在测线」. During indexing the control is disabled with tooltip 「正在建立道索引」. Changing line clears the image first. The tie shows 「A1 D61」 plus milliseconds, or 「无时深表」, 「超出时深表」, or 「时深表无序」, and does not invent a time. The horizon tab shows rejected-point and collision counts. 「在地图上显示」 zooms, flashes, and becomes 「已在地图上」. Hiding stays on the layer tree.
- LAS uses a curve combo. A missing curve is disabled with 「这条曲线不在文件里」. An all-null curve says 「这条曲线没有有效样点」 and draws nothing. Numeric columns use JetBrains Mono 9pt, right-aligned. Tops include X and Y. An empty time cell stays empty.
- Ungeoreferenced images, documents, and the HZ28 XML say 「未配准，不加入地图」. That XML also says 「不对应 A1–A20」. GeoJSON is captioned 「经纬度，与本测网不是同一空间」. A document the desktop cannot open says 「系统没有打开这个文件」 plus the reason.
- The constraint panel lists each well's D61 TVD, D62 TVD, and interval velocity or the reason, and shows 「厚度样本不足以成面」 or 「没有厚度样本」 there. The layer title is 「D61–D62 时间等厚（米）」, or 「井点厚度（米，无层位栅格）」 only for the missing-raster fallback. The mapping page says 「没有相编码栅格，这一工区不从厚度生成相」 when no coded raster exists.
- The validation table lists all 20 wells with name, signed residual or reason, and the 10 ms threshold. Tags are 通过, 超过阈值, and 未计算, and the header count is 「n 口超过 10 ms」. Before the run the panel says 「还没有计算 D61 残差」. Off-survey and nodata rows are not numeric zeros. The click stays on 验证, moves the shared map, selects the existing 连井剖面 bottom-dock tab, and scrolls it. 「在数据页看这条剖面」 switches to 数据管理 and focuses the data-page seismic tab for that one line. No preview tab bar is added on 验证.
- 「导出 D61 图件」 fills one layout: title 「D61 厚度」, well labels, a meter legend, nodata not drawn, scale bar, north arrow, and the CRS sentence. Success shows the path and SHA-256. Failure shows 「导出失败」 and the reason. Publish is a separate confirm. It stays disabled, with a tooltip naming the missing well or the missing PDF, until every well has a residual or a reason and the PDF is in the snapshot. A published row is tagged 「已发布」. The next save is labeled as a new version.
- Horizon chips with no raster stay disabled with tooltip 「这一阶段还没有这个层位的栅格」. A prediction tensor that is not 411×641 shows 「结果不是 411×641，没有写入栅格」 and writes no raster.
- Keyboard focus is a 2px #1B73D0 ring. Disabled controls carry a reason tooltip. The asset table, preview tabs, well combo, and validation table have accessible names. The window minimum stays 1280×800. This plan does not add a phone layout.
- Verify: folder confirm opens only the well-head tab; an unresolved row shows 「未决」; a multi-well tab shows 「先选择一口井」 until a well is chosen and does not change when another asset is selected; Publish stays disabled, with the reason in the tooltip, when a residual or the PDF is missing.
<!-- /autoplan-accepted:design -->

<!-- autoplan-accepted:dx -->
- Where the Chinese sections and this block differ from earlier English bullets on validation navigation, the multi-well filter, and tab titles, follow the Chinese sections and this block. A double-click on 验证 stays on that page, moves the shared map, selects 连井剖面, and scrolls to that top. It does not decode seismic. 「在数据页看这条剖面」 is the only control that opens the data-page seismic tab.
- The preview tab title is the filename plus the well or the line, for example 「DC.dat · A1」 or 「200P · IL1315」. The LAS curve name stays out of the title. Status text stays in the body.
- A thickness sample requires both D61 and D62 TVD values and a numeric TD tie for both tops. dt_ms is D62 time minus D61 time in two-way milliseconds. Vint in m/s is (TVD_D62 - TVD_D61) / (dt_ms / 2000). Thickness in meters is isochron_ms / 2000 times the IDW of Vint. The test computes the expected meters from a known pair. The layer title is 「D61–D62 等厚（米）」.
- Folder confirm counts only three outcomes. 失败 means the file was not stored. 未决 means the asset is stored and the entity id is empty, with tooltip 无匹配, 两个候选, or 井口重名. 入库 means a primary link was written. The user may change a row's type before confirm. XML under 参考资料 defaults to reference unless that type is changed.
- 「挂到这口井」 uses a well combo on the unresolved row, empty by default, and confirms the asset name and the well name. Undo clears that link back to unresolved and does not delete a well that other links still use. An older immutable version can become the primary without copying bytes.
- The releases table adds nullable published INTEGER default 0, pdf_asset_id TEXT, and pdf_sha256 TEXT. Old rows load as unpublished and are not rewritten. Export success and failure both use a dialog: path plus SHA-256, or 「导出失败」, the cause, and 「重试」.
- The D61 geotransform stays (0, dx, 0, 16406, 0, -dy). That origin is the outer corner of the upper-left pixel. Residuals and IDW sample cell centers, half a pixel inside the node. This plan does not move the origin.
- Developer commands stay ./paleo-dev build and ./paleo-dev test. catalog.json is artifacts/metadata/catalog.json under the project directory. The managed directory segment is the lowercased stage. The first-slice fixture is the existing testdata/project_area directory, not the 966 MB volume. Residual, thickness, publish, and ONNX checks do not gate tst_import, tst_segy, tst_segy_lines, or tst_datapreview. The default line is the inline of the D61 top, else the wellhead, and the fixture prints that integer.
- Line spin boxes use the ranges frozen on the survey at index time. For this file those ranges are inline 1315–1725 and crossline 4165–4805. A header mismatch shows expected and actual and does not scan other bytes. This plan has no byte-map editor.
- A disabled Publish tooltip states the missing count and the next action: run validation, or export the PDF. The well names stay on the validation table. An ONNX shape failure includes the actual dimensions. Where PALEO_QGIS_PLAN §42.7 shows a CRS picker, this plan's CRS sentence and empty authid replace it.
- Verify: the thickness test uses the formula above on a synthetic pair; folder confirm counts match the three definitions; a multi-well tab does not follow another asset; double-click does not leave 验证; the seismic button does; old release rows read back unpublished.
<!-- /autoplan-accepted:dx -->

<!-- autoplan-accepted:eng -->
- Replace DataCatalog::localGridCrsProj. The engineering CRS has metre axes, no geodetic datum, an empty authid, and no QgsCoordinateTransform to EPSG:4326. Delete the +proj=eqc string in the same change.
- Delete SegyReader::open's fallback from header offset 188 to offsets 8 and 20. Inline for this file stays 1315 plus trace index over 641. Corners are the integer metres at offsets 180 and 184. A mismatch shows expected and actual and does not scan other bytes.
- TimeDepthTool::interpolateTimeMs keeps file order, does not sort, and does not clamp outside the range. The section post and the residual share one result: a value, 无时深表, too few samples, 时深表无序, or 超出时深表. Update tst_timedeptool in the same change.
- MappingWorkflow::runThicknessChain is not the D61 thickness path. Do not call deriveFaciesPolygons on that raster. Refuse the isochron when D62 does not match D61's dimensions, geotransform, and nodata. A non-positive dt_ms or TVD difference contributes no sample. A hull that is not a polygon with area uses the same panel sentence as fewer than three wells and writes no raster.
- Residual sampling uses the containing cell with half-open bounds. x equal to 12793 is the last column and y equal to 0 is the last row. Outside that rectangle is 「井位不在测网内」. Absolute residual equal to 10 ms is 通过. A well with no D61 top is a row 「无 D61 分层」.
- DataCatalog::addLink accepts an empty entity id and stores a note. 「挂到这口井」 fills that id and clears the note in one save. Folder confirm links well-head rows before logs, tops, and TD, so A1 still gets four primary links when the LAS path is listed first. Strip U+FEFF before the well-head header. HZ28-6-1's type cannot be changed.
- The mapping-page publish button stays on MapVersionStore. ReleaseStore remains a manifest snapshot and is not a second gate. Export registers an OUTPUT asset and does not edit a frozen version. The next save copies the PDF asset id and SHA-256 into a new unpublished version. Publish reads that row, which also holds a residual or a reason for every well. Old rows load unpublished.
- The external SEG-Y stores a streamed SHA-256. A mismatch shows 「源文件与入库时的 SHA-256 不一致」 and does not decode. Path segments reject control characters. Folder import reads regular files, does not follow a symlink outside the chosen root, and one bad file does not stop the walk.
- The preview splitter hides when the user leaves the data page. Validation double-click does not call the bottom-dock seismic panel. Existing publish and attribute bottom tabs stay. The unresolved tag uses #24303E on #FFF4E0 with a #F29900 border.
- Verify in the phase that changes the behavior, not inside tst_import, tst_segy, tst_segy_lines, or tst_datapreview: LAS-before-well-head still binds A1; TD is not clamped; containing-cell corners; dt_ms <= 0; D62 grid mismatch; polygonize is not called from thickness; engineering CRS has no transform to EPSG:4326; the offset-8 fallback does not run; old version rows stay unpublished.
<!-- /autoplan-accepted:eng -->
## Review record
<!-- autoplan-baseline-edits:design {"sourceSha256":"73d5808154bacb5edd851334cc5d03458756e2d980a1639c83a45dc754dee30d","replacements":[]} -->
<!-- autoplan-baseline-edits:dx {"sourceSha256":"73d5808154bacb5edd851334cc5d03458756e2d980a1639c83a45dc754dee30d","replacements":[]} -->
<!-- autoplan-baseline-edits:eng {"sourceSha256":"73d5808154bacb5edd851334cc5d03458756e2d980a1639c83a45dc754dee30d","replacements":[]} -->
<!-- autoplan-baseline-edits:ceo {"sourceSha256":"73d5808154bacb5edd851334cc5d03458756e2d980a1639c83a45dc754dee30d","replacements":[]} -->
<!-- pass1-baseline-edits-ceo {"sourceSha256":"330407813dc71e3f6e877663b8e7e4fe02398d8aca5238f6e3fb849495b44a4f","replacements":[{"oldText":"# project_area 开发计划\n\n日期：2026-09-25。\n\n验收数据是 `/home/kevin/projects/paleo_project/data/project_area`（约 1.4 GB）。目标是用这套工区走通一层古地理编图：井、层位、地震在同一局部坐标里对齐，D61 能编成可编辑的相多边形并导出。\n\n架构仍以 `docs/PALEO_QGIS_PLAN.md` 为准：QGIS 负责渲染、图层、CRS、编辑和布局，Paleo 负责地质对象和导入。界面仍是现有五页壳，视觉以 `DESIGN.md` 为准。\n\n多源数据管理只取 `paleo-merged-main` 的对象和导入规则（ADR 0056 资产目录、ADR 0059 工区—实体—资产、`libs/ingest` 的分类器与 SMI 井分层解析）。不迁移它的导航树、功能区、井位散点页和概览面板。\n\n编图目标层位是数据里的 **D61**。C6 的层位点是齐的，井分层里只有 12/20 口井有 C6。\n\n## 1. 数据事实\n\n工作坐标是局部直角、单位米，范围大约 `x 0–12800`、`y 0–16400`。层位文件头写明 `Projection: Local Rectangular`、`Units: meters`。`project_area.paleo.json` 把 CRS 标成 `EPSG:4326`，井的 `coordinate_status` 是 `untransformed`。这套数不能按经纬度绘制。\n\n| 来源 | 规模 | 对开发的含义 |\n|---|---|---|\n| `井位/ExportWellHead.dat` | 20 口，A1–A20 | 井名、X、Y、KB、TD。A1 在 (5288.67, 8219.94) |\n| `井曲线/*.Las` | 20 个，各约 2 MB | LAS 2.0。曲线 DEPT、AC、DEN、GR 及 `_S`。NULL 为 -99999 |\n| `井分层/DC.dat` | 516 行 | 井名、层名、MD、TVD。Time(ms) 全部是 -99999 |\n| `时深/TD/*.dat` | 20 口井 | TIME(ms)、TVDSS、TVD、MD。depth↔TWT 用这张表 |\n| `层位/*.dat` | 8 个，各约 26.3 万点 | x、y、z(ms)、Inline、Crossline。网格 411×641。Inline 1315–1725，Crossline 4165–4805 |\n| 层序 | 8 个界面 | C3、C6、D53、D61、D62、D63、D71、D72。体系域字段是 LST/TST/HST，没有对应数据 |\n| `地震体/200P_seismic.sgy` | 966 MB，约 263451 道 | 三维体。901 样点，2 ms，IBM 浮点（format 1）。道数与层位网格一致 |\n| `参考相图/*.geojson` | 相 50、亚相 188、微相 397 | 经纬度约 105–125°E、20–40°N，`period=J3`。与局部测网不是同一空间 |\n| `参考资料/` | PNG、PPTX、PDF、XML | 扫描相图、构造图、单井图、编图规范、HZ28-6-1 柱状图。没有地理配准 |\n\n`paleo.json` 里有一条 D61 的 mock 预测，多边形用的是局部米坐标。参考 GeoJSON 不能叠到这张图上。\n\n## 2. 已有代码里要改的行为\n\n下面这些已经写进本仓库，接到 `project_area` 时会错。改这些行为，不另起产品。\n\n| 现况 | 改成 |\n|---|---|\n| 工程可能把 JSON 里的 EPSG:4326 写成图层 CRS | `coordinate` 仍是 CRS 权威。工作坐标记为局部测网、米。4326 只留作标签，不参与绘制 |\n| `DataImportService` 把非 tif/img 都声明成矢量 | 按第 3 节的分类、实体和受管复制导入。`.dat` 不再交给 OGR |\n| `SegyReader::open` 对文件 `readAll()`，样本全部进内存 | 按道偏移索引。只解码一条 inline 或 crossline。补上 crossline 道头（默认字节 193） |\n| 层位若变成点要素，一张图 26 万个点 | 按文件头的 411×641 网格装箱成时间栅格，空道为 nodata |\n| 时深转换若用常速 | 先用该井 TD 表。没有 TD 的井才用常速，并在剖面上标明 |\n| ONNX 结果可以落成 1×1 栅格 | 接到本工区时，范围是测网 `0–12793 × 0–16406`，层位是 D61 |\n| 参考 GeoJSON 若按矢量图层打开 | 入库为未配准辅助资产，不生成地图图层 |\n\nLAS 2.0 解析和 SEG-Y 的 IBM 浮点解码保留。相多边形算法 `paleo:paleo_facies_polygonize`、连井面板、布局导出保留，本计划只给它们接上这套数。\n\n## 3. 数据管理契约\n\n对象链：\n\n```\n工区\n └─ 地质实体或辅助实体\n      └─ 显式关联（角色、是否主版本、是否未决）\n           └─ 数据资产\n                └─ 不可变版本（RAW / DERIVED / INTERMEDIATE / OUTPUT）\n```\n\n文件不是井。一口井是稳定 id、井名、UWI 和别名。地震体是一条 `SeismicSurvey`，打开时从道头冻结角点、inline/crossline 范围、采样间隔和起始时间。每个层序界面是一个地质实体。扫描图、PPT、PDF 和未配准 GeoJSON 是辅助实体。\n\n关联写在 `entity_asset_links`，字段是实体类型、实体 id、资产 id、角色、是否主版本、是否未决。角色用已有名字：`well_head`、`well_log`、`tops`、`time_depth`、`horizon`、`seismic_volume`。关系不从标签推断。同名冲突不合并，链接标 `unresolved`。身份顺序是已有 id、UWI、规范化井名、别名。文件名不作身份。测井曲线先读 LAS `~W` 的 WELL。对得上已有井就挂上；对不上再用文件名主名（`A1.Las` → A1）。仍对不上就建未决链接，不新建一口同名井。时深和分层用文件里的井名列，规则相同。层位文件 `D61.dat` 挂到层序界面 D61；文件名不在 8 个层序界面里时，建未决层位实体，不进编图 chip。\n\n井同时保存原始 `surface_x/y` 和 `project_x/y`，以及 `coordinate_status`：`ok`、`untransformed`、`invalid`、`missing`。本工区 20 口井都是 `untransformed`。地图用局部坐标绘制，状态保持未变换，直到出现真正的投影参数。\n\n`catalog.json` 是资产生命周期的主存储。受管文件路径是 `{stage}/{asset_id}/{version_id}/{filename}`。默认导入是受管 RAW：边复制边算 SHA-256，落盘后只读。用户明确选择链接外部时不复制；966 MB 的 SEG-Y 走外部链接。由层位文件装箱得到的时间栅格是 DERIVED，父版本指向该 RAW。`catalog.json` 在 20 口井的规模上直接当查询源。`catalog.sqlite` 仍定义为可重建索引，但不进第一段实现；等资产数量或查询变慢再补。现有图层清单只登记要画进 QGIS 的结果，不兼任文件目录。\n\n分类沿用 `paleo-merged-main/libs/ingest/src/classifier.cpp`：\n\n| 路径或扩展名 | 类型 | 资产角色 |\n|---|---|---|\n| `井位/`，或文件名含 wellhead | `well_head` | input |\n| `井分层/` | `well_stratification` | input |\n| `时深/`，或路径段 `td` | `time_depth` | input |\n| `层位/` | `horizon` | input |\n| `.las` | `well_log` | input |\n| `.sgy` / `.segy` | `seismic` | input |\n| `.geojson` | `geojson` | input，未配准则不进地图 |\n| `.pdf` `.ppt` `.pptx` `.doc` `.docx` | `document` | reference |\n| `.png` `.jpg` `.tif` | `image_reference` | reference |\n| `.xml` | 再看内容 | 井口或测井；判不出则作参考 |\n\n井分层解析与 `parse_well_tops_text` 一致：`#` 行跳过，列是井名、层名、MD、X、Y、Z、TVD、Time(ms)。\n\n## 4. 数据页用标签页预览\n\n预览只出现在数据管理页，不泄漏到预测、约束、编图、验证。顶部工作流标签栏仍然是唯一的签名元素。预览用页内普通的 `QTabWidget`，样式走 `DESIGN.md` 的 dock 面板，不用工作流标签的蓝色下划线。\n\n布局：右侧仍是资产列表。地图留在中央。列表下方或地图下方放预览标签栏，只在数据管理页可见。从列表选中一条资产时，若已有同资产标签则切过去，否则新开一个可关闭标签。\n\n空态文案是「还没有打开的预览 — 在列表中选择一条数据」。复制或解析还在进行时，标签显示「正在读取」和文件名，不显示半份曲线。失败时标签内给出原因和文件名，不留白面板。外部链接的文件如果路径不存在，地震或文档标签写「找不到源文件」和那条路径。\n\n| 资产类型 | 标签里显示什么 |\n|---|---|\n| `well_log` | 单井曲线。默认 GR，可换 AC、DEN。用现有单道绘制，不把多井连井面板搬进这个标签 |\n| `well_stratification` | 该井的分层表：层名、MD、TVD。Time 列为空就显示空，不填假时间 |\n| `time_depth` | 该井的 TIME–TVD 曲线 |\n| `well_head` | 井名、X、Y、KB、TD、`coordinate_status`。选中时地图同时高亮该井 |\n| `horizon` | 网格尺寸、Z 的单位和范围、派生栅格是否已生成。提供「在地图上显示」。标签内不画 26 万个点 |\n| `seismic` | 现有地震预览。标签内选择一条 inline 或 crossline，只解码这一条 |\n| `image_reference` | 按面板宽度缩放的图片 |\n| `document` | 文件名、类型，以及「用系统程序打开」。这一阶段不做 PDF 内嵌翻页 |\n| `geojson` | 要素个数、坐标范围、相名字段。未配准时标明不加入地图 |\n\n导入向导最后一步的确认预览仍是向导里的一步。确认入库之后，才在数据页打开对应标签。\n\n## 5. 分阶段计划\n\n每段都用 `project_area` 里的文件验收。966 MB 的 SEG-Y 和 8 个层位点文本不提交进本仓库。测试夹具是从中切出的一条 inline、一口井的 LAS/TD，以及 D61 栅格。\n\n### 阶段 A — 实体、受管原文和预览标签\n\n导入 `project_area` 后：\n\n- A1 有四条主关联：井口、LAS、分层、时深。受管副本只读，SHA-256 与源文件一致。\n- D61 有层位关联。派生时间栅格登记到图层清单，能在地图上打开。\n- A1 落在 (5288.67, 8219.94) 附近，并压在 D61 栅格上。\n- 图层 CRS 不是 EPSG:4326。井的 `coordinate_status` 仍是 `untransformed`。\n- 在数据页依次打开 A1 的井口、LAS、分层、时深和 D61 层位，得到五个可关闭标签，来回切换不丢内容。LAS 标签能看到 GR。分层表里 Time 为空。D61 标签能把派生栅格显示到地图上。\n\n这一阶段不读地震道样本。地震资产可以出现在列表里，打开标签时说明剖面在下一阶段才可用。\n\n### 阶段 B — 一条地震剖面\n\n- SEG-Y 建立 inline/crossline 到文件偏移的索引，不把体读进内存。\n- 打开地震资产时使用数据页上的地震标签。标签内选择一条 inline 或 crossline，只解码这一条（约 411 或 641 道，901 样点，2 ms）。\n- A1 的 D61 分层用 TD 表换成毫秒，标到这条剖面上。没有 TD 的井用常速，并标注。\n- 验收夹具是这一条 inline，不是整个 `.sgy`。切换到 A1 的时深标签再切回地震标签，剖面仍在。\n\n### 阶段 C — 只编 D61\n\n- 结构面是 D61 时间栅格。\n- 单因素先算井上 D61 到 D62 的 TVD 厚度，再在测网网格上做现有的约束 IDW。IDW 仍按凸包裁剪。砂地比、距井距离、屏障距离不在本阶段。\n- 预测或融合栅格使用测网范围。相多边形走已有的 `paleo:paleo_facies_polygonize`。\n- 验证项是井上 D61 时间（由 TD 表得到）与 D61 栅格在井位处的差。超过阈值成为一条问题。点开后地图缩放到该井，连井滚到该分层，地震滚到对应测线和时间。\n- 布局导出一张含井位和相多边形的 PDF。\n\n完成标准：20 口井里凡有 D61 分层的井都有一条时间残差；其中一口井走完「问题 → 三视图」。\n\n### 阶段 D — 辅助资料\n\n- PNG、PPTX、PDF 作为 `document` 或 `image_reference` 受管入库，角色 `reference`，挂到辅助实体。在数据页打开后各自成为一个标签：图片直接显示，文档提供「用系统程序打开」。\n- 三份 GeoJSON 入库并标明未配准，不生成地图图层。标签里能看到要素个数和相名。相、亚相、微相名称收成图例字典。\n- HZ28-6-1 的 XML 不并进 A1–A20。打开它时标签标明这是参考资料，不写成 A1 的曲线。\n- 完成后，地图上的井和 D61 位置与阶段 A 相同。\n\n### 阶段 E — 八个层位，然后才是版本状态机\n\n- 编图 chip 只有 C3、C6、D53、D61、D62、D63、D71、D72。切换沿用已有的按层位懒加载。\n- 井分层里其余名字只出现在连井。\n- D61 能保存并导出之后，再实现「保存版本 / 发布 / Published」。在那之前不为状态机单开一段。\n\n## 6. 本计划不做\n\n- 把参考 GeoJSON 或扫描相图配准到局部测网。\n- 地震体渲染、任意测线、三维相机。`geo3d_workspace.json` 不迁。\n- 体系域 LST/TST/HST。\n- 砂地比、距井距离、TIN、等值线、屏障 IDW、相序规则融合、多 realization、相界地质类型、暗色模式。\n- 把 SEG-Y 全本或层位点文本提交进本仓库。\n- `paleo-merged-main` 的数据管理界面。\n\n## 7. 第一段实现\n\n阶段 A 的最小切片和阶段 B 的一条剖面一起做：\n\n1. 局部测网 CRS，不写 4326。\n2. A1 的井实体，以及井口、LAS、分层、时深四条主关联和只读 RAW。\n3. D61 层位实体、其 RAW，以及父版本指向该 RAW 的时间栅格。\n4. `200P_seismic.sgy` 外部链接，抽出一条 inline。\n5. 用 A1 的 TD 表把 D61 标到这条剖面上。\n6. 数据页预览标签栏：至少能同时打开 A1 的 GR、A1 的时深和这一条地震剖面，三个标签互不覆盖。\n\n阶段 C 的残差要等这三样对齐之后再算。\n","newText":"# project_area 开发计划\n\n日期：2026-09-25。\n\n验收数据是 `/home/kevin/projects/paleo_project/data/project_area`（约 1.4 GB）。目标是用这套工区走通一层古地理编图：井、层位、地震在同一局部坐标里对齐，D61 的厚度和井震残差能导出。相多边形只在已有相编码栅格时导出，不从厚度栅格生成。\n\n架构仍以 `docs/PALEO_QGIS_PLAN.md` 为准：QGIS 负责渲染、图层、CRS、编辑和布局，Paleo 负责地质对象和导入。界面仍是现有五页壳，视觉以 `DESIGN.md` 为准。\n\n多源数据管理只取 `paleo-merged-main` 的对象和导入规则（ADR 0056 资产目录、ADR 0059 工区—实体—资产、`libs/ingest` 的分类器与 SMI 井分层解析）。不迁移它的导航树、功能区、井位散点页和概览面板。\n\n编图目标层位是数据里的 **D61**。C6 的层位点是齐的，井分层里只有 12/20 口井有 C6。\n\n## 1. 数据事实\n\n工作坐标是局部直角、单位米，范围大约 `x 0–12800`、`y 0–16400`。层位文件头写明 `Projection: Local Rectangular`、`Units: meters`。`project_area.paleo.json` 把 CRS 标成 `EPSG:4326`，井的 `coordinate_status` 是 `untransformed`。这套数不能按经纬度绘制。\n\n| 来源 | 规模 | 对开发的含义 |\n|---|---|---|\n| `井位/ExportWellHead.dat` | 20 口，A1–A20 | 井名、X、Y、KB、TD。A1 在 (5288.67, 8219.94) |\n| `井曲线/*.Las` | 20 个，各约 2 MB | LAS 2.0。曲线 DEPT、AC、DEN、GR 及 `_S`。NULL 为 -99999 |\n| `井分层/DC.dat` | 516 行 | 表头是井名、层名、MD、X、Y、Z、TVD、Time(ms)。Time(ms) 全部是 -99999。缺 X、Y、Z 时忽略这三列，不把文件判为解析失败 |\n| `时深/TD/*.dat` | 20 口井 | TIME(ms)、TVDSS、TVD、MD。depth↔TWT 用这张表 |\n| `层位/*.dat` | 8 个，各约 26.3 万点 | x、y、z(ms)、Inline、Crossline。网格 411×641。Inline 1315–1725，Crossline 4165–4805 |\n| 层序 | 8 个界面 | C3、C6、D53、D61、D62、D63、D71、D72。体系域字段是 LST/TST/HST，没有对应数据 |\n| `地震体/200P_seismic.sgy` | 966 MB，约 263451 道 | 三维体。901 样点，2 ms，IBM 浮点（format 1）。道数与层位网格一致 |\n| `参考相图/*.geojson` | 相 50、亚相 188、微相 397 | 经纬度约 105–125°E、20–40°N，`period=J3`。与局部测网不是同一空间 |\n| `参考资料/` | PNG、PPTX、PDF、XML | 扫描相图、构造图、单井图、编图规范、HZ28-6-1 柱状图。没有地理配准 |\n\n`paleo.json` 里有一条 D61 的 mock 预测，多边形用的是局部米坐标。参考 GeoJSON 不能叠到这张图上。\n\n## 2. 已有代码里要改的行为\n\n下面这些已经写进本仓库，接到 `project_area` 时会错。改这些行为，不另起产品。\n\n| 现况 | 改成 |\n|---|---|\n| 工程可能把 JSON 里的 EPSG:4326 写成图层 CRS | 图层 CRS 是无基准工程坐标，单位米，authid 留空，不能反投到 EPSG:4326。JSON 里的 EPSG:4326 只留在源标签上 |\n| `DataImportService` 把非 tif/img 都声明成矢量 | 按第 3 节的分类、实体和受管复制导入。`.dat` 不再交给 OGR |\n| `SegyReader::open` 对文件 `readAll()`，样本全部进内存 | 按道偏移索引，不在界面线程上做。只解码一条 inline 或 crossline。本文件实测 1012709244 字节、901 样点、2 ms、format 1、道距 3844、263451 道。道头偏移 188 的 inline 和偏移 192（SEG-Y 1-based 字节 193）的 crossline 都是 0。CDP 在偏移 20，每 641 道从 4165 排到 4805。源坐标道 0 为 (0,0)，道 640 为 (12793,0)，末道为 (12793,16406)。索引：inline = 1315 + 道号/641，crossline = 该道 CDP。道数、CDP 顺序、角点有一项对不上就停止并显示读到的数，不改去扫别的字节 |\n| 层位若变成点要素，一张图 26 万个点 | 按文件头的 411×641 网格装箱成时间栅格，空道为 nodata |\n| 时深转换若用常速 | 只用该井 TD 表做线性插值。没有 TD 的井标「无时深表」，不使用常速 |\n| ONNX 结果可以落成 1×1 栅格 | 接到本工区时使用 D61 的 geotransform。挤成二维后不是 411×641 就失败，不写栅格 |\n| 参考 GeoJSON 若按矢量图层打开 | 入库为未配准辅助资产，不生成地图图层 |\n\nLAS 2.0 解析和 SEG-Y 的 IBM 浮点解码保留。扩展名忽略大小写，`A1.Las` 也按测井分类。连井面板和布局导出保留，并接到这套工区。相多边形算法保留，只接收相编码栅格，不接收厚度栅格。\n\n## 3. 数据管理契约\n\n对象链：\n\n```\n工区\n └─ 地质实体或辅助实体\n      └─ 显式关联（角色、是否主版本、是否未决）\n           └─ 数据资产\n                └─ 不可变版本（RAW / DERIVED / INTERMEDIATE / OUTPUT）\n```\n\n文件不是井。一口井在本计划里只存 id、规范化井名、surface_x/y、KB、TD 和 coordinate_status。不建 UWI 列，也不建别名列。地震体是一条 `SeismicSurvey`，打开时从道头冻结角点、inline/crossline 范围、采样间隔和起始时间。每个层序界面是一个地质实体。扫描图、PPT、PDF 和未配准 GeoJSON 是辅助实体。\n\n关联写在 `entity_asset_links`，字段是实体类型、实体 id、资产 id、角色、是否主版本、是否未决。角色用已有名字：`well_head`、`well_log`、`tops`、`time_depth`、`horizon`、`seismic_volume`、`reference`。表里的 input 和 reference 是资产角色，关联角色是这一列。关系不从标签推断。井名比较前去掉首尾空白、连字符和空格，并忽略大小写。井口文件带 UTF-8 BOM。列是 Name、X、Y、KB、TotalDepth、BottomX、BottomY、WellType。TotalDepth 记为 TD。BottomX、BottomY、WellType 留在井上。没有 UWI。匹配键是规范化井名。井口文件是建井来源：规范化后的井名还没有已有井，就新建一口井并挂 `well_head`。井口文件里同一个规范化名字出现两行，或这一行同时匹配两口已有井，该行标 `unresolved`，不新建，也不合并。测井、分层和时深不新建井。它们先用 LAS `~W` 的 WELL 或文件中的井名列匹配已有井；零个匹配再用文件名主名（`A1.Las` → A1）。仍然零个，或两个已有井都匹配，资产仍然保留，链接标 `unresolved`，实体 id 留空，不新建井，也不合并。两个候选时，链接备注写下两个规范化井名。井口行恰好匹配一口已有井时，挂上 `well_head`，不另建井。SHA-256 已经入库时不新建版本。若未决关联这时能按规范化井名挂上，可以补这一条主关联，不合并两口井。界面说明字节已在库，并说明这次有没有补上关联。SHA-256 是新的才追加不可变版本，该角色只保留这一条主关联。层位文件 `D61.dat` 挂到层序界面 D61。文件名不在 C3、C6、D53、D61、D62、D63、D71、D72 里时，建未决层位实体，不进编图 chip。\n\n`井位/ExportWellHead.dat` 和 `井分层/DC.dat` 各是一份多井文件，各登记为一个资产。每个井名一条关联，指向同一资产。预览标签按当前选中的井过滤，不把文件拆成 20 份。\n\n井保存 `surface_x/y` 和 `coordinate_status`：`ok`、`untransformed`、`invalid`、`missing`。本工区 20 口井都是 `untransformed`。在出现真正的投影参数之前不写 `project_x/y`，地图读 `surface_x/y`。图层 CRS、工程 CRS 和画布 CRS 都是无基准的工程坐标，单位米，authid 留空，不能反投到 EPSG:4326。不用 `+proj=eqc`，因为它会把局部米悄悄变成经纬度。\n\n`catalog.json` 是资产生命周期的唯一主存储，也是这一阶段的查询源。写入走工程写队列，先临时文件再改名。`asset_id`、`version_id`、`filename` 各只占一段路径，不允许斜杠和 `..`。受管路径是 `{stage}/{asset_id}/{version_id}/{filename}`，`stage` 就是 `RAW`、`DERIVED`、`INTERMEDIATE` 或 `OUTPUT`。默认导入是受管 RAW：边复制边算 SHA-256，落盘后只读。用户明确选择链接外部时不复制；966 MB 的 SEG-Y 走外部链接。由层位文件装箱得到的时间栅格是 DERIVED，父版本指向该 RAW。`catalog.sqlite` 不在本计划的实现里。触发条件写在 `TODOS.md` 的「P3 — catalog.sqlite 查询索引」：catalog.json 能往返并且列表查询变慢。现有图层清单只登记要画进 QGIS 的结果，不兼任文件目录。\n\n分类沿用 `paleo-merged-main/libs/ingest/src/classifier.cpp`：\n\n| 路径或扩展名 | 类型 | 资产角色 |\n|---|---|---|\n| `井位/`，或文件名含 wellhead | `well_head` | input |\n| `井分层/` | `well_stratification` | input |\n| `时深/`，或路径段 `td` | `time_depth` | input |\n| `层位/` | `horizon` | input |\n| `.las`，扩展名忽略大小写 | `well_log` | input |\n| `.sgy` / `.segy` | `seismic` | input |\n| `.geojson` | `geojson` | input，未配准则不进地图 |\n| `.pdf` `.ppt` `.pptx` `.doc` `.docx` | `document` | reference |\n| `.png` `.jpg`，以及没有地理变换的 `.tif` | `image_reference` | reference |\n| `.xml` | 再看内容 | 井口或测井；判不出则作参考。`参考资料/` 里 HZ28-6-1 的 XML 固定为辅助参考，不挂到 A1–A20 |\n\n井分层解析与 `parse_well_tops_text` 一致：`#` 行跳过，列是井名、层名、MD、X、Y、Z、TVD、Time(ms)。值为 -99999 的时间、TVD 或 MD 视为空，不参加计算。\n\nD61 栅格只使用层位文件头，不另设一套范围。网格 411×641。P1（inline 1315，crossline 4165）= (0, 0)，P2（1315，4805）= (12793, 0)，P3（1725，4805）= (12793, 16406)。X 随 crossline 增加，Y 随 inline 增加。像元 `dx = 12793/640` 米，`dy = 16406/410` 米。北向上的 geotransform 是原点 x=0、y=16406，列方向 `dx`，行方向 `-dy`。空道 nodata 为 -9999。列号 = crossline - 4165，行号 = 1725 - inline。越界的点不写入，只计入拒绝数。同一像元多点时按文件顺序保留最后一点，并在栅格元数据里记下碰撞次数。A1 的 D61 TVD 是 1935 m，D62 TVD 是 1971 m。A1 (5288.67, 8219.94) 落在这个网格内，验收用这个精确坐标，容差半个像元。\n\n时深转换只用 TD 表。分层的 TVD 对 TD 的 TVD 列线性插值得到 TIME(ms)。该井 TVD 为空时改用 MD 对 TD 的 MD 列。-99999 不参加插值。20 口井都有 TD 文件。若某口井没有 TD 表，剖面上标井名和「无时深表」，不使用常速。`docs/PALEO_QGIS_PLAN.md` 里缺省常速的写法不用于这套工区。深度落在 TD 表可用样点的范围之外时不外推，该层标「超出时深表」，不进入残差。用来查找的那一列不是严格递增，或去掉 -99999 之后不足两个有限样点时，这口井标「时深表无序」或「无时深表」：不是严格递增用前者，样点不足两个用后者。两种都不插值。\n\n井在剖面上的位置、最近层位采样和残差都用该层分层点的 X、Y。分层点没有坐标时才退回井口。井口和分层点相差超过一个像元时，剖面上标出这个偏移。第一段默认打开的测线是 A1 对应的那条 inline。\n\n## 4. 数据页用标签页预览\n\n预览只出现在数据管理页，不泄漏到预测、约束、编图、验证。顶部工作流标签栏仍然是唯一的签名元素。预览用页内普通的 `QTabWidget`，样式走 `DESIGN.md` 的 dock 面板，不用工作流标签的蓝色下划线。\n\n布局：右侧仍是资产列表。地图留在中央。预览标签栏放在地图下方，只在数据管理页可见。从列表选中一条资产时，若已有同资产标签则切过去，否则新开一个可关闭标签。多井文件的标签按当前井过滤。当前井只有在选中资产的关联恰好解析到一口井时才改变。否则已经打开的多井标签保持原过滤。\n\n空态文案是「还没有打开的预览 — 在列表中选择一条数据」。复制或解析还在进行时，标签显示「正在读取」和文件名，不显示半份曲线。索引、LAS 解析、层位装箱和单条解码不在界面线程上做。关掉标签就取消。离开数据页只隐藏预览，不拆掉。多井文件旁边有一口井的选择；只有选中资产恰好关联一口井时才自动改这口井。失败时标签写「读取失败」、原因和文件名，不留白面板。任何外部链接的源文件不存在时，该标签写「找不到源文件」和那条路径。\n\n导入确认仍用 `docs/PALEO_QGIS_PLAN.md` §42.7 的向导（文件、字段、CRS、预览、确认）。SMI 文本的列是固定的，向导展示识别出的列，不让用户重排。确认入库之后，才在数据页打开对应标签。这一阶段向导的 CRS 步展示无基准工程坐标，单位米，不提供 EPSG:4326，authid 留空，不能改成会反算到经纬度的 CRS。\n\n| 资产类型 | 标签里显示什么 |\n|---|---|\n| `well_log` | 单井曲线。默认 GR，可换 AC、DEN。用现有单道绘制，曲线值 -99999 是空样，不绘制。不把多井连井面板搬进这个标签 |\n| `well_stratification` | 该井的分层表：层名、MD、TVD。Time 列为空就显示空，不填假时间 |\n| `time_depth` | 该井的 TIME–TVD 曲线 |\n| `well_head` | 井名、X、Y、KB、TD、`coordinate_status`。选中时地图同时高亮该井 |\n| `horizon` | 网格尺寸、Z 的单位和范围、派生栅格是否已生成。提供「在地图上显示」。标签内不画 26 万个点 |\n| `seismic` | 现有地震预览。标签内选择一条 inline 或 crossline，只解码这一条 |\n| `image_reference` | 按面板宽度缩放的图片 |\n| `document` | 文件名、类型，以及用本地文件 URL 打开。这一阶段不做 PDF 内嵌翻页 |\n| `geojson` | 要素个数、坐标范围、相名字段。未配准时标明不加入地图 |\n\n## 5. 分阶段计划\n\n每段都用 `project_area` 里的文件验收。966 MB 的 SEG-Y 和 8 个层位点文本不提交进本仓库。测试夹具是从中切出的一条 inline、一口井的 LAS/TD，以及 D61 栅格。\n\n### 阶段 A — 实体、受管原文和预览标签\n\n导入 `project_area` 后：\n\n- A1 有四条主关联：井口、LAS、分层、时深。受管副本只读，SHA-256 与源文件一致。\n- D61 有层位关联。派生时间栅格登记到图层清单，能在地图上打开。\n- A1 落在 (5288.67, 8219.94)，容差半个像元，并压在 D61 栅格上。\n- 图层、工程和画布都是无基准工程坐标，单位米，authid 为空，不能反投到 EPSG:4326。井的 `coordinate_status` 仍是 `untransformed`。\n- 在数据页依次打开 A1 的井口、LAS、分层、时深和 D61 层位，得到五个可关闭标签，来回切换不丢内容。LAS 标签能看到 GR。分层表里 Time 为空。D61 标签能把派生栅格显示到地图上。\n\n第一段实现把阶段 A 和阶段 B 一起交付，地震标签解码一条测线。\n\n### 阶段 B — 一条地震剖面\n\n- SEG-Y 建立 inline/crossline 到文件偏移的索引，不把体读进内存。\n- 打开地震资产时使用数据页上的地震标签。标签内选择一条 inline 或 crossline，只解码这一条（约 411 或 641 道，901 样点，2 ms）。\n- A1 的 D61 分层按第 3 节的 TVD 插值换成毫秒，标到 A1 所在的那条 inline 上。插值没有得到数值时，剖面上标第 3 节选出的原因：「无时深表」、「超出时深表」或「时深表无序」。只有得到数值时才标时间。\n- 验收夹具是这一条 inline，不是整个 `.sgy`。切换到 A1 的时深标签再切回地震标签，剖面仍在。\n\n### 阶段 C — 只编 D61\n\n- 结构面是 D61 时间栅格。\n- 单因素先算井上 D61 到 D62 的厚度，厚度用已经装箱的 D62 与 D61 时间栅格相减，得到双程时间等厚。井上只用两口都有 TVD 的厚度做标定，不把 MD 厚度混进同一张栅格。缺 TVD 的井不提供样本，并写明原因。时间等厚换成米时，用这些井的间隔速度做 power 为 2 的 IDW，权重与 `paleo:paleo_constraint_idw` 相同，采样在 D61 像元中心，不用它默认的外扩网格，不传约束线。能提供样本的井不足 3 口时不写假曲面，界面写「厚度样本不足以成面」。一口都没有时写「没有厚度样本」。像元中心落在这些井的分层点凸包之外时为 nodata -9999。只有层位栅格缺失时，才退回对井点厚度本身做 IDW。砂地比、距井距离、屏障距离本计划不做。\n- 厚度栅格不是相编码。`paleo:paleo_facies_polygonize` 只吃整数相编码栅格，空值为 -9999。本文件夹没有这样的栅格，参考 GeoJSON 也不在这个网格上，所以不从厚度栅格造相，也不新设厚度分档。已有的优先级融合只接受已经带相编码的栅格，输入顺序就是优先级。两张或以上才融合，一张时直接多边形化，一张都没有时界面写「没有相编码栅格」并且不调用融合。这不是第 6 节排除的相序规则融合。\n- 验证项是井上 D61 时间（由 TD 表得到）减去 D61 栅格在井位处的时间。残差保留符号。绝对值大于 10 ms 成为一条问题。没有算出时间的井列出原因，不算一条数值残差。采样取分层点最近的像元中心。点落在测网外超过半个像元时列出「井位不在测网内」，不算数值残差。该像元是 nodata 时列出「井位落在空道」，不算数值残差。问题字段是井名、带符号的残差或原因、以及 10 ms 阈值。点开问题留在验证页，不新建预览标签栏。共享地图移到该井，已有连井面板滚到该分层。地震视图已经打开时滚到对应测线和时间。还没打开时，按阶段 B 解码这一条，放进数据页已有的地震标签，验证页不新增标签。\n- 布局导出一张 PDF，含井位和厚度栅格。相多边形进 PDF 只在 `paleo:paleo_facies_polygonize` 已经跑过相编码栅格之后。`paleo.json` 里的 mock 多边形和参考 GeoJSON 不进 PDF。\n\n完成标准：凡有可用 D61 时间的井都有一条时间残差；其中一口井走完「问题 → 地图、连井、地震剖面」；PDF 已导出。相多边形出现在 PDF 里只在相编码栅格已经多边形化之后。\n\n### 阶段 D — 辅助资料\n\n- PNG、PPTX、PDF 作为 `document` 或 `image_reference` 受管入库，角色 `reference`，挂到辅助实体。在数据页打开后各自成为一个标签：图片直接显示，文档用 QDesktopServices 打开本地文件 URL，不拼 shell。\n- 三份 GeoJSON 入库并标明未配准，不生成地图图层。标签里能看到要素个数和相名。\n- 文档、未配准图片、未配准 GeoJSON，以及 `参考资料/` 里 HZ28-6-1 的 XML，都挂到辅助实体，关联角色是 `reference`。这份 XML 不按内容去挂井，也不并进 A1–A20。其他 XML 用 QXmlStreamReader 区分井口或测井，不解析 DTD，不取外部实体。\n- 完成后，地图上的井和 D61 位置与阶段 A 相同。\n\n### 阶段 E — 八个层位，然后才是版本状态机\n\n- 编图 chip 只有 C3、C6、D53、D61、D62、D63、D71、D72。切换沿用已有的按层位懒加载。\n- 井分层里其余名字只出现在连井。\n- 8 个层序界面的 chip 切换属于本阶段。版本用现有 ReleaseStore。保存调用 createRelease，快照不可改。ReleaseStore 增加 published 标记，并记下 D61 PDF 的资产 id 和 SHA-256。发布要求这份快照里有该 PDF，并且每口井的残差或原因已经写入。八个 chip 不挡住发布。已发布的行不更新。下一次保存是新的 release。不单独再开一段产品。\n\n## 6. 本计划不做\n\n- 把参考 GeoJSON 或扫描相图配准到局部测网。\n- 地震体渲染、任意测线、三维相机。`geo3d_workspace.json` 不迁。\n- 体系域 LST/TST/HST。\n- 砂地比、距井距离、TIN、等值线、屏障 IDW、相序规则融合、多 realization、相界地质类型、暗色模式。\n- 把 SEG-Y 全本或层位点文本提交进本仓库。\n- `paleo-merged-main` 的数据管理界面。\n\n## 7. 第一段实现\n\n阶段 A 的最小切片和阶段 B 的一条剖面一起做：\n\n1. 局部测网 CRS，不写 4326。\n2. A1 的井实体，以及井口、LAS、分层、时深四条主关联和只读 RAW。\n3. D61 层位实体、其 RAW，以及父版本指向该 RAW 的时间栅格。\n4. `200P_seismic.sgy` 外部链接，抽出一条 inline。\n5. 用 A1 的 TD 表把 D61 标到这条剖面上。\n6. 数据页预览标签栏：至少能同时打开 A1 的 GR、A1 的时深和这一条地震剖面，三个标签互不覆盖。阶段 A 的井口、分层和 D61 三个标签同时仍在，地震标签是第六个。\n\n阶段 C 的残差要等这三样对齐之后再算。\n\n测试：分类、单井匹配、两候选未决、多井分层文件产生多条关联、D61 装箱的 geotransform 和 nodata、复制 LAS 的 SHA-256、缺失外部路径、再次点选聚焦已有标签、打开 SEG-Y 时内存不随文件大小线性增长、图层 authid 为空。为本工区写出的 ONNX 栅格使用同一套 D61 geotransform，且不是 1×1，这是行为约束的测试，不是第一段交付物。井口恰好匹配一口已有井时不新建井。相同 SHA-256 再次导入不增加第二条主关联。TD 超出范围标「超出时深表」且不外推。时深表无序标「时深表无序」。缺 D62 的井不提供厚度样本。LAS 的 -99999 不绘制。失败文案是「读取失败」。`A1.Las` 能分类为测井。道头范围不对时不解码。加载文案含「正在读取」和文件名。相同 SHA-256 再次导入不新建版本，只允许补上已能匹配的未决关联。不足两个 TD 样点标「无时深表」。残差用最近像元中心，空道不是数值残差，绝对值大于 10 ms 才成为问题。手工：A1 在 (5288.67, 8219.94)，容差半个像元，压在 D61 栅格上。\n"}]} -->

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
| 3 | Any missing external path shows "找不到源文件" plus the path | S | ACCEPTED | Same blast radius. Every external type, not only SEG-Y and documents. |
| 4 | First slice lists four A1 links, including LAS, matching stage A | S | ACCEPTED | Fixes an internal contradiction. |
| 5 | Build `catalog.sqlite` in the first slice | M | DEFERRED | 20 wells fit in `catalog.json`. A second store is new infra. P3 pragmatic. |

Deferred item 5 is recorded in TODOS.md during this review.

Spec review launch 1 scored 5/10 on an export that predates the geotransform, the TD interpolation rule, and the multi-well link rule. Its recommendation to cut phases C–E, the residual check, the PDF, and the eight chips is declined. Those stages are the written plan, sequenced after the first slice. One reviewer is not a two-model user challenge.

<!-- AUTONOMOUS DECISION LOG -->
## Decision Audit Trail

| # | Phase | Decision | Classification | Principle | Rationale | Rejected |
|---|-------|----------|-----------|-----------|----------|
| 1 | CEO | Well heads create wells; logs, tops, and TD only link | Mechanical | P5 explicit | A new normalized well-head name creates one well. LAS, tops, and TD never create a well. Two matches stay unresolved. | Never creating the 20 wells |
| 2 | CEO | One multi-well file, one link per well | Mechanical | P1 completeness | ExportWellHead.dat and DC.dat stay one asset. Tabs filter by the selected well. | Split into 20 files |
| 3 | CEO | No constant velocity on this work area | Mechanical | P1 completeness | All 20 wells have TD tables. A missing table shows 「无时深表」. | Constant-velocity fallback |
| 4 | CEO | D61 geotransform from P1 P2 P3 only | Mechanical | P5 explicit | 411×641, origin (0, 16406), dx=12793/640, dy=-16406/410, nodata -9999, last write wins. ONNX uses the same grid. | A second 0–12793 × 0–16406 extent |
| 5 | CEO | Draw surface_x/y on a custom eqc meter CRS | Mechanical | P5 explicit | coordinate_status stays untransformed. Authid is not EPSG:4326. | Write project_x/y or EPSG:4326 |
| 6 | CEO | catalog.json is the only store in this plan | Mechanical | P3 pragmatic | catalog.sqlite waits in TODOS.md until JSON roundtrips and queries get slow. | Build sqlite in the first slice |
| 7 | CEO | Ungeoreferenced tif is image_reference | Mechanical | P5 explicit | Derived GeoTIFF and ONNX rasters stay map rasters. | Treat every tif as a picture |
| 8 | CEO | First delivery is phase A plus phase B | Mechanical | P1 completeness | Section 7 already says they ship together. The alone-A seismic placeholder is unused on that delivery. | Ship A with no seismic line |
| 9 | CEO | Polygons come from a coded raster, not from thickness | Mechanical | P5 explicit | FaciesFusion only stacks coded rasters. This folder has none. The PDF shows wells and the thickness raster, and adds polygons when a coded raster exists. | Polygonize the thickness raster |
| 10 | CEO | HZ28-6-1 XML is always a reference | Mechanical | P5 explicit | That file is not one of wells A1–A20. Other XML is still classified by content. | Link it as a well |
| 11 | CEO | Keep phases C, D, and E | Mechanical | P6 bias to action | Spec launch 1 asked to cut them. The user's plan keeps them after the first slice. Publish waits for the D61 PDF. | Cut residuals, PDF, and eight chips |
| 12 | CEO | Absolute residual above 10 ms opens an issue | Taste | P5 explicit | The volume sample rate is 2 ms. 10 ms is five samples and is visible. The signed residual is kept. | Leave the threshold unnamed |
| 13 | CEO | TD outside range, non-monotonic, or empty does not invent a time | Mechanical | P1 completeness | No extrapolation and no constant velocity. The posted reason is 超出时深表, 时深表无序, or 无时深表. | Clamp to the table ends |
| 14 | CEO | One well-head match links; same checksum does not duplicate | Mechanical | P1 completeness | Creating a second well on a one-match, or a second primary link for the same bytes, breaks the 20-well list. | Create on every import |
| 15 | CEO | Empty layer authid | Mechanical | P5 explicit | Section 2 already says the authid is not written. The test asserts it is empty. | Any authid other than EPSG:4326 |
| 16 | CEO | Issue click drives map, correlation, and the existing seismic decoder | Mechanical | P5 explicit | The data-page QTabWidget stays on the data page. The validation page does not grow a second tab bar. | Put preview tabs on the validation page |
| 17 | CEO | Continue after three spec launches | Mechanical | P6 bias to action | Launch 3 scored 6/10. The cap is three launches. The contradictions it named were repaired in the plan and were not re-graded. | Stop the pipeline on the failed score |
| 18 | Design | Move preview under the map | Mechanical | P5 explicit | DataPreviewTabs is in the right dock today. Curves need the center column. The right dock keeps import and the asset table. | Leave the curve in the narrow dock |
| 19 | Design | One seismic viewer on the data page | Mechanical | P4 DRY | The bottom dock already has a 地震 tab. This plan keeps seismic in the data-page tab and leaves 连井 in the bottom dock. | Two seismic panels |
| 20 | Design | Folder import opens only the well-head tab | Mechanical | P3 pragmatic | project_area is one folder. Opening every asset as a tab hides the map. | One five-step wizard per file |
| 21 | Design | Per-tab well combo | Mechanical | P5 explicit | A later LAS selection must not retarget an open tops tab. Empty body is 「先选择一口井」. | One global current-well that rewrites open tabs |
| 22 | Design | Validation click stays visible | Taste | P5 explicit | The row, the map, and the 连井 bottom tab update without leaving 验证. Seismic is the button 「在数据页看这条剖面」. | Scroll a hidden data-page tab and call that the answer |
| 23 | Design | Preview starts at one third | Taste | P3 pragmatic | An always-open empty band fights the 1280×800 map. The user can drag the splitter. | A fixed 320px band |
| 24 | Design | Publish disabled until the tooltip condition is met | Mechanical | P1 completeness | DESIGN.md requires a reason tooltip on a disabled control. The eight chips still do not gate publish. | Enable Publish and fail after the click |
| 25 | DX | One screen rule for the validation double-click | Mechanical | P5 explicit | Chinese section 5 and the design block already use the button. The older English bullet still said the click decodes seismic. This block is the tie-break. | Keep both click behaviors |
| 26 | DX | Write the thickness formula | Mechanical | P1 completeness | Interval velocity needs a two-way time from the TD ties. IDW interpolates that velocity. | Leave the scaling expression unnamed |
| 27 | DX | Split 失败 and 未决 | Mechanical | P5 explicit | The confirm counts were not decidable. Stored-but-unmatched is 未决. Not stored is 失败. | One bucket for every problem |
| 28 | DX | Keep the geotransform origin | Taste | P3 pragmatic | The accepted coefficients stay the outer corner. Centers are half a pixel inside. Shifting the origin would move A1. | Move the origin onto the nodes |
| 29 | DX | No SEG-Y byte-map editor in this plan | Mechanical | P3 pragmatic | This file's inline header is zero on purpose. The next volume can add an override later. | A header-map dialog in the first slice |
| 30 | Eng | Delete the eqc CRS string | Mechanical | P5 explicit | localGridCrsProj is +proj=eqc +ellps=WGS84. An empty authid does not stop the inverse. | Keep eqc and hope authid stays empty |
| 31 | Eng | One publish gate on MapVersionStore | Mechanical | P4 DRY | The mapping 发布 button does not call ReleaseStore. A second gate would not disable that button. | Add published columns only on releases |
| 32 | Eng | Containing cell, not nearest center | Taste | P5 explicit | A node on the pixel corner is equidistant from four centers. Half-open bounds make the corner test stable. | Keep nearest-center and an unnamed tie-break |
| 33 | Eng | Do not call runThicknessChain for this isochore | Mechanical | P5 explicit | That function IDWs a TVD difference and then polygonizes. The plan already forbids both. | Leave the button on the old chain |

| 34 | CEO2 | Retire `SeismicPreviewPanel` rather than wire `setCatalog` | Mechanical | P4 DRY | The preview tabs own SHA-verify and single-line preview; the panel is unhosted and its `traces()` fallback would load ~950 MB. | Keep it compiled but unhosted |
| 35 | CEO2 | `linksForEntity("")` is not the unresolved accessor | Mechanical | P5 explicit | An empty well id silently fetches every unresolved link; add `unresolvedLinks()` and guard the empty call. | Keep the ambiguous overload |
| 36 | CEO2 | `catalog.json` schema_version gate + `.bak` rotation + refuse writes after failed open | Mechanical | P1 completeness | A failed `open()` followed by a mutation overwrites the sole registry with an empty catalog; the §9 rollback sentence has nothing to restore. | Warn-only, allow overwrite |
| 37 | ENG2 | Batch catalog mutations per import (transaction scope) | Mechanical | P3 pragmatic | A folder import performs ~5N full JSON serializations; a crash mid-import leaves assets without links. | Keep per-mutator `save()` |
| 38 | ENG2 | Write `PALEO_INLINE_*` (and XLINE/DT/T0) onto derived horizon rasters | Mechanical | P1 completeness | Validation→seismic navigation is dead in production: tests inject the keys by hand, `writeHorizonGeoTiff` never does. | Leave navigation disabled |
| 39 | ENG2 | Sentinel-filter `TIME(ms) == -99999` rows at TD parse | Mechanical | P5 explicit | Raw `toDouble` lets a -99999 ms row enter interpolation and contaminate the tie — live bug at wellfileparsers. | Filter only the lookup column |
| 40 | ENG2 | `isochronMs <= 0` cells write nodata, not negative thickness | Mechanical | P5 explicit | Horizon crossover produces negative metres; the plan names no rule. | Keep silent negative output |
| 41 | DX2 | `kTypes` vocabulary = classifier vocabulary + lock fixed-aux row + valid-type-only overrides | Mechanical | P5 explicit | `tops` is a role not a type; `tabular` is missing, so untouched rows can emit a spurious `well_head` override. | Keep string-keyed combos |
| 42 | DX2 | The 20-well residual table owns validation navigation | Mechanical | P1 completeness | Only exceed-wells (issueTable rows) are locatable; pass/warn rows cannot navigate. | Keep issue-table-only navigation |
| 43 | DX2 | Publish-gate residual summary samples top X/Y | Mechanical | P4 DRY | The gate samples `surfaceX/Y` while the table uses `pickSamplePoint`; published snapshot can disagree with what the user reviewed. | Keep surface sampling |
| 44 | ENG2 | Derived rasters go to `artifacts/derived/` with catalog DERIVED versions | Mechanical | P1 completeness | `/tmp` outputs dead-link after reboot and bypass version lineage. | Keep `QDir::temp()` outputs |
| 45 | DESIGN2 | Status tags render as DESIGN.md capsules, not colored text | Mechanical | P5 explicit | `setForeground` yields ~2.3–3.3:1 contrast at 9pt; the 未决 capsule already follows the token pattern. | Keep colored text |
| 46 | DESIGN2 | Status-bar copy 「工程坐标 · 米 · 未投影」 + CRS sentence in folder confirm | Mechanical | P5 explicit | Shipped 「工程网格 · 局部米」 drifts from the contracted sentence that the PDF already prints. | Keep current copy |
| 47 | ENG2-impl | Wave-1 rebase onto `ce746b3` keeps HEAD's stricter primitives | Mechanical | P4 DRY | `ce746b3` (WindWang2) already landed ver-N restore, schema gate, QSaveFile atomic write, TD sentinel, isochron guards, `PALEO_INLINE_*`; branch work rebased on it so only unique deltas layer on top (BatchSave, `.bak`, unresolvedLinks, catalogOpenFailed, TOCTOU, load-time segment skip, residual nav, RASTER_MISSING, top-XY gate). | Keep both implementations and pick per hunk ad hoc |

### 0I. Build order

Hour 1: empty authid, `catalog.json`, well-head create, case-insensitive `.Las`, D61 column and row formulas, read-only RAW hash. Hour 2–3: TD edges (outside range, strictly increasing, at least two samples), SEG-Y range gate, one inline, tab copy. Hour 4–5: power-2 IDW at D61 cell centers, residual at the nearest cell center, issue click stays on the validation page. Hour 6: the section 7 tests, the thickness PDF, reference link role `reference`, ReleaseStore save and publish.

Effort for the first delivery (phases A and B together): human about two weeks, CC about one day once the catalog types exist. Phases C–E are later and are not cut. The 10 ms threshold is the open taste decision.

Spec loop: three launches, latest score 6/10. Metrics appended to `~/.gstack/analytics/spec-review.jsonl` at 2026-09-25T15:02:32Z. No fourth review.

<!-- autoplan-accepted:ceo -->
- Work coordinates are a local meter grid. EPSG:4326 stays a label and is not written onto map layers. While `coordinate_status=untransformed`, the map reads `surface_x/y` and does not write `project_x/y`. Layer, project, and canvas CRS are an engineering meter CRS with an empty authid and no transform to EPSG:4326. Do not use +proj=eqc.
- Import classifies by the `libs/ingest` path rules, parses metadata, resolves the entity, creates a well only from a new well-head name, creates a horizon or survey only when that name is new, writes an explicit `entity_asset_link`, and stores a read-only RAW with SHA-256. The 966 MB SEG-Y is an external link. File extensions are matched without case, so `A1.Las` is a well log. An ungeoreferenced `.tif` is `image_reference`. A derived GeoTIFF stays a map raster.
- Well names are compared after trim, dropping hyphens and spaces, and case-folding. This work area well-head file has no UWI column, so the match key is the normalized well name. This plan does not add UWI or alias columns. Well-head rows are the only creator of well entities. A normalized well-head name with no existing well creates one well and a `well_head` link. Two well-head rows with the same normalized name, or one row that matches two existing wells, stay unresolved and do not merge. LAS, tops, and TD never create a well. LAS reads the `~W` WELL mnemonic first. Tops and TD read the well-name column first. Zero matches then try the filename stem (`A1.Las` to A1). Zero after that, or two existing matches, keeps the asset, marks one link unresolved, leaves the entity id empty, creates no well, and does not merge. Two candidates are named in the link note. A well-head row that matches exactly one existing well links `well_head` and does not create another well. A SHA-256 that is already stored does not create a version. It may attach one missing primary link when an unresolved link now matches one well. It does not merge two wells. The UI says the bytes are already stored and whether a link was attached. A new SHA-256 appends an immutable version and that version becomes the only primary link for that role. `ExportWellHead.dat` and `DC.dat` are each one asset with one link per well name. Preview tabs filter that asset by the selected well. A horizon filename in {C3,C6,D53,D61,D62,D63,D71,D72} binds to that boundary; any other name is an unresolved horizon and stays off the map chips.
- D61.dat becomes a RAW whose child is a 411 by 641 time raster. P1 (inline 1315, crossline 4165) is (0, 0). P2 (1315, 4805) is (12793, 0). P3 (1725, 4805) is (12793, 16406). dx is 12793/640 m and dy is 16406/410 m. The north-up geotransform origin is x=0, y=16406, pixel width +dx, pixel height -dy. Empty bins are nodata -9999. Duplicate bins keep the last sample and record the collision count. ONNX rasters for this work area use this same geotransform. A1 at (5288.67, 8219.94) falls inside the grid within half a cell.
- Time ties interpolate TIME(ms) linearly from the top TVD against the TD table TVD column. If that TVD is empty, use MD against the TD MD column. The sentinel -99999 never enters the interpolation. A well with no TD table is posted with the well name and 「无时深表」. This work area does not invent a constant velocity, including the fallback named in PALEO_QGIS_PLAN. Section position, the nearest horizon sample, and the residual use the top X,Y. Fall back to the wellhead only when the top has no coordinates. The default first section is A1 inline. A depth outside the usable TD samples is not extrapolated. That top is marked 「超出时深表」 and does not enter the residual. If the depth column used for that lookup is not strictly increasing, the well is marked 「时深表无序」 and is not interpolated. If fewer than two finite samples remain on that column, the well is marked 「无时深表」 and is not interpolated.
- `catalog.json` is the only lifecycle store and the query source for this plan. The managed path is `{stage}/{asset_id}/{version_id}/{filename}` where stage is RAW, DERIVED, INTERMEDIATE, or OUTPUT. `catalog.sqlite` is not implemented in this plan. TODOS.md holds the rebuild trigger: after catalog.json roundtrips and list queries get slow.
- Data-page preview is a closable QTabWidget under the map on the data page only, not the workflow tab bar. Re-selecting an open asset focuses its tab. The filter well changes only when the selected asset link resolves to exactly one well. Otherwise an open multi-well tab keeps its filter. Loading copy is 「正在读取」 plus the filename. Failure copy is 「读取失败」 plus the reason plus the filename. LAS samples equal to -99999 are gaps and are not drawn. Any missing external path shows 「找不到源文件」 plus the path. Empty copy is 「还没有打开的预览 — 在列表中选择一条数据」. Import confirm stays inside the existing wizard in PALEO_QGIS_PLAN §42.7. SMI columns are shown and are not rearranged. The CRS step shows the engineering meter CRS, does not offer EPSG:4326, writes an empty authid, and cannot switch to a CRS that transforms into degrees.
- Phase A acceptance includes five open tabs for A1 well head, LAS (GR visible), tops (Time blank), TD curve, and D61 (grid stats plus show-on-map). The first delivery decodes one seismic line. That first delivery keeps the five phase A tabs and adds the seismic tab as a sixth.
- Phase B decodes one inline or crossline only. Opening the SEG-Y does not load the whole 966 MB file. A1 D61 is posted in time on A1 inline using the TVD interpolation above.
- Phase C thickness is the D62 time raster minus the D61 time raster, scaled to meters by power-2 IDW of per-well interval velocity from TVD pairs only. MD thickness is not mixed in. Sampling is at D61 cell centers, not the default expanded grid, and no constraint lines are passed. Fewer than three wells shows 「厚度样本不足以成面」. Zero wells shows 「没有厚度样本」. Outside the hull of those tops is nodata -9999. Well-only IDW of thickness is the fallback only when a horizon raster is missing. The thickness raster is not a facies code. paleo:paleo_facies_polygonize receives only an integer facies raster with nodata -9999. This folder has no such raster, and the reference GeoJSON is not on this grid, so thickness is not classified into facies and no thickness breaks are invented. Existing priority-stack fusion accepts only rasters that already carry facies codes. Two or more are fused in input order. One is polygonized directly. Zero shows 「没有相编码栅格」 and does not call fusion. That is separate from the excluded facies-order rule table. The phase C PDF contains the wells and the thickness raster. Facies polygons are added only after paleo:paleo_facies_polygonize has run on a coded facies raster. Mock polygons in paleo.json and the reference GeoJSON stay off the PDF. The residual is computed D61 time minus the time at the D61 cell whose center is nearest the top. The signed value is kept. Absolute value greater than 10 ms opens an issue. A nodata cell is listed as 「井位落在空道」 and is not a numeric residual. Issue fields are well name, signed residual in ms or the reason, and the 10 ms threshold. The click stays on the validation page, moves the shared map, and scrolls the existing correlation panel. It scrolls the seismic view only when that view is already open. Otherwise that one line is decoded into the existing data-page seismic tab. No QTabWidget is created on the validation page.
- Documents, ungeoreferenced images, ungeoreferenced GeoJSON, and the HZ28-6-1 XML under 参考资料 use link role `reference` on an auxiliary entity. That XML is never linked as a well. Other XML is still classified by content.
- Phase E keeps the eight horizon chips C3, C6, D53, D61, D62, D63, D71, and D72. Versions use the existing ReleaseStore. Save calls createRelease and the snapshot is immutable. ReleaseStore gains a published flag and stores the D61 PDF asset id and SHA-256. Publish requires that PDF and a residual row or a reason for every well. The eight chips do not gate publish. A published row is not updated. The next save is a new release. Phases C, D, and E stay in this plan and are sequenced after the first slice.
- Verification: QTest for classify, one-match bind, two-candidate unresolved, multi-well tops file yielding many links, D61 geotransform and nodata, checksum of a copied LAS, missing external path on any type, tab focus-on-reselect, SEG-Y open not loading the whole file, layer authid is empty, well-head one-match does not create a well, same SHA-256 re-import does not add a second primary link, TD outside range posts 「超出时深表」 and does not extrapolate, a non-monotonic TD posts 「时深表无序」, a well missing D62 contributes no thickness sample, LAS -99999 is not drawn, and failure copy is 「读取失败」. An ONNX raster written for this work area uses the D61 geotransform and is not 1 by 1. That ONNX check is a behavior constraint, not a first-delivery deliverable. Manual check: A1 at (5288.67, 8219.94) within half a cell on the D61 raster.
<!-- /autoplan-accepted:ceo -->

### CEO dual voices

Native review completed. Its first line is `INPUT: ceo 9ba68233b1889bf9bb06907d9df3900a3c38835969493b763fcf754b3d47d550`, which matches the voice snapshot `autoplan-ceo-rLS241`. Codex preflight is `model_unusable`: `gpt-6-astra` returns HTTP 400 and asks for a newer Codex. One-line fix: `GSTACK_CODEX_MODEL=<supported-model>`. Outside status: unavailable. This phase is `[subagent-only]`. Consensus cells are N/A.

```
CEO DUAL VOICES — CONSENSUS TABLE:
  Dimension                           Claude  Codex  Consensus
  1. Premises valid?                   mixed   —      N/A
  2. Right problem to solve?           mixed   —      N/A
  3. Scope calibration correct?        no      —      N/A
  4. Alternatives sufficiently explored? partial —    N/A
  5. Competitive/market risks covered? partial —      N/A
  6. 6-month trajectory sound?         no      —      N/A
```

The native voice's request to drop phase D and the publish machine is not applied. Phases C, D, and E stay. It is recorded at the final gate as a single-voice disagreement, not a two-model user challenge.

### Section 1: Architecture Review

Mode remains SELECTIVE EXPANSION. Accepted scope is the catalog, the data-page tabs, the D61 tie, the thickness and residual loop, reference files, eight chips, and ReleaseStore publish after the D61 PDF and the residual list exist.

Measured on `200P_seismic.sgy`: 1,012,709,244 bytes, 901 samples, 2 ms, format 1, stride 3844, 263451 traces. Inline at header offset 188 and crossline at offset 192 are 0. CDP at offset 20 runs 4165–4805 inside each 641-trace block. Corners match P1/P2/P3. The plan now indexes `inline = 1315 + trace/641` and `crossline = CDP`, and stops if the trace count, CDP order, or corners disagree. A1 D61 TVD is 1935 m and D62 TVD is 1971 m.

```
project_area files
    | classify, explicit link
    v
catalog.json  (temp file, rename, write queue)
    |-- RAW copies (LAS, tops, TD, horizons) or external SEG-Y link
    |-- D61/D62 time rasters on the header grid
    v
QGIS canvas (engineering CRS, empty authid, no path to EPSG:4326)
    |-- data-page QTabWidget under the map
    |-- validation issues (no second tab bar)
    v
ReleaseStore snapshot + D61 PDF id/hash + published flag
```

Applied from the swarm: off-GUI-thread index and parse; same SHA does not create a second version but may attach one unresolved link after the well exists; project and canvas CRS match the layer; path segments cannot contain slashes or `..`.

### Section 2: Error & Rescue Map

The user-visible strings already in the plan cover a missing file, a bad parse, the three TD reasons, an empty trace, and no thickness samples. Gaps that are now required behavior: a copy or checksum failure discards the partial and does not write a catalog row; a torn catalog keeps the previous file; a header that fails the CDP/corner test shows the numbers and does not decode; fewer than three thickness wells shows 「厚度样本不足以成面」; a point outside the grid is counted and not written; publish without the PDF hash stays unpublished.

### Section 3: Security & Threat Model

This is a single-user desktop app. Applied: managed paths are one segment each; other XML is read with QXmlStreamReader and no external entities; documents open through QDesktopServices, not a shell; reference GeoJSON stays off the map because it is ungeoreferenced, not because of the asset-role word `input`. The engineering CRS has no inverse into EPSG:4326, which is the fix for on-the-fly reprojection of local meters.

### Section 4: Data Flow & Interaction Edge Cases

Re-select focuses an open tab. A known SHA does not create a second version. A missing external path shows 「找不到源文件」. Still required, and now stated: the long read runs off the GUI thread, closing the tab cancels it, and leaving the data page hides the preview. The well filter changes automatically only when the selected asset resolves to exactly one well; a multi-well file has its own well picker.

### Section 5: Code Quality

Reuse `LasParser`, `SegyReader` sample decode, `paleo:paleo_constraint_idw` weights, `FaciesFusionAlgorithm`, `FaciesPolygonizeAlgorithm`, and `ReleaseStore`. Do not call `ConstraintWorkflow::runIdw` for this grid: that path grows the extent by 10 percent and clips only when constraint lines exist. Do not add a second polygonizer. `ProjectData::timeAtTvd` must return the TD reason, not a bare NaN.

### Section 6: Test Review

Section 7 names the QTests for classify, bind, geotransform, SHA, tabs, TD reasons, and the SEG-Y memory bound. The structural SEG-Y index is now part of that test: a fixture with inline word 0 and CDP 4165–4805 must decode, and a bad CDP order must not. The 966 MB file stays out of git. Friday check: one generated inline, A1 time from the TD table, empty authid, section still there after switching tabs. Hostile check: two wells that normalize like A1, and the thickness raster is not passed to polygonize.

### Section 7: Performance

The index is about 4 MB of trace headers, not 966 MB of samples. One line is about 2.3 MB. Eight horizons stay lazy, one raster at a time. `catalog.json` for 20 wells is not the bottleneck. The header walk is. It runs once per open, off the GUI thread.

### Section 8: Observability

The geologist sees the named Chinese string on the tab or the issue row. The same sentence is also written to the existing log dock. There is no metrics service, and this plan does not add one.

### Section 9: Deployment

Rollback of a bad import is: quit, restore the previous `catalog.json`, delete the new managed RAW tree. The external SEG-Y is not deleted. There is no feature flag and no second process. First session checks are the QTests plus A1 on the D61 grid and six data-page tabs.

### Section 10: Long-Term Trajectory

Reversibility is 4/5. The engineering CRS, `catalog.json` before sqlite, and polygons waiting on a coded raster are accepted debt. Phases C–E sit on the A/B tie. The native voice asked to cut phase D and the publish machine. That cut is declined. Publish no longer waits for the other seven horizons, and it does require the residual list.

### Section 11: Design & UX

The data page order is map, asset list, then the ordinary QTabWidget under the map. Loading, failure, and missing-path copy are specified. DESIGN.md stays the source for tokens. The workflow tab bar is untouched. `/plan-design-review` should run after this phase because the preview tabs are UI scope.

```
select row -> focus or open tab -> 正在读取 -> content or 读取失败
missing path -> 找不到源文件
validation issue -> stay on 验证, move map, scroll correlation
```

### NOT in scope

Georegistration, volume render, arbitrary lines, system tracts, sand ratio, TIN, contours, barrier IDW, facies-order rule fusion, multi-realization, facies-boundary semantic types, dark mode, committing the SEG-Y or the horizon text, and the old data-manager UI. `catalog.sqlite` is deferred in TODOS.md under "P3 — catalog.sqlite 查询索引".

### What already exists

LAS parsing, IBM float decode, correlation tracks, seismic preview panel, `paleo:paleo_facies_polygonize`, `paleo:paleo_facies_fusion`, `paleo:paleo_constraint_idw`, LayerManifest, and ReleaseStore snapshots. This plan changes their inputs and does not replace them.

### Dream state delta

```
CURRENT                         THIS PLAN                         12-MONTH
Importer treats files as        One D61 tie, thickness from       Eight horizons,
vectors and reads the SEG-Y     the two time grids, residual      a coded facies map,
whole.                          list, tabs, then publish.         versions a geologist
                                --->                              will defend.
                                                                  --->
```

### Error & Rescue Registry

| Codepath | Failure | Rescued | User sees |
|---|---|---|---|
| SEG-Y index | CDP order or corners disagree | yes | the numbers read, no line |
| Import copy | hash mismatch or disk full | yes | 「读取失败」 and no catalog row |
| TD tie | outside range, not strictly increasing, or fewer than two samples | yes | the named reason, no invented time |
| Thickness | fewer than three TVD pairs | yes | 「厚度样本不足以成面」 |
| Residual | off the survey or nodata | yes | 「井位不在测网内」 or 「井位落在空道」 |
| Publish | PDF hash or residual list missing | yes | stays unpublished |

### Failure Modes Registry

| Codepath | Failure mode | Rescued? | Test? | User sees? | Logged? |
|---|---|---|---|---|---|
| SegyIndex | structural check fails | Y | Y | Y | Y |
| Catalog write | torn JSON | Y | Y | Y | Y |
| TdTie | bad table | Y | Y | Y | Y |
| Thickness | degenerate hull | Y | Y | Y | Y |
| Publish | missing PDF | Y | Y | Y | Y |

### Scope decisions

Accepted: binding rules, tab copy, missing path, LAS in the first slice, structural SEG-Y index, engineering CRS, horizon time-isochore scaled by TVD velocity, top XY for the tie, residual list required before publish. Deferred: catalog.sqlite. Declined: cutting phases C–E, polygonizing thickness, constant velocity.

### Implementation Tasks

- [x] **T1 (P1, human: ~3d / CC: ~2h)** — SEG-Y index — Index this volume by CDP and 641-trace blocks, off the GUI thread. (landed `bdfeb58`/`10c9cbf`; indexing + 641-block + field-record base done — "off the GUI thread" still open, see pass-2 report)
  - Surfaced by: Section 1 — inline and crossline words are zero
  - Files: src/io/segyreader.cpp, src/io/segyreader.h
  - Verify: a fixture with inline word 0 and CDP 4165–4805 decodes one line; a bad CDP order does not; memory is not linear in file size
- [x] **T2 (P1, human: ~2d / CC: ~1h)** — catalog — Atomic catalog.json, safe path segments, re-link unresolved wells without a second RAW. (landed `0f02fc3` + `64349a2`; atomic save, safe segments, empty-id+note re-link)
  - Surfaced by: Section 1 — SHA freeze and torn writes
  - Files: src/io/dataimportservice.cpp, src/services/projectdata.h
  - Verify: QTest for one-match, two-candidate, same SHA attaches a link and does not write a second version
- [x] **T3 (P1, human: ~2d / CC: ~1h)** — D61 grid and CRS — Bin with the header geotransform; engineering CRS on the layer, project, and canvas. (landed `5972466`; ENGCRS WKT + binner geotransform)
  - Surfaced by: Section 3 — eqc can inverse-project
  - Files: src/qgis/qgisprojectservice.cpp
  - Verify: authid empty and no transform to EPSG:4326; A1 within half a cell
- [x] **T4 (P2, human: ~3d / CC: ~2h)** — phase C — Time isochore from the two rasters, TVD-only velocity IDW, residual at the top coordinate. (landed `781914e`)
  - Surfaced by: native voice — well-only IDW discards the horizon grids
  - Files: src/workflow/workflows.cpp, src/algorithms/paleoalgorithms.cpp
  - Verify: fewer than three wells does not write a fake surface; off-survey is not a numeric residual
- [x] **T5 (P2, human: ~1d / CC: ~45min)** — data-page tabs — QTabWidget under the map, fixed copy, well picker for multi-well files. (landed `6735dcf`)
  - Surfaced by: Section 11
  - Files: src/ui/pages/pagepanels.cpp
  - Verify: re-select focuses; loading and failure strings; six tabs in the first delivery

### Taste for the final gate

The 10 ms cutoff is a screening value, printed on the issue, not a geologic standard. The native voice asked to remove phase D and the publish step. Those stay. Thickness is the horizon time difference scaled by well TVD velocity, not an IDW of mixed MD and TVD.

Approval readiness for the decisions applied above: the measured SEG-Y index, the engineering CRS, the re-link rule, and the residual list before publish are in the implementation plan. The cut of phases C–E is not.
<!-- pass1-baseline-edits-design {"sourceSha256":"e1be363ee47a2ff97f6d9d36187c3e8249660ac5343a4801240013dd88b6f650","replacements":[{"oldText":"布局：右侧仍是资产列表。地图留在中央。预览标签栏放在地图下方，只在数据管理页可见。从列表选中一条资产时，若已有同资产标签则切过去，否则新开一个可关闭标签。多井文件的标签按当前井过滤。当前井只有在选中资产的关联恰好解析到一口井时才改变。否则已经打开的多井标签保持原过滤。\n\n空态文案是「还没有打开的预览 — 在列表中选择一条数据」。复制或解析还在进行时，标签显示「正在读取」和文件名，不显示半份曲线。索引、LAS 解析、层位装箱和单条解码不在界面线程上做。关掉标签就取消。离开数据页只隐藏预览，不拆掉。多井文件旁边有一口井的选择；只有选中资产恰好关联一口井时才自动改这口井。失败时标签写「读取失败」、原因和文件名，不留白面板。任何外部链接的源文件不存在时，该标签写「找不到源文件」和那条路径。\n\n导入确认仍用 `docs/PALEO_QGIS_PLAN.md` §42.7 的向导（文件、字段、CRS、预览、确认）。SMI 文本的列是固定的，向导展示识别出的列，不让用户重排。确认入库之后，才在数据页打开对应标签。这一阶段向导的 CRS 步展示无基准工程坐标，单位米，不提供 EPSG:4326，authid 留空，不能改成会反算到经纬度的 CRS。\n\n| 资产类型 | 标签里显示什么 |\n|---|---|\n| `well_log` | 单井曲线。默认 GR，可换 AC、DEN。用现有单道绘制，曲线值 -99999 是空样，不绘制。不把多井连井面板搬进这个标签 |\n| `well_stratification` | 该井的分层表：层名、MD、TVD。Time 列为空就显示空，不填假时间 |\n| `time_depth` | 该井的 TIME–TVD 曲线 |\n| `well_head` | 井名、X、Y、KB、TD、`coordinate_status`。选中时地图同时高亮该井 |\n| `horizon` | 网格尺寸、Z 的单位和范围、派生栅格是否已生成。提供「在地图上显示」。标签内不画 26 万个点 |\n| `seismic` | 现有地震预览。标签内选择一条 inline 或 crossline，只解码这一条 |\n| `image_reference` | 按面板宽度缩放的图片 |\n| `document` | 文件名、类型，以及用本地文件 URL 打开。这一阶段不做 PDF 内嵌翻页 |\n| `geojson` | 要素个数、坐标范围、相名字段。未配准时标明不加入地图 |\n","newText":"布局：右侧 dock 仍放导入和资产表。地图留在中央。`DataPreviewTabs` 现在嵌在右侧 dock 的资产表下面。本计划把它移到地图下方的中央列，只在数据管理页出现。没有打开的标签时，这里只有一行次级文字，颜色 #5D6E80，字号 9pt：「还没有打开的预览 — 在列表中选择一条数据」。第一个标签打开后，地图和预览用竖向 QSplitter 分开。预览一开始约占中央列高度的三分之一，可以拖。预测、约束、编图、验证不放这根分割，也不放预览标签。底部 dock 继续是日志、任务、连井剖面。单线地震只出现在数据页的预览标签里。底部 dock 不再同时放一条地震预览，避免两处剖面。\n\n从列表选中一条资产时，已有同资产标签就切过去，否则新开一个可关闭标签。关掉标签就取消读取。离开数据页只隐藏预览，不拆掉。索引、LAS 解析、层位装箱和单条解码不在界面线程上做。超过 1 秒的进度出现在底部「任务」页，超过 10 秒给出预计时间。\n\n标签标题始终是资产名，例如「A1 · GR」「200P · IL1315」或文件名。标题不被状态句替换。正文里才写「正在读取」和文件名，不显示半份曲线。失败时正文写「读取失败」、原因和文件名，并给「重试」。外部链接的源文件不存在时，正文写「找不到源文件」和那条路径。\n\n多井文件（井口表、DC.dat）每个预览标签自带一个标为「井」的下拉框，只列出挂到这份资产上的井。下拉框不改其他标签里已选的井。还没选井时，正文是「先选择一口井」，不是空表。选中后标题写成「DC.dat · A1」。打开 A1 的 LAS 只聚焦 LAS 标签，不改已经打开的分层标签。\n\n资产表列是名称、类型、关联。关联显示井名，或警告标签「未决」。标签用浅底 #FFF4E0，字用 #F29900，并且始终带「未决」二字。tooltip 写出两个规范化井名。参考资产显示「参考」。没有合并动作。未决标签上的「挂到这口井」只有在用户点了一口已有井之后才可点。名称的 tooltip 保留受管或外部路径。\n\n数据页在现有「导入井数据」「导入地震数据」「导入边界数据」旁边增加「导入工区文件夹」。单文件按钮仍选一个文件，确认后只打开这一条的标签。文件夹按钮选 `project_area` 目录。下一步是分类表：路径、类型、实体、未决或失败。不让用户重排列。预览步只显示口数、井名和失败数，不展开层位散点，也不解地震道。确认步写「入库 n，未决 n，失败 n」，然后只打开井口标签。其余资产等列表点击。SMI 文本的列是固定的，向导展示识别出的列。同一 SHA-256 在确认步写「字节已在库」，以及「已补上关联」或「没有新的关联」，然后聚焦已有标签，不新开第二个。CRS 步不是坐标系下拉框。正文是「局部工程坐标，单位米。源文件里的 EPSG:4326 只是标签，不会画到地图上。」不能改成会反算到经纬度的 CRS。状态栏用同一事实加一条次级文字：「工程坐标 · 米 · 未投影」，颜色 #5D6E80，不用警告色。这 20 口井都是这个状态。\n\n| 资产类型 | 标签里显示什么 |\n|---|---|\n| `well_log` | 单井曲线。曲线用下拉框，默认 GR，可换 AC、DEN。缺的曲线项禁用，tooltip「这条曲线不在文件里」。整条都是 -99999 时写「这条曲线没有有效样点」，不绘制。深度和曲线值用 JetBrains Mono 9pt，右对齐。不把多井连井面板搬进这个标签 |\n| `well_stratification` | 该井的分层表：层名、MD、TVD、X、Y。Time 列为空就显示空，不填 -99999，也不填假时间。数字列用 JetBrains Mono 9pt，右对齐 |\n| `time_depth` | 该井的 TIME–TVD 曲线。没有可用样点时写「无时深表」，不画假线 |\n| `well_head` | 井名、X、Y、KB、TD、`coordinate_status`。选中时地图同时高亮该井。数字用 JetBrains Mono 9pt |\n| `horizon` | 网格尺寸、Z 的单位和范围、派生栅格是否已生成、拒绝点数、碰撞次数。提供「在地图上显示」。点下去缩放到栅格并闪一下，按钮变成「已在地图上」。隐藏仍用左侧图层树的勾选。栅格还没有时按钮禁用，tooltip「这一阶段还没有这个层位的栅格」。标签内不画 26 万个点 |\n| `seismic` | 数据页里的唯一地震预览。纵测线 / 横测线切换，加上限制在 inline 1315–1725 或 crossline 4165–4805 的数值框。初始值是 A1 所在 inline，旁注「A1 所在测线」。索引没建好时数值框禁用，tooltip「正在建立道索引」。换测线先清掉上一张剖面。标定写「A1 D61」和时间，或「无时深表」「超出时深表」「时深表无序」之一，没有数值就不标时间 |\n| `image_reference` | 按面板宽度缩放的图片，并写「未配准，不加入地图」 |\n| `document` | 文件名、类型，以及用本地文件 URL 打开。打不开时写「系统没有打开这个文件」和原因。并写「未配准，不加入地图」。这一阶段不做 PDF 内嵌翻页 |\n| `geojson` | 要素个数、坐标范围、相名字段。注明「经纬度，与本测网不是同一空间」，不加入地图 |\n| `xml` 参考 | HZ28-6-1 写「未配准，不加入地图」和「不对应 A1–A20」 |\n\n键盘焦点是 2px #1B73D0 描边。禁用控件带原因 tooltip。资产表、预览标签、井下拉框、验证表都设 accessible name。窗口最小仍是 1280×800。本计划不做手机版。dock 可以浮动。\n\n```\n工作流标签    层位 chips                 搜索\n图层 dock | 地图                        | 导入 + 资产表\n          | 预览 QTabWidget（仅数据页）  |\n底部 dock：日志 | 任务 | 连井剖面\n状态栏：坐标  比例尺  层位  工程坐标 · 米 · 未投影\n```\n"},{"oldText":"能提供样本的井不足 3 口时不写假曲面，界面写「厚度样本不足以成面」。一口都没有时写「没有厚度样本」。","newText":"能提供样本的井不足 3 口时不写假曲面。约束页面板写「厚度样本不足以成面」。一口都没有时，同一面板写「没有厚度样本」。这两句不弹对话框。同一面板列出每口井的 D61 TVD、D62 TVD、间隔速度或原因。"},{"oldText":"只有层位栅格缺失时，才退回对井点厚度本身做 IDW。","newText":"只有层位栅格缺失时，才退回对井点厚度本身做 IDW。正常图层名是「D61–D62 时间等厚（米）」。这条退回才用「井点厚度（米，无层位栅格）」。"},{"oldText":"一张都没有时界面写「没有相编码栅格」并且不调用融合。","newText":"一张都没有时，编图页写「没有相编码栅格，这一工区不从厚度生成相」，并且不调用融合。"},{"oldText":"问题字段是井名、带符号的残差或原因、以及 10 ms 阈值。点开问题留在验证页，不新建预览标签栏。共享地图移到该井，已有连井面板滚到该分层。地震视图已经打开时滚到对应测线和时间。还没打开时，按阶段 B 解码这一条，放进数据页已有的地震标签，验证页不新增标签。","newText":"验证页已有的问题表，在这次 D61 残差里使用列：井名、残差或原因、阈值。20 口井都在，不只有超限的井。标签用 DESIGN.md 的状态色，并且带字：通过用 #43A047，超过阈值用 #F29900，未计算用 #5D6E80。表头计数「n 口超过 10 ms」。还没跑时表为空，面板写「还没有计算 D61 残差」。「井位落在空道」和「井位不在测网内」是警告行，不是数值 0。双击留在验证页，不新建预览标签栏。这一行已经能看见井名、带符号的残差或原因、10 ms 阈值。共享地图移到该井。底部 dock 先切到已有的「连井剖面」，再滚到该分层。按钮「在数据页看这条剖面」才离开验证页：切到数据管理，聚焦数据页地震标签，并按阶段 B 解码或滚到这一条。验证页不新增标签。"},{"oldText":"布局导出一张 PDF，含井位和厚度栅格。","newText":"「导出 D61 图件」用现有布局填一张模板再写 PDF，不把用户放在空白版面上。图上有标题「D61 厚度」、井名、米制图例、不画 nodata、比例尺、指北针，以及「工程坐标 · 米 · 未投影」。成功显示路径和 SHA-256。失败显示「导出失败」和原因。图内仍含井位和厚度栅格。"},{"oldText":"ReleaseStore 增加 published 标记，并记下 D61 PDF 的资产 id 和 SHA-256。发布要求这份快照里有该 PDF，并且每口井的残差或原因已经写入。八个 chip 不挡住发布。已发布的行不更新。下一次保存是新的 release。","newText":"ReleaseStore 增加 published 标记，并记下 D61 PDF 的资产 id 和 SHA-256。保存和发布是两个按钮。发布另开确认，列出版本、PDF 名和井数。每口井都有残差或原因，并且这份 PDF 已经在快照里，发布才可点。缺任何一条时按钮禁用，tooltip 写出缺的是哪一口井，或是缺 PDF。八个 chip 不挡住发布。没有栅格的 chip 禁用，tooltip 是「这一阶段还没有这个层位的栅格」。已发布的行标「已发布」，不更新。下一次保存标成新版本，不写成在改这一行。"},{"oldText":"接到本工区时使用 D61 的 geotransform。挤成二维后不是 411×641 就失败，不写栅格","newText":"接到本工区时使用 D61 的 geotransform。挤成二维后不是 411×641 就失败，不写栅格，预测页写「结果不是 411×641，没有写入栅格」"},{"oldText":"手工：A1 在 (5288.67, 8219.94)，容差半个像元，压在 D61 栅格上。","newText":"手工：A1 在 (5288.67, 8219.94)，容差半个像元，压在 D61 栅格上。界面：文件夹确认只打开井口标签；未决行显示「未决」；多井标签在选井前显示「先选择一口井」，并且选择另一条资产不改这个井；发布在缺残差或缺 PDF 时禁用，tooltip 写出原因。"},{"oldText":"预览标签按当前选中的井过滤，不把文件拆成 20 份。","newText":"多井文件不拆成 20 份。每口井的过滤留在该预览标签自己的「井」下拉框里，不跟另一张标签走。"}]} -->
<!-- autoplan-accepted:design -->
- The data-page preview QTabWidget moves from the right dock to a vertical splitter under the map, on the data page only. With no tab open it is one muted line in #5D6E80 at 9pt: 「还没有打开的预览 — 在列表中选择一条数据」. The first tab gives the preview about one third of the center column, and the user can drag the splitter. Predict, constraint, mapping, and validation do not show it. The bottom dock stays 日志, 任务, and 连井剖面. The only seismic viewer is the data-page preview tab.
- 「导入工区文件夹」 sits beside the existing single-file import buttons. The folder step shows a classification table of path, type, entity, and 未决 or 失败, then counts, then confirm. Confirm reads 「入库 n，未决 n，失败 n」 and opens only the well-head tab. A single-file import opens only that asset's tab. The same SHA-256 says 「字节已在库」 and either 「已补上关联」 or 「没有新的关联」, then focuses the existing tab.
- Asset columns are 名称, 类型, 关联. 关联 shows the well name, a warning tag 「未决」 with tooltip of both candidate names, or 「参考」. There is no merge action. 「挂到这口井」 enables only after the user picks one existing well.
- Each multi-well preview tab has its own 井 combo and does not rewrite another tab. With no well selected the body is 「先选择一口井」. The title includes the filename and the well.
- Tab titles stay the asset name. The body shows 「正在读取」 plus the filename, 「读取失败」 plus the reason plus the filename and a 「重试」 button, or 「找不到源文件」 plus the path. Task progress uses the bottom 任务 tab: a bar after 1 second and an estimate after 10 seconds.
- The status bar shows 「工程坐标 · 米 · 未投影」 in #5D6E80. The wizard CRS step is that sentence, not a CRS picker, and the same sentence is printed on the D61 PDF.
- The seismic tab has a 纵测线 / 横测线 control and a spin box clamped to inline 1315–1725 or crossline 4165–4805. The initial line is A1 inline, captioned 「A1 所在测线」. During indexing the control is disabled with tooltip 「正在建立道索引」. Changing line clears the image first. The tie shows 「A1 D61」 plus milliseconds, or 「无时深表」, 「超出时深表」, or 「时深表无序」, and does not invent a time. The horizon tab shows rejected-point and collision counts. 「在地图上显示」 zooms, flashes, and becomes 「已在地图上」. Hiding stays on the layer tree.
- LAS uses a curve combo. A missing curve is disabled with 「这条曲线不在文件里」. An all-null curve says 「这条曲线没有有效样点」 and draws nothing. Numeric columns use JetBrains Mono 9pt, right-aligned. Tops include X and Y. An empty time cell stays empty.
- Ungeoreferenced images, documents, and the HZ28 XML say 「未配准，不加入地图」. That XML also says 「不对应 A1–A20」. GeoJSON is captioned 「经纬度，与本测网不是同一空间」. A document the desktop cannot open says 「系统没有打开这个文件」 plus the reason.
- The constraint panel lists each well's D61 TVD, D62 TVD, and interval velocity or the reason, and shows 「厚度样本不足以成面」 or 「没有厚度样本」 there. The layer title is 「D61–D62 时间等厚（米）」, or 「井点厚度（米，无层位栅格）」 only for the missing-raster fallback. The mapping page says 「没有相编码栅格，这一工区不从厚度生成相」 when no coded raster exists.
- The validation table lists all 20 wells with name, signed residual or reason, and the 10 ms threshold. Tags are 通过, 超过阈值, and 未计算, and the header count is 「n 口超过 10 ms」. Before the run the panel says 「还没有计算 D61 残差」. Off-survey and nodata rows are not numeric zeros. The click stays on 验证, moves the shared map, selects the existing 连井剖面 bottom-dock tab, and scrolls it. 「在数据页看这条剖面」 switches to 数据管理 and focuses the data-page seismic tab for that one line. No preview tab bar is added on 验证.
- 「导出 D61 图件」 fills one layout: title 「D61 厚度」, well labels, a meter legend, nodata not drawn, scale bar, north arrow, and the CRS sentence. Success shows the path and SHA-256. Failure shows 「导出失败」 and the reason. Publish is a separate confirm. It stays disabled, with a tooltip naming the missing well or the missing PDF, until every well has a residual or a reason and the PDF is in the snapshot. A published row is tagged 「已发布」. The next save is labeled as a new version.
- Horizon chips with no raster stay disabled with tooltip 「这一阶段还没有这个层位的栅格」. A prediction tensor that is not 411×641 shows 「结果不是 411×641，没有写入栅格」 and writes no raster.
- Keyboard focus is a 2px #1B73D0 ring. Disabled controls carry a reason tooltip. The asset table, preview tabs, well combo, and validation table have accessible names. The window minimum stays 1280×800. This plan does not add a phone layout.
- Verify: folder confirm opens only the well-head tab; an unresolved row shows 「未决」; a multi-well tab shows 「先选择一口井」 until a well is chosen and does not change when another asset is selected; Publish stays disabled, with the reason in the tooltip, when a residual or the PDF is missing.
<!-- /autoplan-accepted:design -->

### Design review

Scope gate: plan mode, auto-selected B, reviewing docs/PROJECT_AREA_PLAN.md. UI scope is in. The shell is Operate. DESIGN.md exists and is the token source. Base branch: no origin and no local main; the working branch is master. Recent commits already landed the catalog, preview tabs, and the three-way locator (`424e185`, `f3d9b83`, `c994d21`). This review specifies the screen those commits still leave open.

Outside voice: unavailable. Codex is installed. Preflight `CODEX_MODE: model_unusable`. Verbatim: Model metadata for gpt-6-astra not found; HTTP 400 "The 'gpt-6-astra' model requires a newer version of Codex." Set `GSTACK_CODEX_MODEL` to a model this CLI accepts. Consensus cells are N/A. `[subagent-only]`.

Native design voice completed. First line `INPUT: design 2f516ca25b53f2b21c61dbfe2a7a7a73e355f98c82c26228dedef7596c319c47`. Ten findings. One was checked against the shell and corrected before editing: 连井剖面 is a bottom-dock tab (`paleomainwindow.cpp`, `WellCorrelationPanel`), not a widget that exists only on 约束. `ThreeWayLocator` scrolls it without selecting that tab. `DataPreviewTabs` is currently inside the right-dock `DataPage`, under the asset table. Import buttons are 导入井数据, 导入地震数据, 导入边界数据.

Designer binary is present at `~/.claude/skills/gstack/design/dist/design`. `generate` failed: "No OpenAI API key found." No `~/.gstack/openai.json` and no `OPENAI_API_KEY`. Visual mockups were not generated. HTML wireframe: `~/.gstack/projects/paleo_workstation/designs/data-page-20260925/wireframe.html`. Run `$D setup` to enable the image designer.

```
DESIGN OUTSIDE VOICES — LITMUS SCORECARD:
  Check                                    Claude  Codex  Consensus
  1. Brand unmistakable in first screen?   YES     —      N/A
  2. One strong visual anchor?             YES     —      N/A
  3. Scannable by headlines only?          YES     —      N/A
  4. Each section has one job?             YES     —      N/A
  5. Cards actually necessary?             NO      —      N/A
  6. Motion improves hierarchy?            NO      —      N/A
  7. Premium without decorative shadows?   YES     —      N/A
  Hard rejections triggered:               none    —      N/A
```

Classifier: OPERATE. The first screen is the workflow bar, the map, and the docks. No hero, no card grid, no carousel. Cards are not the interaction. Motion stays on the task dock. Litmus 5 is NO: cards are not needed. Litmus 6 is NO: instant Qt switching is the DESIGN.md motion rule, and that is the intended answer.

#### Pass 1 — Information architecture

Before: 6/10. The plan named map, list, and tabs under the map, and it did not say the widget currently lives in the right dock, how tall it is, or what the first click in an empty project is.

After: 9/10. Center column is map over preview. Right dock is import plus the asset table. Bottom dock is log, tasks, and correlation. One seismic viewer. A 10 would be a designer PNG of the same frame. The HTML wireframe is the stand-in.

#### Pass 2 — Interaction states

Before: 5/10. Preview loading, empty, and failure copy existed. Unresolved rows, the multi-well empty body, retry, all-null curves, the 20-well residual table, publish's disabled reason, and chip tooltips did not.

After: 9/10. Each of those has the sentence the user sees. Loading longer than one second uses the task dock.

#### Pass 3 — Journey

Before: 4/10. Phases listed files. They did not say what the geologist sees at the first import or when a residual is clicked.

After: 8/10. The storyboard below is the path. Phase D file types share the reference caption instead of each getting its own scene.

| Step | User does | User feels | Plan specifies |
|---|---|---|---|
| 1 | Opens the data page | Where do I put this folder? | 「导入工区文件夹」 next to the three single-file buttons |
| 2 | Confirms the folder | Did all 20 wells land? | 「入库 n，未决 n，失败 n」, well-head tab only |
| 3 | Opens A1 LAS, tops, TD, D61, one inline | Are they the same well? | Six tabs, GR visible, Time blank, A1 inline captioned |
| 4 | Runs thickness | Is this an isopach or a fallback? | Layer title distinguishes the time isochore from well-only IDW |
| 5 | Opens validation | Which wells are bad? | All 20 rows, count of wells over 10 ms |
| 6 | Double-clicks a well | Show me, here | Row, map, and 连井 update without leaving 验证 |
| 7 | Presses the seismic button | Now the line | Data page, existing seismic tab, one line |
| 8 | Exports and publishes | Is this the version I can hand on? | Template PDF, disabled Publish until the list is complete, 「已发布」 |

Five seconds: the workflow step and the map. Five minutes: one well tied on its inline. Five years: a published release that is not edited in place.

#### Pass 4 — AI slop

Before: 8/10. The plan already spoke in well names, milliseconds, and file names. It did not yet forbid a second seismic chrome.

After: 9/10. No hard rejection. Utility copy. One accent, used for the workflow step, the primary action, and the focus ring. Status colors always carry a word.

#### Pass 5 — Design system

Before: 7/10. The plan pointed at DESIGN.md and did not cite the status tag, the mono numbers, the focus ring, or the task-dock timing.

After: 9/10. Those tokens are in the preview, validation, and publish rules. No new color. Body stays 9pt. The 16px web rule is not applied. DESIGN.md already records that departure for this desktop tool.

#### Pass 6 — Responsive and accessibility

Before: 3/10. The code has a 1280×800 minimum. The plan did not say so, and it did not say there is no phone layout.

After: 8/10. Minimum stays 1280×800. Docks can float. Focus ring, accessible names, and disabled tooltips are required. A phone stack is out of scope for this workstation. Dock widths when every dock is open at exactly 1280 are still the window manager's job.

#### Pass 7 — Decisions

Chosen in this phase: the twelve structural rules in the design accepted block. Two are taste and stay on the final gate: the preview starts at one third of the center column, and seismic is revealed by 「在数据页看这条剖面」 instead of scrolling a hidden tab. The 10 ms cutoff, the time-isochore, the engineering CRS, and keeping phases C–E remain the earlier gate items.

### NOT in scope

- Phone and tablet layouts. This is a 1280×800 desktop shell.
- A second custom tab chrome. The workflow bar stays the only signature.
- Merging two unresolved well names from the asset table.
- Embedding a PDF page viewer. Documents open through the local file URL.
- Image mockups from the gstack designer until an OpenAI key exists.

### What already exists

- DESIGN.md tokens: primary #1B73D0, surface #FFFFFF, surface-alt #EDF1F5, text #24303E, text-muted #5D6E80, success #43A047, warning #F29900, error #E53935, Noto Sans SC, JetBrains Mono, focus ring 2px, status tags with a word.
- `PaleoMainWindow` workflow bar, left layer dock, right page stack, bottom dock, status coordinates and scale.
- `DataPage` import buttons, asset table, `DataPreviewTabs`.
- `WellCorrelationPanel` and `SeismicPreviewPanel` in the bottom dock. `ThreeWayLocator` on the validation double-click.
- `ValidatePage` issue table. `ReleaseStore` snapshots without a published flag yet. Layout designer shell.

### TODOS.md

No new design TODO. Phone layout is out of scope, not deferred work. Dark mode is already a P3 TODO. catalog.sqlite stays the existing P3 trigger.

### Design implementation tasks

- [x] **T6 (P1, human: ~1d / CC: ~40min)** — preview place — Move DataPreviewTabs under the map on the data page and give it the splitter and the one-line empty state. (landed `5bb80bc`)
  - Surfaced by: Pass 1 — the widget is in the right dock
  - Files: src/ui/pages/pagepanels.cpp, src/ui/paleomainwindow.cpp, src/ui/datapreview/datapreviewtabs.cpp
  - Verify: other workflow pages show no preview tabs; empty copy is the one muted line; dragging the splitter keeps both map and preview
- [x] **T7 (P1, human: ~1d / CC: ~40min)** — folder import and unresolved row — Add 「导入工区文件夹」, the classification table, and the 关联 column with 「未决」. (landed `4c0f575`/`090ffa2`; residual: HZ28 row lock + kTypes vocabulary — pass-2 tasks)
  - Surfaced by: Pass 2 — the three buttons do not cover 层位, 时深, or 参考资料
  - Files: src/ui/pages/pagepanels.cpp, src/io/dataimportservice.cpp
  - Verify: confirm opens only the well-head tab; same SHA focuses the existing tab; two candidates show 「未决」 and do not merge
- [x] **T8 (P1, human: ~4h / CC: ~20min)** — visible residual — Validation lists 20 wells; double-click selects the 连井 bottom tab; the seismic button focuses the data-page tab. (partial: residual table lists all 20 wells; navigation wired only on issueTable — pass-2 task)
  - Surfaced by: Pass 3 — a hidden scroll looks like a no-op
  - Files: src/ui/pages/pagepanels.cpp, src/linkage/threewaylocator.cpp
  - Verify: double-click does not change the workflow page; the button does; off-survey is not a numeric zero
- [x] **T9 (P2, human: ~4h / CC: ~20min)** — publish and PDF template — Disabled Publish tooltip, 「已发布」, and the one-page D61 template. (landed `0b8de17`)
  - Surfaced by: Pass 2 — publish had no control
  - Files: src/metadata/releasestore.cpp, src/ui/layoutdesignershell.cpp
  - Verify: missing PDF or a missing residual disables Publish and the tooltip names which; the PDF contains the CRS sentence

jq is not installed. Design task JSONL was not written. Install jq if a later aggregator needs the file.

### Design completion

| | |
|---|---|
| System audit | DESIGN.md present. UI scope yes. Branch master, no origin |
| Step 0 | 5/10 before the state and place gaps. Focus: all 7 passes |
| Pass 1 | 6/10 → 9/10 |
| Pass 2 | 5/10 → 9/10 |
| Pass 3 | 4/10 → 8/10 |
| Pass 4 | 8/10 → 9/10 |
| Pass 5 | 7/10 → 9/10 |
| Pass 6 | 3/10 → 8/10 |
| Pass 7 | structural rules written. 2 taste items held for the final gate |
| Overall | 3/10 → 8/10 |
| Mockups | 0 generated. 1 HTML wireframe, not an approved PNG |
| Decisions made | 12 structural rules in the design accepted block |
| Decisions deferred | 0 unspecified. Taste stays listed for the final gate |
<!-- pass1-baseline-edits-dx {"sourceSha256":"c5484b712b7c96809f6519dbb0f33561b735b4ab603d67612d9a54a5492a0b45","replacements":[{"oldText":"标签标题始终是资产名，例如「A1 · GR」「200P · IL1315」或文件名。标题不被状态句替换。","newText":"标签标题是「文件名 · 井或测线」，例如「DC.dat · A1」「200P · IL1315」。曲线名不进标题。状态句只在正文，不替换标题。"},{"oldText":"超过 1 秒的进度出现在底部「任务」页，超过 10 秒给出预计时间。","newText":"超过 1 秒的进度出现在底部「任务」页，超过 10 秒按已读字节线性估计剩余时间。标签关掉之后晚到的结果丢弃。"},{"oldText":"未决标签上的「挂到这口井」只有在用户点了一口已有井之后才可点。","newText":"未决行上有一个井下拉框，列出已有井，默认空。「挂到这口井」只有选中一口之后才可点。确认时同时写出资产名和井名。这一步可以撤销：撤销只把关联改回未决，不删除仍被别的关联用着的井。同一角色的旧版本可以用「将此版本设为主版本」，不复制字节。"},{"oldText":"不让用户重排列。预览步只显示口数、井名和失败数，不展开层位散点，也不解地震道。确认步写「入库 n，未决 n，失败 n」，然后只打开井口标签。","newText":"不让用户重排列。确认前可以改这一行的类型，规范化匹配键照常显示。参考资料目录里的 XML 默认显示为参考，要按井解析必须在表里改类型。预览步只显示口数、井名和失败数，不展开层位散点，也不解地震道。确认步的三个数只按这张表计，然后只打开井口标签。失败：文件没有落盘，原因是读写、解析或路径不合法，行上写原因和「重试」。未决：资产已保存，实体 id 为空；tooltip 用「无匹配」「两个候选: 名字, 名字」或「井口重名」。入库：写成了一条主关联。确认文案仍是「入库 n，未决 n，失败 n」。"},{"oldText":"不能改成会反算到经纬度的 CRS。状态栏用同一事实加一条次级文字：「工程坐标 · 米 · 未投影」，颜色 #5D6E80，不用警告色。这 20 口井都是这个状态。","newText":"不能改成会反算到经纬度的 CRS。与 `docs/PALEO_QGIS_PLAN.md` §42.7 的 CRS 选择器冲突时，以这句和空 authid 为准。状态栏用同一事实加一条次级文字：「工程坐标 · 米 · 未投影」，颜色 #5D6E80，不用警告色。这 20 口井都是这个状态。`invalid` 写「坐标无效」，`missing` 写「没有坐标」，仍用 #5D6E80。"},{"oldText":"栅格还没有时按钮禁用，tooltip「这一阶段还没有这个层位的栅格」。","newText":"栅格还没有时按钮禁用，tooltip「还没有这个层位的栅格」。"},{"oldText":"加上限制在 inline 1315–1725 或 crossline 4165–4805 的数值框。","newText":"数值框的范围来自打开时冻结的测网。本文件是 inline 1315–1725、crossline 4165–4805。"},{"oldText":"井名、X、Y、KB、TD、`coordinate_status`。","newText":"井名、X、Y、KB、TD、BottomX、BottomY、WellType、`coordinate_status`。"},{"oldText":"资产表、预览标签、井下拉框、验证表都设 accessible name。","newText":"资产表、预览标签、井下拉框、验证表的 accessible name 等于各自的可见标题。线框里的搜索是现有定位器，不在这一段新做。"},{"oldText":"北向上的 geotransform 是原点 x=0、y=16406，列方向 `dx`，行方向 `-dy`。","newText":"北向上的 geotransform 六个系数是 (0, dx, 0, 16406, 0, -dy)。原点是左上角像元的外角，不是像元中心。层位点按列号、行号写入该像元。残差和 IDW 用像元中心，中心比节点向网格内侧偏半个像元。本计划不改这组原点。"},{"oldText":"不改去扫别的字节","newText":"不改去扫别的字节。界面并排写出期望值和读到的值"},{"oldText":"井上只用两口都有 TVD 的厚度做标定，不把 MD 厚度混进同一张栅格。缺 TVD 的井不提供样本，并写明原因。时间等厚换成米时，用这些井的间隔速度做 power 为 2 的 IDW，权重与 `paleo:paleo_constraint_idw` 相同","newText":"一口井要同时有 D61 和 D62 的 TVD，才提供间隔速度，不把 MD 厚度混进同一张栅格。缺 D61 或 D62 的 TVD，或两层时深插值有一层没有数值，这口井不提供样本，并写明原因。井上 dt_ms 是 D62 时间减 D61 时间，单位毫秒，双程，时间来自时深表而不是层位栅格。Vint = (TVD_D62 − TVD_D61) / (dt_ms / 2000)，单位 m/s。栅格上等厚米数 = isochron_ms / 2000 × 该像元的 Vint。等厚若就用这口井自己的 dt_ms，米数等于两层 TVD 之差。A1 的插值约 16.23 ms，对应 36 m，只核对公式，不是栅格像元的期望值。IDW 插的是 Vint，power 为 2，权重与 `paleo:paleo_constraint_idw` 相同。测试用一对已知数按这个式子算期望米数"},{"oldText":"正常图层名是「D61–D62 时间等厚（米）」。","newText":"正常图层名是「D61–D62 等厚（米）」。值是米，名称里不再写时间。"},{"oldText":"还没跑时表为空，面板写「还没有计算 D61 残差」。","newText":"还没跑时表为空，面板写「还没有计算 D61 残差」，旁边是现有的「运行验证」。"},{"oldText":"预测页写「结果不是 411×641，没有写入栅格」","newText":"预测页写「结果不是 411×641，没有写入栅格」，并写出实际行列数"},{"oldText":"ReleaseStore 增加 published 标记，并记下 D61 PDF 的资产 id 和 SHA-256。","newText":"现有 CREATE TABLE IF NOT EXISTS 不会给旧库加列。打开时若缺列，用 ALTER TABLE 增加可空列 published INTEGER 默认 0、pdf_asset_id TEXT、pdf_sha256 TEXT。旧行读成未发布，不回写。"},{"oldText":"缺任何一条时按钮禁用，tooltip 写出缺的是哪一口井，或是缺 PDF。","newText":"缺任何一条时按钮禁用。tooltip 写缺的口数和下一步：先在验证页运行验证，或先导出 PDF。缺哪些井以验证表为准。"},{"oldText":"没有栅格的 chip 禁用，tooltip 是「这一阶段还没有这个层位的栅格」。","newText":"没有栅格的 chip 禁用，tooltip 是「还没有这个层位的栅格」。"},{"oldText":"受管路径是 `{stage}/{asset_id}/{version_id}/{filename}`，`stage` 就是 `RAW`、`DERIVED`、`INTERMEDIATE` 或 `OUTPUT`。","newText":"受管路径是 `{stage}/{asset_id}/{version_id}/{filename}`。逻辑阶段仍是 `RAW`、`DERIVED`、`INTERMEDIATE`、`OUTPUT`。目录段用小写，跟现有 DataCatalog 一致。catalog.json 在工程目录的 `artifacts/metadata/catalog.json`。"},{"oldText":"阶段 A 的最小切片和阶段 B 的一条剖面一起做：","newText":"开发入口沿用 README：`./paleo-dev build`，测试 `./paleo-dev test`（offscreen ctest）。catalog.json 在工程目录的 `artifacts/metadata/catalog.json`。第一段夹具用已有的 `testdata/project_area/`，CMake 变量是 `PROJECT_FIXTURE_DIR`，不读 966 MB 的体。默认测线取 D61 分层点所在的 inline；分层点没有坐标时用井口。夹具打印这个整数。打开 SEG-Y 时新增内存要小于一条剖面加索引，不随文件大小线性增长。第一段沿用已有的 `tst_import`、`tst_segy`、`tst_segy_lines`、`tst_datapreview`。残差、厚度、发布和 ONNX 分属后面的阶段，不挡第一段。\n\n阶段 A 的最小切片和阶段 B 的一条剖面一起做："},{"oldText":"成功显示路径和 SHA-256。失败显示「导出失败」和原因。","newText":"成功和失败都用对话框。成功写出路径和 SHA-256。失败写「导出失败」、原因和「重试」。"}]} -->
<!-- autoplan-accepted:dx -->
- Where the Chinese sections and this block differ from earlier English bullets on validation navigation, the multi-well filter, and tab titles, follow the Chinese sections and this block. A double-click on 验证 stays on that page, moves the shared map, selects 连井剖面, and scrolls to that top. It does not decode seismic. 「在数据页看这条剖面」 is the only control that opens the data-page seismic tab.
- The preview tab title is the filename plus the well or the line, for example 「DC.dat · A1」 or 「200P · IL1315」. The LAS curve name stays out of the title. Status text stays in the body.
- A thickness sample requires both D61 and D62 TVD values and a numeric TD tie for both tops. dt_ms is D62 time minus D61 time in two-way milliseconds. Vint in m/s is (TVD_D62 - TVD_D61) / (dt_ms / 2000). Thickness in meters is isochron_ms / 2000 times the IDW of Vint. The test computes the expected meters from a known pair. The layer title is 「D61–D62 等厚（米）」.
- Folder confirm counts only three outcomes. 失败 means the file was not stored. 未决 means the asset is stored and the entity id is empty, with tooltip 无匹配, 两个候选, or 井口重名. 入库 means a primary link was written. The user may change a row's type before confirm. XML under 参考资料 defaults to reference unless that type is changed.
- 「挂到这口井」 uses a well combo on the unresolved row, empty by default, and confirms the asset name and the well name. Undo clears that link back to unresolved and does not delete a well that other links still use. An older immutable version can become the primary without copying bytes.
- The releases table adds nullable published INTEGER default 0, pdf_asset_id TEXT, and pdf_sha256 TEXT. Old rows load as unpublished and are not rewritten. Export success and failure both use a dialog: path plus SHA-256, or 「导出失败」, the cause, and 「重试」.
- The D61 geotransform stays (0, dx, 0, 16406, 0, -dy). That origin is the outer corner of the upper-left pixel. Residuals and IDW sample cell centers, half a pixel inside the node. This plan does not move the origin.
- Developer commands stay ./paleo-dev build and ./paleo-dev test. catalog.json is artifacts/metadata/catalog.json under the project directory. The managed directory segment is the lowercased stage. The first-slice fixture is the existing testdata/project_area directory, not the 966 MB volume. Residual, thickness, publish, and ONNX checks do not gate tst_import, tst_segy, tst_segy_lines, or tst_datapreview. The default line is the inline of the D61 top, else the wellhead, and the fixture prints that integer.
- Line spin boxes use the ranges frozen on the survey at index time. For this file those ranges are inline 1315–1725 and crossline 4165–4805. A header mismatch shows expected and actual and does not scan other bytes. This plan has no byte-map editor.
- A disabled Publish tooltip states the missing count and the next action: run validation, or export the PDF. The well names stay on the validation table. An ONNX shape failure includes the actual dimensions. Where PALEO_QGIS_PLAN §42.7 shows a CRS picker, this plan's CRS sentence and empty authid replace it.
- Verify: the thickness test uses the formula above on a synthetic pair; folder confirm counts match the three definitions; a multi-well tab does not follow another asset; double-click does not leave 验证; the seismic button does; old release rows read back unpublished.
<!-- /autoplan-accepted:dx -->

### DX review

Mode is DX POLISH. The product is a Qt6 desktop workstation, not a public SDK. The person this pass serves is the C++ developer already in this repo, and the geologist opening `project_area`. Codex preflight remains `model_unusable` (gpt-6-astra HTTP 400). Outside status: unavailable. Consensus is N/A.

Native review completed. First line `INPUT: dx a7b69dccb06da847a3cce1fd51c221dd8f105af10c132d4f4c824fb77d4591b8`.

TARGET DEVELOPER PERSONA
Who: the engineer building Paleo Workbench, and the geologist who imports this one work area.
Context: Linux, `./paleo-dev`, an existing QGIS shell. The 1.4 GB tree stays outside git.
Tolerance: a small fixture test should finish after the tree is already bootstrapped. Copying the full SEG-Y is not a two-minute hello world.
Expects: README commands, one catalog file, and error text that says what to do next.

Developer clock: bootstrapped tree, `./paleo-dev build`, `./paleo-dev test`, small fixture, first green ctest for the slice. Target: competitive, 2–5 minutes for that test, not for vendor bootstrap.
Geologist clock: open a project, 「导入工区文件夹」, confirm, well-head tab. The copy of the work area dominates. Champion under 2 minutes is not the target for 1.4 GB.

Magical moment, existing vehicle: the data-page tab shows A1 GR and the map highlights the well. No new hosted playground.

```
DX DUAL VOICES — CONSENSUS TABLE:
  Dimension                           Claude  Codex  Consensus
  1. Getting started < 5 min?          no      —      N/A
  2. API/CLI naming guessable?         partial —      N/A
  3. Error messages actionable?        partial —      N/A
  4. Docs findable & complete?         partial —      N/A
  5. Upgrade path safe?                partial —      N/A
  6. Dev environment friction-free?    yes     —      N/A
```

#### Passes

Getting started was 4/10 because the plan named no command and mixed later-phase tests into the first slice. It is 7/10 after the README commands, the small fixture, and the phase split. Bootstrap of QGIS stays outside this clock.

API naming was 5/10 because `well_stratification` and `tops`, and `seismic` and `seismic_volume`, sit side by side without a sentence that the link role is the second one. The plan already lists both. Left as the existing vocabulary. 6/10.

Errors were 5/10. Preview failures already had a reason and 「重试」. Confirm counts, publish, and ONNX shape did not say the next action. 7/10 after the three-way count, the publish tooltip, and the actual tensor shape.

Documentation was 4/10 because the Chinese body and the older English bullets disagreed on the validation click. 7/10 after this block names the tie-break. A public docs site is out of scope.

Upgrade was 4/10 because `releases` had no column names. 7/10 after nullable columns and "old rows stay unpublished". No codemod.

Dev environment was 7/10. `./paleo-dev test` is offscreen ctest. Windows stays deferred. 8/10 with the fixture path named.

Community was 6/10. This is an internal workstation. No new community channel. No issues found that belong in this plan.

Measurement was 5/10. The fixture prints the inline integer and the thickness test checks the formula. No telemetry product. 6/10. A recurring TTHW dashboard is out of scope.

Overall 4/10 → 6/10. The lowest pass after the edits is community and measurement at 6. That is acceptable for an internal desktop tool in POLISH mode.

#### Journey

| Stage | Developer or geologist does | Friction | Status |
|---|---|---|---|
| Discover | Reads README Quickstart | Vendor bootstrap is long | ok, clock excludes it |
| Install | `./paleo-dev build` | Already the repo entry | fixed in the plan |
| Hello world | `./paleo-dev test` on testdata/project_area | Full SEG-Y must not be required | fixed |
| Real usage | Folder import, six tabs, one inline | Title and filter rules disagreed | fixed |
| Debug | 「读取失败」 plus reason, header expected versus actual | No docs URL | ok for this app |
| Upgrade | Old release rows load unpublished | No byte-map editor for a future volume | deferred |

#### NOT in scope

- A SEG-Y byte-map editor. This volume's inline header is zero by contract.
- Controls for IDW power, the 10 ms threshold, or ONNX resampling.
- A public documentation site.
- Phone layout, already excluded.
- Shifting the geotransform origin onto the nodes.

#### What already exists

- `./paleo-dev build` and `./paleo-dev test` in README.
- `releases(release_id, name, note, created_utc, manifest_json)` with no published column yet.
- `ValidatePage` button 「运行验证」.
- DESIGN.md tokens for the colors already cited.

#### DX tasks

- [x] **T10 (P1, human: ~4h / CC: ~20min)** — thickness formula — Implement Vint from the two TD ties and scale the isochron. Test a synthetic pair. (landed `781914e`; isochron/2000 × IDW²(Vint))
  - Surfaced by: DX pass on errors of scaling — the expression was missing
  - Files: src/workflow/workflows.cpp
  - Verify: expected meters match dt_ms / 2000 * Vint; a failed tie contributes no sample
- [x] **T11 (P1, human: ~2h / CC: ~15min)** — confirm counts — Count 失败, 未决, and 入库 with the three definitions. (landed `090ffa2`; 「跳过」 fourth count pending gate)
  - Surfaced by: DX — the confirm line was not decidable
  - Files: src/ui/pages/pagepanels.cpp, src/io/dataimportservice.cpp
  - Verify: a parse failure increments 失败 and does not create a well; two well-name candidates increment 未决
- [x] **T12 (P2, human: ~2h / CC: ~15min)** — release columns — Add the three nullable columns. Old rows stay unpublished. (landed `0b8de17`; columns live on map_versions per audit #31, not releases)
  - Surfaced by: DX — createRelease has no published field
  - Files: src/metadata/releasestore.cpp
  - Verify: a database from before the columns loads, and published is 0

jq is not installed. DX task JSONL was not written.

### DX scorecard

| Dimension | Before | After |
|---|---|---|
| Getting Started | 4/10 | 7/10 |
| API/CLI/SDK | 5/10 | 6/10 |
| Error Messages | 5/10 | 7/10 |
| Documentation | 4/10 | 7/10 |
| Upgrade Path | 4/10 | 7/10 |
| Dev Environment | 7/10 | 8/10 |
| Community | 6/10 | 6/10 |
| DX Measurement | 5/10 | 6/10 |
| TTHW | bootstrap is hours; slice test unnamed | slice test targeted at 2–5 min after bootstrap |
| Competitive rank | Needs Work | Needs Work for the geologist copy; Competitive for the slice test |
| Magical moment | map plus A1 curve, unnamed | same vehicle, now the first confirm opens the well-head tab |
| Product type | desktop workstation | desktop workstation |
| Mode | DX POLISH | DX POLISH |
| Overall | 4/10 | 6/10 |
<!-- pass1-baseline-edits-eng {"sourceSha256":"8e10b77d387cb362866e8eed542c838f571e8414815b197ac146354259a37a36","replacements":[{"oldText":"不用 `+proj=eqc`，因为它会把局部米悄悄变成经纬度。","newText":"不用 `+proj=eqc`。现有 `DataCatalog::localGridCrsProj()` 就是 `+proj=eqc +ellps=WGS84 +units=m +no_defs`，会把局部米反投到 EPSG:4326。删掉这串。图层、工程和画布共用一个无大地基准的工程米坐标。`mapUnits` 是米，authid 为空，`isGeographic` 为假，转到 EPSG:4326 必须失败。"},{"oldText":"不改去扫别的字节。界面并排写出期望值和读到的值","newText":"不改去扫别的字节。删掉 `SegyReader::open` 在偏移 188 不变时改读偏移 8 和 20 的回退。本文件偏移 188 全是 0，CDP 在偏移 20 会变化，那条回退会盖掉按道号算出的 inline。角点用道头偏移 180 和 184 的整数米；比例因子 0 或 1 都表示用这个整数。对不上就并排写出期望值和读到的值"},{"oldText":"井口文件带 UTF-8 BOM。","newText":"井口文件带 UTF-8 BOM。读表头前去掉 U+FEFF。`trimmed()` 去不掉它。"},{"oldText":"BottomX、BottomY、WellType 留在井上。","newText":"BottomX、BottomY、WellType 不新增井表列。井口预览从这份井口资产的行里读。"},{"oldText":"资产仍然保留，链接标 `unresolved`，实体 id 留空，不新建井，也不合并。","newText":"资产仍然保留，链接标 `unresolved`，实体 id 留空，不新建井，也不合并。现有 `DataCatalog::addLink` 会拒绝空的实体 id。放行空实体 id，并增加备注字段，`catalog.json` 能往返。"},{"oldText":"不允许斜杠和 `..`。","newText":"不允许斜杠、`..`、换行、NUL 和其他控制字符。文件夹导入只收普通文件，不跟随指向所选目录之外的符号链接。一行失败不中断其余文件。"},{"oldText":"966 MB 的 SEG-Y 走外部链接。","newText":"966 MB 的 SEG-Y 走外部链接。链接时流式计算 SHA-256。打开时摘要不一致就写「源文件与入库时的 SHA-256 不一致」，不解码。"},{"oldText":"文件夹按钮选 `project_area` 目录。","newText":"文件夹按钮选 `project_area` 目录。一次确认里先处理井口行，再关联测井、分层、时深和层位。LAS 排在井口文件前面时，A1 仍然得到四条主关联。"},{"oldText":"参考资料目录里的 XML 默认显示为参考，要按井解析必须在表里改类型。","newText":"HZ28-6-1 这一行的类型不能改，始终是参考。同目录其他 XML 默认显示为参考；改成井之后如果对不上 A1–A20，保持未决，不用文件名去挂井。"},{"oldText":"底部 dock 继续是日志、任务、连井剖面。单线地震只出现在数据页的预览标签里。底部 dock 不再同时放一条地震预览，避免两处剖面。","newText":"底部 dock 继续放日志、任务、连井剖面，已有的发布和属性页保留。单线地震只出现在数据页的预览标签里。底部不再同时放一条地震预览。验证行的双击不再调用底部地震页。离开数据页时藏起预览分割条，不另造一块地图。"},{"oldText":"标签用浅底 #FFF4E0，字用 #F29900，并且始终带「未决」二字。","newText":"标签用浅底 #FFF4E0，字用 #24303E，边用 #F29900，并且始终带「未决」二字。"},{"oldText":"两种都不插值。","newText":"两种都不插值。现有 `TimeDepthTool::interpolateTimeMs` 会按深度排序，并在范围外夹到端点。改成按文件顺序、不排序、不夹取。剖面和残差共用这一个结果。同步改 `tst_timedeptool`。"},{"oldText":"只有层位栅格缺失时，才退回对井点厚度本身做 IDW。","newText":"现有 `MappingWorkflow::runThicknessChain` 用井点 TVD 差做 IDW，再调用相多边形。本计划的等厚不走这条路径，也不调用 `deriveFaciesPolygons`。D62 与 D61 的尺寸、geotransform 或 nodata 不一致就不写等厚。dt_ms 小于或等于 0，或 TVD 差不是正数，这口井不提供样本。控制点的凸包如果不是有面积的多边形，按不足 3 口处理，不写栅格。只有层位栅格缺失时，才退回对井点厚度本身做 IDW。"},{"oldText":"采样取分层点最近的像元中心。点落在测网外超过半个像元时列出「井位不在测网内」","newText":"采样用包含该点的像元，左闭右开。x 等于 12793 算最后一列，y 等于 0 算最后一行。落在这个像元矩形之外列出「井位不在测网内」"},{"oldText":"没有算出时间的井列出原因，不算一条数值残差。","newText":"没有 D61 分层的井写「无 D61 分层」，仍然占一行。没有算出时间的井列出原因，不算一条数值残差。绝对值等于 10 ms 记为通过。"},{"oldText":"版本用现有 ReleaseStore。保存调用 createRelease，快照不可改。现有 CREATE TABLE IF NOT EXISTS 不会给旧库加列。打开时若缺列，用 ALTER TABLE 增加可空列 published INTEGER 默认 0、pdf_asset_id TEXT、pdf_sha256 TEXT。旧行读成未发布，不回写。","newText":"编图页现有的发布按钮走 `MapVersionStore`，不走底部 `ReleaseStore`。只在这一条门上记下 D61 的 PDF 资产 id、SHA-256，以及 20 口井的残差或原因。`ReleaseStore` 仍只做清单快照，不成为第二道发布门。导出登记一个 OUTPUT 资产，不改已经冻结的版本行。下一次保存把该资产 id 和 SHA-256 抄进新的未发布版本。发布只读这一行。旧的 `map_versions` 和 `releases` 行读成未发布，不回写。缺 PDF 时 tooltip 写「导出 PDF 后再保存」。"},{"oldText":"残差用最近像元中心，空道不是数值残差","newText":"残差用包含该点的像元，空道不是数值残差"}]} -->
<!-- autoplan-accepted:eng -->
- Replace DataCatalog::localGridCrsProj. The engineering CRS has metre axes, no geodetic datum, an empty authid, and no QgsCoordinateTransform to EPSG:4326. Delete the +proj=eqc string in the same change.
- Delete SegyReader::open's fallback from header offset 188 to offsets 8 and 20. Inline for this file stays 1315 plus trace index over 641. Corners are the integer metres at offsets 180 and 184. A mismatch shows expected and actual and does not scan other bytes.
- TimeDepthTool::interpolateTimeMs keeps file order, does not sort, and does not clamp outside the range. The section post and the residual share one result: a value, 无时深表, too few samples, 时深表无序, or 超出时深表. Update tst_timedeptool in the same change.
- MappingWorkflow::runThicknessChain is not the D61 thickness path. Do not call deriveFaciesPolygons on that raster. Refuse the isochron when D62 does not match D61's dimensions, geotransform, and nodata. A non-positive dt_ms or TVD difference contributes no sample. A hull that is not a polygon with area uses the same panel sentence as fewer than three wells and writes no raster.
- Residual sampling uses the containing cell with half-open bounds. x equal to 12793 is the last column and y equal to 0 is the last row. Outside that rectangle is 「井位不在测网内」. Absolute residual equal to 10 ms is 通过. A well with no D61 top is a row 「无 D61 分层」.
- DataCatalog::addLink accepts an empty entity id and stores a note. 「挂到这口井」 fills that id and clears the note in one save. Folder confirm links well-head rows before logs, tops, and TD, so A1 still gets four primary links when the LAS path is listed first. Strip U+FEFF before the well-head header. HZ28-6-1's type cannot be changed.
- The mapping-page publish button stays on MapVersionStore. ReleaseStore remains a manifest snapshot and is not a second gate. Export registers an OUTPUT asset and does not edit a frozen version. The next save copies the PDF asset id and SHA-256 into a new unpublished version. Publish reads that row, which also holds a residual or a reason for every well. Old rows load unpublished.
- The external SEG-Y stores a streamed SHA-256. A mismatch shows 「源文件与入库时的 SHA-256 不一致」 and does not decode. Path segments reject control characters. Folder import reads regular files, does not follow a symlink outside the chosen root, and one bad file does not stop the walk.
- The preview splitter hides when the user leaves the data page. Validation double-click does not call the bottom-dock seismic panel. Existing publish and attribute bottom tabs stay. The unresolved tag uses #24303E on #FFF4E0 with a #F29900 border.
- Verify in the phase that changes the behavior, not inside tst_import, tst_segy, tst_segy_lines, or tst_datapreview: LAS-before-well-head still binds A1; TD is not clamped; containing-cell corners; dt_ms <= 0; D62 grid mismatch; polygonize is not called from thickness; engineering CRS has no transform to EPSG:4326; the offset-8 fallback does not run; old version rows stay unpublished.
<!-- /autoplan-accepted:eng -->

### Eng review

Scope gate: plan mode, auto-selected B, reviewing docs/PROJECT_AREA_PLAN.md. Scope is not reduced. Phases C, D, and E stay. The file count is above eight. The arrangement stays the existing modules: catalog, SEG-Y reader, time-depth tool, mapping workflow, and the version store the publish button already calls.

Outside review: unavailable. Codex `gpt-6-astra` returns HTTP 400. Consensus is N/A.

Native review completed. First line `INPUT: eng 23b621ec3b1e679e20588f83b96a0bfce91ce4632143794e387448f0e8468a39`. The code checks below are from that pass and were re-read in this session.

```
ENG DUAL VOICES — CONSENSUS TABLE:
  Dimension                           Claude  Codex  Consensus
  1. Architecture sound?               no      —      N/A
  2. Test coverage sufficient?         no      —      N/A
  3. Performance risks addressed?      partial —      N/A
  4. Security threats covered?         partial —      N/A
  5. Error paths handled?              partial —      N/A
  6. Deployment risk manageable?       yes     —      N/A
```

#### Architecture

`DataCatalog::localGridCrsProj` returns `+proj=eqc +ellps=WGS84 +units=m +no_defs` (`datacatalog.h`). Horizon GeoTIFFs already embed it (`horizonbinner.cpp`). The plan forbids that string. The replacement is one engineering metre CRS with no datum.

`MappingWorkflow::runThicknessChain` builds a TVD difference, calls constraint IDW, then `deriveFaciesPolygons` (`mappingworkflow.cpp`). That is the opposite of the thickness contract. The button must stop using that chain for D61.

`SegyReader::open` moves inline to offset 8 and crossline to offset 20 when offset 188 does not vary (`segyreader.cpp` around the `fieldVaries(8)` branch). This file's inline word is 0 and CDP at offset 20 varies, so the fallback would replace the trace/641 inline. Delete that branch.

`TimeDepthTool::interpolateTimeMs` sorts keys and returns the end sample outside the range (`timedeptool.cpp`). The plan's 「超出时深表」 and 「时深表无序」 cannot appear through that function. One unordered, unclamped result serves the section and the residual.

`DataCatalog::addLink` rejects an empty entity id (`datacatalog.cpp`). The unresolved row needs that empty id and a note.

The mapping-page publish path is `MapVersionStore`. `ReleaseStore` is a separate `releases` table. One gate, on the store the button already calls.

```
catalog.json / DataCatalog
    ├── EntityAssetLink (empty entity id + note allowed)
    ├── RAW/DERIVED files, stage directory lowercased
    └── external SEG-Y + streamed SHA-256
SegyReader::open  -- no offset 8/20 fallback
    └── one inline into the data-page tab
TimeDepthTool     -- file order, no clamp
    ├── section post
    └── residual row for every well
D61/D62 rasters -- horizonbinner geotransform, containing cell
    └── thickness metres, not runThicknessChain
MapVersionStore -- PDF asset id, SHA-256, residual list
    └── 发布 reads that row
ReleaseStore -- manifest snapshot only
```

#### Code quality

The thickness button and the plan describe two algorithms. Keeping both and switching with a flag would hide the old polygonize call. Remove that call from this work area's thickness action. Confidence 9/10, `mappingworkflow.cpp` `deriveFaciesPolygons`.

`WellHeadRecord` does not need BottomX columns. The preview reads those cells from the well-head asset. Confidence 8/10.

#### Tests

Existing `tst_timedeptool` locks the clamp. `tst_mapping` locks the TVD-difference chain if it calls `runThicknessChain`. Those expectations change in the same commit as the behavior. New cases stay out of `tst_import`, `tst_segy`, `tst_segy_lines`, and `tst_datapreview`. The test plan file lists the flows.

```
CODE PATHS                                      USER FLOWS
[+] SegyReader::open                            [+] Folder import
  ├── [GAP] no offset 8/20 fallback               ├── [GAP] LAS before well-head still binds A1
  └── [★★ TESTED] one-line decode exists          └── [GAP] one bad file does not stop the walk
[+] TimeDepthTool                               [+] Validation
  ├── [★★ TESTED] clamp (must change)             ├── [GAP] 20 rows including 无 D61 分层
  └── [GAP] out of range is not a value          └── [GAP] double-click does not decode seismic
[+] runThicknessChain                           [+] Publish
  └── [GAP] not used for this isochore            ├── [GAP] old rows stay unpublished
                                                  └── [GAP] export does not edit a frozen row
COVERAGE: existing slice tests stay; the new geology is uncovered until the phase that changes it.
```

#### Performance

SEG-Y open already seeks past samples. The plan's memory check is bytes read for the index plus one line, not resident set size. Catalog writes stay on the single write queue. No new network path.

#### Failure modes

- Replaced external SEG-Y: mismatch string, no decode. Test the hash.
- TD outside range: reason, not a clamped time. Test it. Critical if the old helper remains, because the user would see a time the plan says not to invent.
- Thickness button still polygonizes: user gets facies from metres. Critical until `runThicknessChain` is off that button.
- Publish on the wrong table: the button stays enabled. Critical until one store owns the gate.

#### NOT in scope

- A SEG-Y byte-map editor.
- A second map canvas.
- Deleting the attribute or publish bottom tabs.
- Phone layout.
- Windows.

#### What already exists

- `horizonbinner.cpp` geotransform `(0, dx, 0, 16406, 0, -dy)`.
- `testdata/project_area` and `PROJECT_FIXTURE_DIR`.
- `./paleo-dev test`.
- `MapVersionStore` publish messages and `ReleaseStore::createRelease`.

Sequential implementation. The catalog, the SEG-Y reader, the time-depth tool, and the mapping button share types. They land in that order, not in parallel worktrees.

#### Eng tasks

- [x] **T13 (P1, human: ~4h / CC: ~20min)** — CRS — Replace localGridCrsProj and assert no transform to EPSG:4326. (landed `5972466`)
  - Surfaced by: Eng architecture — eqc string is live
  - Files: src/catalog/datacatalog.h, src/io/horizonbinner.cpp
  - Verify: QgsCoordinateTransform to EPSG:4326 fails; authid empty
- [x] **T14 (P1, human: ~4h / CC: ~30min)** — SEG-Y fallback — Delete the offset 8/20 branch before the range check. (landed `10c9cbf`)
  - Surfaced by: Eng — this file would take that branch
  - Files: src/io/segyreader.cpp, tests/tst_segy.cpp
  - Verify: inline word 0 and varying CDP still uses 1315 + trace/641
- [x] **T15 (P1, human: ~1d / CC: ~40min)** — thickness path — Stop runThicknessChain from IDW-then-polygonize for this isochore. (landed `781914e`)
  - Surfaced by: Eng — mappingworkflow.cpp calls deriveFaciesPolygons
  - Files: src/workflow/mappingworkflow.cpp, tests
  - Verify: a metre raster is not passed to polygonize; D62 grid mismatch writes nothing
- [x] **T16 (P1, human: ~4h / CC: ~20min)** — one publish gate — Store the PDF id, SHA-256, and residual rows on the MapVersionStore row the button reads. (landed `0b8de17`; residual coverage + PDF asset + SHA on MapVersionStore)
  - Surfaced by: Eng — ReleaseStore is not that button
  - Files: src/metadata/mapversionstore.cpp, src/ui/paleomainwindow.cpp
  - Verify: old rows load unpublished; export does not edit a frozen row

jq is not installed. Eng task JSONL was not written.

### Eng completion

- Step 0: scope accepted as-is. No features cut.
- Architecture: 6 issues written into the plan
- Code quality: 2
- Test review: diagram above, gaps listed in the test plan file
- Performance: 1, bytes-read bound already in section 7
- Failure modes: 3 critical until T13–T16 land
- Outside voice: unavailable
- Parallelization: sequential
- Unresolved decisions in this review: 0. The containing-cell rule is a taste item for the final gate.

### CEO pass 2 (2026-09-26)

Second autoplan pass over the implemented plan. Spec-review dispositions recorded in ~/.gstack/projects/paleo_workstation/ceo-plans/2026-09-26-project-area-progress-review.md.

<!-- autoplan-pass2-ceo-scope -->
- Retire or wire `SeismicPreviewPanel::setCatalog`: the data-page preview tabs already own the external SHA-256 verify contract; the standalone bottom-dock panel is no longer hosted. Prefer retirement; keep or port its test coverage before deleting the class.
- `DataCatalog::linksForEntity("")` must not silently mean "all unresolved links". Introduce an explicit `unresolvedLinks()` (or equivalent guard) so callers passing an empty well id by accident cannot obtain the unresolved set.
- `DataCatalog` validates `schema_version` on `load()` and refuses unknown versions; `save()` rotates the previous file to `catalog.json.bak` so the §9 rollback sentence has something to restore.
- Drift errata (superseded text, not rewritten history): the eng-accepted corner-offset sentence reads offsets 180/184 but implementation and the corrected body use 72/76 (Source X/Y); early bullets naming ReleaseStore as the publish gate are superseded by MapVersionStore per audit row 31; §4 "这一阶段不做 PDF 内嵌翻页" is superseded by the shipped soffice→DERIVED→QtPdf preview (`6dbeec7`).
<!-- /autoplan-pass2-ceo-scope -->

Unresolved for the gate: §4's off-UI-thread IO clause and the 任务 1s/10s progress bar are plan requirements not met by the synchronous implementation; the 「正在建立道索引」 hooks exist but are unobservable while reads stay synchronous. Implement or formally defer — user decision.


## GSTACK REVIEW REPORT

| Review | Trigger | Why | Runs | Status | Findings |
|--------|---------|-----|------|--------|----------|
| CEO Review | `/plan-ceo-review` | Scope & strategy | 1 native | issues_open | T1–T5 written into the plan; 17 audit rows; the phase-D/publish cut declined, C–E stay |
| Outside Review | codex (gpt-6-astra) | Independent 2nd opinion | 0 | unavailable | HTTP 400 model_unusable in every phase; one-line fix GSTACK_CODEX_MODEL |
| Eng Review | `/plan-eng-review` | Architecture & tests (required) | 1 native | issues_open | architecture 6, code quality 2, test gaps in the test-plan file, performance 1; 3 critical failure modes until T13–T16 land |
| Design Review | `/plan-design-review` | UI/UX gaps | 1 native | issues_open | 10 issues; overall 3/10 → 8/10 |
| DX Review | `/plan-devex-review` | Developer experience gaps | 1 native | issues_open | 13 issues; overall 4/10 → 6/10 |

- **Approval:** APPROVED — /autoplan Final Approval Gate option A (approve as-is), 2026-09-26. All 7 taste choices accepted as recommended (10 ms cutoff; horizon isochron scaled by well TVD velocity; preview at one third; validation double-click stays on 验证; geotransform origin at the outer corner; containing-cell residual sampling; engineering CRS with the eqc string deleted). The single-voice challenge to cut phase D and the publish machine was declined per the user's standing direction; phases C–E stay.
- **OUTSIDE COVERAGE:** Codex (gpt-6-astra) was preflighted in all four phases and returned HTTP 400 model_unusable; outside status unavailable everywhere. Consensus is N/A for every phase. One-line fix: GSTACK_CODEX_MODEL=<supported-model>.
- **VERDICT:** CEO + DESIGN + DX + ENG CLEARED — plan APPROVED, ready to implement T1–T16 in the recorded order (catalog, SEG-Y reader, time-depth tool, mapping workflow, version store).

NO UNRESOLVED DECISIONS

---

## GSTACK REVIEW REPORT — pass 2 (implementation-vs-plan audit, 2026-09-26)

Second `/autoplan` pass. Pass 1 approved the plan pre-implementation; this pass audits the **shipped implementation** (master `090ffa2`, ctest 54/54, real-data smoke green) against the plan text and re-reviews residual scope.

| Review | Trigger | Runs | Status | Findings |
|--------|---------|------|--------|----------|
| CEO | `/autoplan` phase | 1 spec-review subagent + 1 native voice | issues_open | Spec loop: 2H/6M/6L (drift errata, catalog durability, sync-IO contradiction). Native: F1–F11 (frame vs product; 4 text contradictions; coverage-numbers loophole; IDW credibility; hardcoding) |
| Design | pass 2 | 1 implementation audit + 1 native voice | issues_open | Audit: 8/6/6/7/5/6 per pass — sync IO hides every state; residual table inert; tokens uncentralized; fonts/focus-ring unshipped. Native: F1–F12 (status-tag contrast, relocate dead-end, wells-on-map unspec'd, preview budget, unnamed thickness trigger) |
| DX | pass 2 | 1 contract audit | issues_open | Contracts 1/3/5/6 pass; contract 2 drifts HIGH (HZ28 row editable, kTypes vocabulary, XML default, 4th outcome, no retry, missing CRS step); contract 4 MED-HIGH (residual table not navigable) |
| Eng | pass 2 | 1 implementation audit + 1 native voice | issues_open | Audit: ver-N seq reload bug (deterministic dup ids), PALEO_INLINE_* never written (dead navigation), per-click re-index+re-hash, corrupt-catalog overwrite, /tmp derived outputs. Native: F1–F21 (TD sentinel live bug, nodata-cell rule, fallback grid undefined, batch writes, trust-boundary load validation) |
| Outside | codex/claude/gemini | 0 | unavailable | codex: TLS handshake EOF on every attempt (2 probes, session started, transport dead); claude: broken shim; gemini: no auth configured. Consensus cells N/A. |

### Progress accounting (shipped vs plan)

| Phase | ~Complete | Evidence |
|---|---|---|
| A catalog/entities/managed-RAW/preview | ~90% | `tst_catalog`/`tst_import`/`tst_datapreview` green; links+note, BOM, SHA verify, 9 tab types |
| B seismic section | ~90% | ordinal index, 72/76 corners, per-line decode; **missing:** cached reader, real indexing state |
| C D61 mapping | ~85% | isochron×IDW²(Vint), hull clip, residual semantics all correct per plan (A1=36.22m, 15过/4超/1警); **missing:** raster-level isochron≤0 rule, derived-asset registration |
| D auxiliary | ~90% | reference links, PDF preview chain (beyond plan text — recorded supersession) |
| E chips/versions/publish | ~85% | MapVersionStore gate + PDF+sha+residual completeness; **missing:** residual-table navigation, top-XY sampling in gate |

**Drift errata** (text vs code; text intentionally not rewritten, provenance preserved): eng-block corner offsets say 180/184 → correct is **72/76** (implemented); early bullets name ReleaseStore for publish → **MapVersionStore** won (audit #31, implemented); §4 「不做 PDF 内嵌」 → superseded by shipped soffice→DERIVED→QtPdf; L227 "otherwise decode" conflicts with L248 → **L248** correct (no decode on double-click); L241 「时间等厚」 vs L172/L250 → shipped **「等厚」** (name without 时间 per L172); chip tooltip wording variants — pin 「这一阶段还没有这个层位的栅格」; L229/L253 releases-table columns → realized as `map_versions` columns + `state='Published'` (implemented).

### New implementation tasks (pass 2, prioritized)

- [x] **T17 (P1)** Restore `m_versionSeq` on reload and reject duplicate version ids in `addVersion` — plus the symmetric fix: explicit `ver-N`/`ast-N` ids advance `m_versionSeq`/`m_assetSeq` so `nextVersionId()`/`nextAssetId()` never reissue (`versionSeqRestoredAfterReload`, `duplicateVersionIdRejected`, publish-path regression in `tst_mapping`).
- [x] **T18 (P1)** Sentinel-filter TD rows with `TIME(ms) == -99999` at parse and again at the interpolation boundary; non-finite numeric values are rejected (`wellfileparsers.cpp`, `timedeptool.cpp`).
- [x] **T19 (P1)** `isochronMs <= 0` or non-finite → nodata in the thickness raster; reject non-finite/invalid output thickness too (`mappingworkflow.cpp`).
- [x] **T20 (P1)** Refuse imports after catalog-open failure (`refusesWrites()`/`m_catalogReady` + per-mutator `ensureOpen`), schema-version gate (missing key = current version; explicit mismatch refuses), QSaveFile atomic write + `.bak` rotation — **landed** (`refusesWritesAfterFailedOpen`, `rotatesBakAndVerifiesWrite`, `refusesUnsupportedSchemaVersion`). 余项（catalogOpenFailed 状态栏红胶囊 + 导入禁用 + 恢复）已由 wave-3 ux-consistency 落地（`statusCatalogError`，merge `f79945a`）。
- [x] **T21 (P1)** `writeHorizonGeoTiff` stores inline/xline ranges and bin counts as GDAL metadata; validate P1/P2/P3 presence, numeric geometry, and agreement with `Grid_size` before binning (`horizonbinner.cpp`).
- [x] **T22 (P1)** Folder confirm: classifier-vocabulary combos (label↔type via item data, `tabular` present, `tops` absent), HZ28-6-1 rows locked, 「参考资料」 dir defaults `reference` but editable, valid-type-only overrides, per-row 「重试」 via `importFolderRow`, CRS sentence in both dialogs, four-count summary kept (D3).
- [x] **T23 (P2)** Cache `SegyReader`+SHA verify per asset (one open/index/hash per asset, not per line change); then evaluate worker-thread index/decode/binning/import per §4 — **landed via D1a** (`9ed4abd`: per-asset reader cache + 每会话 SHA 复验一次；worker 线程化由 D1 一并覆盖)。
- [x] **T24 (P2)** `residualTable` double-click emits `locateRequested` with the same layerId/WKT/payload shape as issue rows; the section button arms from whichever table was selected last (`armedPayload` property).
- [x] **T25 (P2)** `residualSummaryJson` samples via `pickSamplePointForWell` (top XY, wellhead fallback) with the same containing-cell/1-ULP rules as the validation table; missing raster emits a `RASTER_MISSING` issue the summary row echoes.
- [x] **T26 (P2)** Derived rasters → `artifacts/derived/` + DERIVED catalog versions (thickness, ONNX, processing outputs) — **landed wave-3 derived-publish** (`tst_derivedassets`; `/tmp` 死链清零，merge `72dc070`)。
- [x] **T27 (P2)** Status-bar copy 「工程坐标 · 米 · 未投影」 + zh validation messages (SRC_MISSING/DUP_HORIZON/BUSY) — **landed**；余项（status 胶囊、中性「未计算」、coordinate_status 中文化）已由 wave-3 ux-consistency 落地（merge `f79945a`）。
- [x] **T28 (P2)** Attach/primary actions re-keyed by link identity (assetId+role), not `links()` index; confirm strip survives `changed()`; undo restore (demoted primary + note) — **landed wave-3 ux-consistency**（merge `f79945a`；会话内 undo，跨 reload 持久化由 D4 决定保持会话域）。
- [x] **T29 (P2)** 「在地图上显示」 two-way sync with layer visibility + zoom + flash — **landed wave-3 ux-consistency**（`flashHorizonLayer` + `visibilityChanged` 双向同步；合并接缝修复 `175db2a`：图层树节点 `paleo.visSync` 属性去重替代 lambda+UniqueConnection）。
- [x] **T30 (P2)** Retire `SeismicPreviewPanel` (class + test + CMake) — audit row 34; also removed the `paleo.seismic.ctx` property and dead `IssueLocatorFilter`/`zoomToLayer` fallback registration.
- [x] **T31 (P3)** Empty states for asset table/map/layer tree; unresolved multi-well tab dead-end copy + pointer to 挂到这口井; 「查看未决」 post-confirm filter — **landed wave-3 ux-consistency**（`setUnresolvedFilter` + 空态，merge `f79945a`）。
- [x] **T32 (P3)** Font registration (Noto Sans SC + JetBrains Mono vendored), 2px #1B73D0 focus ring, Mono 9pt on all numeric surfaces, accessible-name coverage, tab overflow policy — **landed wave-3 ux-consistency**（merge `f79945a`）。
- [x] **T33 (P3)** `DataCatalog::BatchSave` coalesces folder import and dedup-attach saves (one `save()`+`changed()` per batch, nested-safe); `linksForEntity("")` → empty + `unresolvedLinks()`; `unsafeVersionSegmentReason` skips bad-segment versions on load; symlink canonical re-check both at enumeration and again before bytes are read (`batchSaveCoalescesWrites`, `unresolvedLinksIsTheExplicitAccessor`, `unsafeManagedPathSkippedOnLoad`).

### Unresolved decisions (gate)

**All fifteen resolved** at the pass-2 approval (below); landing waves annotated inline.

- **D1 — Async IO (plan §4 clause unmet):** implement worker-thread index/decode/binning/import now (M+ effort, satisfies 「正在建立道索引」 observability + enables 任务 progress), or formally amend §4 to defer with trigger (real-machine freeze measurement). CEO recommendation: measure first on real machine, then decide scope — but the plan text must be amended either way since code currently contradicts it.
- **D2 — 任务页 progress bar (1s bar / 10s byte-linear ETA):** same disposition as D1 — coupled; if async lands, implement; if deferred, strike the clauses.
- **D3 — 「跳过」 as a fourth confirm outcome:** keep (honest for symlink/non-regular skips) or fold into 失败? Current ships 4 counts vs contracted 3.
- **D4 — Undo semantics:** restore demoted primary + preserve note + persist across reload — or keep session-scoped minimal undo.
- **D5 — Retyped-to-`well_head` rows rejoin phase-1 ordering** (currently ordered on pre-override type — wells created too late). Accept as-is or re-order by final type.
- **D6 — Wells-on-map spec:** wells layer + symbol + zoom-to-content on import + map→table selection (design F3; currently unspecified and unimplemented).
- **D7 — Preview vertical budget:** plan-fixed ⅓ vs ≥60% splitter range / maximize affordance (design F4; at 1280×800 the seismic preview is ~150px).
- **D8 — Thickness invocation:** name the control + placement + enablement (design F5; currently runs via constraint flow without a labeled trigger).
- **D9 — Publish hard-gate vs PALEO_QGIS_PLAN warn-not-disable guidance:** acknowledge deliberate override in plan text.
- **D10 — Minimum numeric-residual gate for publish** (e.g. ≥15/20 numeric) beyond "row or reason" completeness (native CEO F3).
- **D11 — Provisional georeferencing tier for reference GeoJSON** (manual affine + watermark) to exercise the real facies pipeline (native CEO F9) — scope expansion question.
- **D12 — `uwi`/`aliases` legacy entity fields:** strip (plan §62) or keep as dormant compat.
- **D13 — `QgsTaskManagerWidget` vs custom `TaskPanel`:** DESIGN.md control mapping unmet — adopt QGIS widget or record deviation.
- **D14 — `Qt6 WebEngineWidgets` hard REQUIRED build dep** vs optional-with-fallback packaging policy.
- **D15 — ONNX end-to-end fixture inference** (prove the differentiated path, not just the gate) — keep as constraint-only or add one smoke inference.

**OUTSIDE COVERAGE:** unavailable this pass (codex TLS handshake EOF ×2 attempts incl. HTTPS fallback; claude shim broken; gemini unauthenticated). All findings are native-voice + implementation-audit only. Prior pass likewise unavailable.

**REVIEW LOG:** pass 2 run 2026-09-26; reviewers: spec×1, design×2, dx×1, eng×2 (6 subagents) + inline CEO analysis; artifacts `autoplan-ceo-jUNQhI`, `autoplan-design-t0tR1p`, `autoplan-dx-X3unPM`, `autoplan-eng-k3lfjP`; methodologies `ceo-3CGA7C`, `design-CUgus6`, `dx-qaWNBF`, `eng-zT7FfC`. T1–T16 annotated as landed/partial with commit refs. Decision Audit Trail extended rows 34–46.

### Pass-2 approval (2026-09-26)

- **APPROVED** at the final gate: implement T17–T22 (all P1) and T23–T33; D1/D2 async IO + task progress bar → **implement**; D6/D7/D8/D11 UX expansions → **all accepted**; taste batch → accept defaults: keep 「跳过」 fourth count (D3), undo restores demoted primary + note (D4), retyped-to-well_head rows rejoin phase-1 ordering (D5), publish hard-gate acknowledged as deliberate override of PALEO_QGIS_PLAN warn-not-disable guidance (D9), publish gate adds a minimum numeric-residual count (D10), strip `uwi`/`aliases` legacy fields (D12), WebEngineWidgets becomes an optional build dep (D14), ONNX gains one end-to-end fixture inference (D15). D13 QgsTaskManagerWidget deferred (custom TaskPanel gets progress instead, lands with D1).
- Thickness layer title pinned: 「D61–D62 等厚（米）」(L172/L250 normative; L241 时间等厚 superseded).
- Residual sampling: containing cell, half-open (L174/L264 normative; L227 nearest-center + "otherwise decode" superseded).

**VERDICT:** Implementation substantially faithful to plan (~88% weighted across phases); 6 P1 engineering tasks and the D1/D2 threading decision stand between current state and full plan conformance. Plan text contains 7 superseded/contradictory passages documented as errata rather than rewritten.

### Pass-2 Wave-1 implementation record (2026-09-27)

Merged into `master` (`ce746b3` → `61cdb0d`): `b52114e` catalog durability, `0e4010c` mapping/validation, `f4b7335` folder confirmation UI, `1af8d1e` panel retirement, `61cdb0d` seam fixes. Verification: `ctest` **53/53**; real-data smoke (`PALEO_REAL_PROJECT_AREA`, 1.4 GB): `tst_import` + `tst_smoke_realdata` both pass (60 files / 113 folder rows / 0 failures).

Landed: T17–T22 (T20 pending only the statusbar sink for `catalogOpenFailed`), T24, T25, T30, T33; D3 (fourth 「跳过」 count kept) and D5 (retyped `well_head` rows rejoin phase 1 via `effectiveFolderType`) exercised by tests.

Merge seam notes: `ce746b3` had already landed T17/T18/T19/T20-core/T21 with stricter primitives (`ensureOpen`, `QSaveFile`, per-field P-row errors); the branch work was rebased onto it and only unique deltas layered — `BatchSave`, `.bak` rotation, `unresolvedLinks()`, `m_openError`/`catalogOpenFailed`, load-time bad-segment skip, symlink TOCTOU canon re-check, residual-table navigation payloads, `RASTER_MISSING`, publish-gate top-XY sampling, folder-UI vocabulary/lock/retry/CRS sentence. Integration also fixed `addAsset("ast-N")` not advancing `m_assetSeq` (same bug class as T17).

Remaining per approval: Wave-2 D1/D2 async IO + task-page progress (SEG-Y index, SHA hash, LAS parse, horizon binning, single-line decode; progress+ETA, stale-result discard); UX expansions D6 well overlays / D7 preview budget / D8 thickness trigger / D11 provisional GeoJSON affine registration; Wave-3 D4 undo restore, D10 minimum numeric residuals in publish gate, D12 strip `uwi`/`aliases`, D14 optional WebEngine dep, D15 end-to-end ONNX fixture inference; T23 per-asset SEG-Y reader/SHA cache, T26 derived rasters → `artifacts/derived/` + DERIVED versions, T27 capsule/未计算/coordinate_status 中文 remainder, T28–T29, T31–T32.

### Pass-2 Wave-2 implementation record (2026-09-27)

Landed on `master` (`545f1ad` … `8a65531`): verification `ctest` **54/54**; real-data smoke (`PALEO_REAL_PROJECT_AREA`, 1.4 GB) green — `tst_import` 35/35 (folder 113 rows, 0 failures) and `tst_smoke_realdata` 3/3 (60 files, A1 thickness 36.22 m, residuals 15/4/1).

- **D2 TaskPanel + PaleoTaskService** (`60f6fc7`): QThreadPool task registry — byte progress, ≤10 s window ETA, cooperative cancel, busy/free layer marking; TaskPanel shows progress rows alongside the existing busy mirror.
- **D1a/D1d seismic async** (`9ed4abd`): SEG-Y open/index/single-line decode via task service with per-asset reader cache, SHA re-verify once per session, generation-discard for stale decode results; `SegyReader::open` gains a byte-progress + cancel hook (sync path kept when no task service is wired).
- **D1b/D1c import/folder/horizon async** (`2935669`): `catInvoke` BlockingQueued marshaling — `DataCatalog` is only ever touched by its owning thread; single-file and folder imports run in the pool with `(done,total,path)` progress and cooperative cancel; sync fallback preserved without a task service.
- **D6 wells on map** (`545f1ad`): `wells.geojson` from catalog entities, declared/instantiated vector layer with point symbols + labels, zoom-to-content after import, map selection → asset-table row selection via linked entity ids (single `QItemSelection`, not repeated `selectRow`), 4 px click tolerance in `PaleoSelectTool`.
- **D7 preview height budget** (`4681180`): first tab expands preview to ≈60 % (was ⅓); corner `QToolButton` 最大化预览/还原预览 pins the map to its min-height shell and restores prior sizes; empty state unchanged, automatic sizing never overrides user drags.
- **D8 thickness trigger** (`03f22d2`): compose-page button renamed 「生成 <层位> 等厚图」, enabled only with an active horizon chip (disabled tooltip carries the reason); wired to `activeHorizonChanged`.
- **D11 provisional GeoJSON registration** (`8a65531`): `io/geojsonaffine` scale→rotate→translate transform; preview-tab dialog with live transformed-bounds preview; DERIVED version (extra `provisional`+affine, parent=RAW) → 「临时配准 · name」 dashed-orange vector layer + canvas watermark `PaleoWatermarkDecoration`; decoration manager now attached to the real canvas (previously test-only).

Notable seam fixes inside the wave: `QThreadPool::global()`→`globalInstance()`, `std::atomic_bool` for Qt 6, worker-thread `QVERIFY` misuse removed, `QgsMarkerSymbol::createSimple` unique_ptr ownership, Qt 6 `selectRow` replacement semantics, position-level GeoJSON coordinate arrays (Point/MultiPoint) were being skipped by the recursive transform — caught by the new test.

Remaining per approval: Wave-3 — D4 undo restore, D10 minimum numeric residuals in publish gate, D12 strip `uwi`/`aliases`, D14 optional WebEngine dep, D15 end-to-end ONNX fixture inference; T26 derived rasters → `artifacts/derived/` + DERIVED versions (partially served by D11's own derived registration), T27/T28–T29/T31–T32 remainder; T23 largely superseded by D1a's per-asset reader/SHA cache.

### Wave-3 implementation record (2026-09-27, 三 PR 并入 master)

`72dc070` derived-publish（PR #13）、`82dff27` model-hardening（PR #14）、`f79945a` ux-consistency（PR #15）、接缝修复 `175db2a`；随后 codex 评审修复 PR #12（`c908e1e`，36 文件：managed 路径越界/符号链接、EBCDIC EndText、变长道、inline↔Y 轴修正、Windows CI + deb 锁定）。

落地：D4 undo 跨 reload 恢复降级 primary + note；D10 发布门最小数值残差数；D12 剥 `uwi`/`aliases`（旧 catalog 键装载时静默忽略）；D14 WebEngineWidgets 可选依赖；D15 ONNX 端到端 fixture 推理；D13 显式递延（自定义 TaskPanel 拿进度，记偏差）；T26 `artifacts/derived/` + DERIVED 登记 + `/tmp` 死链清零；T27 胶囊/未计算/coordinate_status 中文化；T28 链接身份重键 + confirm strip 存活；T29 双向同步 + flash；T31 空态 + 「查看未决」过滤；T32 vendor 字体 + 焦点环 + mono 数字面 + accessible-name；T20 余项 statusCatalogError 胶囊；速度模型 + line-geometry↔CDP；schema 迁移；SEG-Y 合成 fixture；算法 harness；算法 + CRS 审计文档；渲染钉死（vendor 字体 + 显式浅色 palette）。验证：`ctest` 61/61 + 真数据 smoke 双绿。

### Wave-4 implementation record（2026-09-27，两 PR 并入 master）

`2c1c8e1` runtime-resilience（PR #17）：§38 崩溃报告按本地优先落地——致命信号 fd 落盘 + `.running` 脏退出检测 + 非模态重启提示 + `docs/CRASH_REPORTING.md`；「重新定位文件」恢复路径——`relocateVersionSource` 流式 SHA-256 复验、不一致拒解、同 SHA 追加外链版本（版本不可变模型），预览死胡同挂入口。

`8fc7bb2` area-parametrization（PR #16）：`AreaRules` 工程级参数 seam——8 层序界面名单/分类器目录规则/SEG-Y 道号索引四偏移（188/192/8/20）/411×641 ONNX 门全部经 `project_area.json` 可覆盖（默认=本工区值，语义逐分支等价）；Phase-0 收口——vendor superbuild 骨架、`PLATFORM_MATRIX`、`APP_ONLY_AUDIT`、`BUILDING.md` TTHW。验证：`ctest` 67/67；真数据回归 A1=36.223053 m 与 wave-3 基线逐字节一致。

### Data-fabric 采纳波次 + ribbon（2026-09-27，四包 + 一分支并入 master）

语义参考 `paleo_project/main`（data-fabric-v11 契约），只采纳数据管理/流转、不采纳 UI；全部锚在工程生命周期（无工程不导入不落盘；catalog→.qgz 同一写队列）。`docs/DATA_FABRIC_ADOPTION.md` 是采纳规格。

- **RoleRegistry**（`f7f64eb`→`d2bcaf1`）：工程作用域角色词表（9 井 + 7 测网角色，`project_area.json` `roles` 覆盖，`catalog.roleRegistry()`）；`maxCount=0` 统一——单 primary 约束由 catalog 链接不变量承载。
- **commit-coord-lite**（`cb57b53`→`12aa9b1`）：`PaleoProjectStore::commitAll(opId,digest,catalogCommit,qgzWrite)`——journal 先于执行、阶段推进（queued→catalog_done→qgz_done→complete）、complete 重入 no-op、中道崩殂只报告不重放（`recoverCommitJournal` 挂 projectOpened）。
- **EntityView + ordinal + staleness**（`bbea533`→`6e34d9d`）：`EntityAssetLink.ordinal` 持久化排序；`entityDataView` 角色槽（registry 全角色枚举、primary/members/unresolved 分桶、derivedProducts、missingSources）；`downstreamClosure` BFS 闭包；`addVersion` 同一原子写标下游 `extra["stale"]`。
- **IngestPlan 三段式**（`a74ab1a`→`6d1f3cb`）：`buildIngestPlan` 纯函数（扫描→分类→shp 族归组→身份匹配→≤200MB sha 去重→primary 建议，歧义恒未决不猜）；`executeIngestPlan` 幂等执行（path+sha 已注册即跳过→续跑天然）；单文件/文件夹同语义；确认表「重复→跳过/新版本」决策列。真数据 116 行含 1.01 GB SEG-Y 全绿。
- **ribbon-icons**（`a99a42a`→`0615050`）：`PaleoIcons` 出口（`QgsApplication::getThemeIcon` qrc 直取 + QPainter 自绘补缺）；编辑条 icon-over-text、chips 独占 `ribbonActionRow`、动作钮 icon-beside-text、预览角落钮自绘图标。

Deferred as designed: working-copy 编辑会话、trash/retention/pin、typed RunPort（派生链短，扁平 parentVersionIds 够用）、catalog.sqlite（TODOS P3 触发条件未满足）。待接线接缝：EntityView→DataPage、`markDownstreamStale`→预览 sha 失配、发布门 stale advisory、addLink 词表强制——wave-5（p5a/p5b/p5c）覆盖。存量债：`attachWorkflows` 非幂等（p5c 修）、计时类用例负载敏感（环境性）。
