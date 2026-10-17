# Agent A 深度审计报告（src/domain / src/catalog / src/io / src/metadata）

- worktree：`paleo_workstation-full-review-20261018`（分支 `codex/full-review-fix-review-fix-20261018`，基线 366aef97）
- 审查员：Agent A（只读）。审查日期：2026-10-18。
- 排除库：`01-conflict-matrix.md` + `99-external-issues-snapshot.txt`（#460–#503 全文通读）+ `/tmp/paleo-audit/issue-*.txt`。
- 总体印象：本树已吸收 2026-10-15 swarm review 的大部分修复（#461/#462/#464/#466 尺寸闸/#467 预算/#476/#477 多项均可确认已修），四个域的解析与存储面整体高度加固。本轮新发现以「未纳入既有 issue 覆盖面的遗漏角落」为主，无 P0。

---

## 1. 覆盖清单

审查方式：**逐行** = 全文通读；**部分逐行** = 指定区段逐行 + 其余接口/调用点；**接口+调用链** = 头文件契约 + 调用方核对（含 grep 排查），未逐行读实现。
风险级为本轮评估（高=不可信输入/持久化核心，中=状态一致性，低=纯内存值对象/工具）。

### src/io

| 文件 | 审查方式 | 风险级 | 关键类/函数 | 已核查调用链 | 测试对应 |
|---|---|---|---|---|---|
| segyreader.cpp (1517) | 逐行 | 高 | open/openCached/scanParallel/resumeScan/decodeTrace/snapshot/restore | openCached←seismic 装载（qgis/services 层经 m_byInline/readInline 内部索引，均安全）；restore 仅持 load() 结果 | tst_segy |
| segyindexstore.cpp (389) | 逐行 | 高 | load/loadForResume/decodePayload/save | openCached 内部；decodePayload 计数闸+容量闸确认 | tst_segy（缓存族） |
| dlisparser.cpp (1444) | 逐行 | 高 | walk/parseEflr/consumeFdata/ByteSource/parse/parseHeader | WellLogRead::readWellInfo/parseHeader/parseCurves/parseDoc；#467 预算闸确认在位 | tst_dlis |
| lisparser.cpp (1155) | 逐行 | 高 | TifView/LogicalReader/walkRecords/parse/parseHeader | 同上；#467 body 预算确认；TifView 段索引残余见排除节 | tst_lis |
| lasparser.cpp (1053) | 逐行 | 高 | parse/parseHeader/readWellInfo/parseDoc/parseAsciiRows/parseRange/parseDepthRange | WellLogRead 分派、lascache、井剖面/属性建模（#466 调用链） | tst_las*、tst_cache_las |
| welllogread.cpp (162) | 逐行 | 中 | detect/readWellInfo/parseHeader/parseCurves/parseDoc | 导入预览与井曲线装载统一入口 | tst_welllogread |
| ziparchive.cpp (634) | 逐行 | 高 | zipListBytes/zipExtractBytes/zipBuildArchive/zipWriteArchive | sfpkgreader、outsourceworkbook（xlsx）；#477 CRC32 确认已修 | tst_ziparchive |
| sfpkgreader.cpp (699) | 逐行 | 高 | readSfPackage/parseNpy/readManifest | 单因素成果包读回（workflow/services）；#477 端序、#216/#382 闸确认 | tst_sfpkg |
| outsourceworkbook.cpp (1204) | 逐行 | 高 | readSpreadsheetMl/readXlsx/readWorkbook/readWellCoordinateTable/scanWorkbookDirectory | previewdoc.cpp:539、cuttingsdoc.cpp:517、mkproject.cpp:91/144、外委表导入 | tst_io_outsource*、tst_io_workbook_edges |
| wellcompositexml.cpp (1025) | 逐行 | 高 | parseComprehensiveWellXml(Data)/writeComprehensiveWellXml/collectSheetRows/parseDeviationSurvey/parseTimeDepthTable | previewdoc.cpp:399（综合柱状图预览）、wellfacies 侧 | tst_wellcomposite、tst_wellfacies |
| streaming.cpp (542) | 逐行 | 中 | ChunkedLineReader/forEachLine/forEachRecord/GeoJsonScanner | 井文本流式读、GeoJSON 预扫描（导入预览） | tst_streaming |
| partialread.cpp (211) | 逐行 | 中 | readTextPartial/sweepTempFiles/WriteGuard | 版本对比部分读、缓存清尾 | tst_partialread |
| constraintstore.cpp (1004) | 逐行 | 高 | Transaction/appendInternal/replaceHorizon/journal/migrateLegacyCrsLocked/copyAudit | 约束编辑/快照/迁移（workflow 侧门面）；#457/#488/#496 加固确认 | tst_constraint*、tst_singlefactor_constraint* |
| cachecore.cpp (432) | 逐行 | 高 | writeCacheFileAtomic/readCacheFile/cacheZstd*/cacheio | segy/las/attr 缓存族共用底座；#477 序号后缀、#217 闸确认 | tst_cache_io |
| encodingdetect.cpp (201) | 逐行 | 中 | isStrictUtf8/detect/decodeText/stripBom | lasparser/wellfileparsers/streaming；#477 修复确认 | tst_encodingdetect |
| wellfileparsers.cpp (398) | 逐行 | 高 | parseWellHeadText/parseWellTopsText/parseTimeDepthText/parseDeviationText | 井位/分层/时深/井斜文本导入 | tst_wellfileparsers |
| attrgridout.cpp (257) | 逐行 | 中 | writeTimeSliceGeoTiff/readTimeSliceGeoTiffSummary | 属性时间切片缓存写出/读回 | tst_attrgridout |
| timedeptool.cpp (153) | 逐行 | 中 | interpolateTimeMs/interpolateDepthAtTimeMs | mappingworkflow/mapversioncontroller | tst_timedeptool |
| sattrio.cpp (684) | 接口+调用链（#477/#518 排除项核对） | 高 | SattrVolumeWriter/readCell | 单因素属性体 | tst_sattr |
| rasterpyramid.cpp (613) | 接口+调用链（#477 排除） | 中 | 瓦片自愈协议 | 栅格金字塔 | tst_pyramid |
| ingestplan.cpp / dataimport_*.cpp（8 文件 ~3100 行） | 接口+调用链（头文件+关键入口 grep） | 中 | IngestPlan/DataImportService 族 | 导入会话/台账（opt-01 域） | tst_import、tst_folderimport |
| cuttingsdoc / horizonbinner / geojsonaffine / faciescoveragereader / lascache / lasalias / lrucache.h / shacache / cachebudget / sfpkgwriter / perffixtures / benchreport | 接口+调用链（未逐行） | 中低 | 岩屑文档/层位分箱/仿射/覆盖读取/缓存族 | 各自消费方 | 见「未覆盖区域」说明 |

### src/metadata

| 文件 | 审查方式 | 风险级 | 关键类/函数 | 已核查调用链 | 测试对应 |
|---|---|---|---|---|---|
| paleoprojectstore.cpp (568) | 逐行 | 高 | enqueueWrite/saveAll/commitAll/busy-token/commit journal | 全部 store 单写队列（constraintstore/mapversioncontroller 等）；#462/#464 修复确认 | tst_versions、tst_storagegovernance |
| atomicfile.h (352) | 逐行 | 高 | paleoWriteFileAtomic/paleoSyncFile/PaleoAtomicTransaction | frameworkstore:174 等；#461 fsync 修复确认 | tst_atomicfile |
| mapversionstore.cpp (1084) | 逐行 | 高 | saveVersion/publish/commitPublicationPlacement/recoverInterruptedPublications/validatePublishGates | mapversioncontroller（同步/异步）、发布面板；#476 收口确认 | tst_versions、tst_mapsnapshot |
| wellsectionstore.cpp (521) | 逐行 | 高 | save/load/escape 族/saveInterpretationSelection | 剖面工作流（经 enqueueWrite）；#476 keepEmpty 修复确认 | tst_wellsection_store |
| paleoprojectfile.cpp (409) | 逐行 | 高 | readProjectFile/writeProjectFile/georeference 族 | 工程打开（projectopen） | tst_projectfile |
| metastore.cpp (161) | 逐行 | 高 | openConnection/closeConnectionsFor/ensureUserVersion | 全部 sqlite store；#476 qHash 碰撞修复确认 | tst_metastore |
| layermanifest.cpp (258) | 逐行 | 高 | upsert/removeMany/readAll/forHorizon | 图层声明（paleomainwindow_attach 经白名单头） | tst_layermanifest |
| mapsnapshot.cpp (127) | 逐行 | 高 | prepareMapSnapshot/verifyPreparedMapSnapshot | mapversioncontroller 异步快照 | tst_mapsnapshot |
| faultsetstore.cpp / wellsitingstore.cpp / releasestore.cpp / layouttemplatestore.cpp / projectlock.cpp / processprobe.cpp | 接口+调用链（未逐行；releasestore/layouttemplatestore 的 #476 readOnly 项未复核当前态） | 中 | 各专用 store | 各自面板/工作流 | 见「未覆盖区域」 |

### src/catalog

| 文件 | 审查方式 | 风险级 | 关键类/函数 | 已核查调用链 | 测试对应 |
|---|---|---|---|---|---|
| datacatalog.cpp (768) | 逐行 | 高 | open/commitStore/applyJournal/staging/adoptPrepared/BatchSave | appcontext 单例、导入/资产操作；线程纪律核对 | tst_catalog* |
| datacatalog_versions.cpp (486) | 逐行 | 高 | addVersion/updateVersionExtra/resolvedVersionPath/sha256FileHex | 导入去重、受管路径解析；#476 版本号唯一闸确认 | tst_catalog_versions |
| catalogstore.cpp (663) | 逐行 | 高 | openProject/recover/rotateFiles/rewritePrimary | datacatalog.open | tst_catalogstore |
| catalogstore_json.cpp (472) | 逐行 | 高 | loadTables/fromJson/toJson | JSON 迁移/兜底恢复 | tst_catalogstore |
| catalogindex.cpp (208) | 逐行 | 中 | rebuild/verifyAgainst/前缀序号 | datacatalog 内部 | tst_catalogindex |
| catalogstore_crud/schema.cpp、datacatalog_entities/assets/links/derivation/export.cpp、frameworkstore、realizationset、roleregistry、entityview、purgelease、catalogroles | 接口+调用链（未逐行） | 中 | CRUD/血缘/导出 | datacatalog 分治 TU | 见「未覆盖区域」 |

### src/domain

| 文件 | 审查方式 | 风险级 | 关键类/函数 | 已核查调用链 | 测试对应 |
|---|---|---|---|---|---|
| deviationsurvey.cpp (418) | 逐行 | 高 | fromStations/pointAt/tvdToMd/arcFrame | 剖面投影、TVD 换算（sectionwellprojector 等） | tst_deviation |
| wellcompositemodel.cpp (59) | 逐行 | 中 | CurveData::valueAtDepth/isEmpty | 综合柱状图渲染 | tst_wellcomposite_framework |
| faciesconsistency.cpp (132) | 逐行 | 中 | computeConsistency | 相一致性页 | tst_faciesconsistency |
| seismic/nicestep.cpp (81) | 逐行 | 低 | NiceStep::Calculate/GenerateTicks | 轴刻度渲染 | tst_nicestep |
| wellsection.cpp (736) | 部分逐行（520–700：wellPathFractions/inferSandShale/SeismicGap::sampleAt/adaptiveClip；其余接口+调用链） | 高 | 井路径分数/GR 岩性推断/地震缝采样 | 剖面工作台 | tst_wellsection |
| constraintsemantics.cpp (859) | 部分逐行（300–450：checkConstraintPart/polylineTouchesRect/canonicalMethod；其余接口+调用链） | 高 | 自交判定（扫描剪枝）/方法词表 | 约束绘编/计划 | tst_singlefactor_constraintuse |
| faultset.cpp (772) | 接口+调用链（#476 P3 + PR #518 触碰 domain/faultset.h → 排除） | 中 | branchesOfVertex | — | — |
| welllogfacies.cpp (572) | 接口+调用链（#476 P3 / #486 触碰 → 排除；步长判据修复确认在 :197 注释） | 中 | 采样判据/模型选择 | 智能预测 | tst_wellfacies |
| 其余（receiptsemantics/welltopsedit/arearules/evidenceconflict/frameworkdiagnostics/sequenceframework/litholexicon/interpretationevidence/singlefactorstrategy/faciescatalog/faciestaxonomy/predictionevidence/projectclassifier/types/timedepthmodel/facieshierarchy/singlefactorrequest/sectionwellprojector/sectiongeometry/wellrecords/wellsitingplan/crossplotsamples/wellnumeric） | 接口+调用链 + 针对性 grep（first/last/除法/int 转换模式，见 Findings 节的排查说明）；timedepthmodel 属 #476/#490 记录项 | 中低 | 值对象/规则引擎 | 各自消费方 | 见「未覆盖区域」 |

grep 模式排查（全四域）：`.first()/.last()/[0]` 无守卫、`static_cast<int>(size)`、除法无零检查——命中处逐一回读上下文，均已确认有守卫（wellcompositemodel 空列 NaN、faciesconsistency `compared>0`、wellsection `total>1e-9`、constraintsemantics 先验 `size()<2`、deviationsurvey 二分前提注释）。

---

## 2. Findings（新问题，可修候选）

### FA-001 [P2] [VERIFIED] 工作簿解析「列填充」内存放大无总量闸：~40 字节输入可换 ~131KB 内存

- **Module**：src/io
- **File:Line/Function**：
  - `src/io/outsourceworkbook.cpp:46-53,183-275,498-499,627-629`（`readSpreadsheetMl`/`readXlsx` 的 `padTo` + 行保留）
  - `src/io/wellcompositexml.cpp:15,533-537,556,941-945,962`（`parseComprehensiveWellXmlData`/`collectSheetRows` 的列填充与 `sheetRows` 全量累积）
- **Root Cause**：三条 SpreadsheetML/OOXML 读路径都有**每格**列号上限（`kMaxColumns`/`kMaxSpreadsheetColumns` = 16384），但没有任何**总量**预算：
  1. `readSpreadsheetMl`：`<Cell ss:Index="16384">` 把 `row`（QStringList）补空到 16384 列（`:270 padTo(&row, column + span)`）；物理 `<Row>` 元素数无上限（`kMaxRows` 只校验 `ss:Index` 属性值），每行整体存进 `sheet.rows`（`:112-113`）。
  2. `readXlsx`：`r="XFD1"` 同样 `padTo(&row, column)`（`:627`）；zip 层 512MB 解压闸对**内存表示**无效——一个 `<c r="XFD1"/>` 约 14 字节即可换一行 16384 槽（Qt6 QList<QString> 每元素 ≥8B ⇒ ≥128KB/行）。
  3. `wellcompositexml`：同样的 ss:Index 填充（`:533-537`）且**整表行先全量收进 `sheetRows` 再处理**（`:556`），行数与列填充均无总量闸。
- **Trigger Condition**：用户导入/预览一个异常或构造的 `.xml`/`.xlsx` 工作簿。真实调用链（已核）：`services/previewdoc.cpp:399`（综合柱状图 XML 预览）、`:539`（工作簿预览）、`io/cuttingsdoc.cpp:517`（岩屑文档）、`app/mkproject.cpp:91/144`（建工程转换）、`scanWorkbookDirectory`（目录批量扫描，每个 .xml/.xlsx 都读）。
- **Observed/Expected**：观察（代码推演，只读约束未跑运行时）：约 400KB 的构造 XML（1 万行 × 每行 1 个 `ss:Index="16384"` 单元格 ≈ 40B/行）⇒ ≥1.3GB 内存；xlsx 路径 512KB 解压 XML ⇒ 同量级。期望：与 ziparchive `kMaxTotalUncompressedBytes`、sfpkgreader `#382`、segyindexstore 计数闸同款的总量预算，超限如实拒收。
- **Reproduction**：构造 SpreadsheetML：`<Worksheet><Table>` + N 个 `<Row><Cell ss:Index="16384"><Data> x</Data></Cell></Row>`；经 `readWorkbook()`/`parseComprehensiveWellXml()` 观察 RSS。
- **Impact**：导入/预览路径 OOM 或长时间假死（GUI 线程内执行）；对共享目录批量扫描（`scanWorkbookDirectory`）可被单个坏文件打挂整轮扫描。
- **Existing Issue Match**：无。#477 对这两个文件的条目（CRC32 属 zip 层、NPY 端序属 sfpkg、SpreadsheetML Type=String/静默失败）均已在本树修复且主题不同。
- **Active PR Match**：无（#518 触碰 io/lasparser、sattrio、catalog/datacatalog*、services/jsonsidecar；不含 outsourceworkbook/wellcompositexml）。
- **Fix Strategy**（最小根因修复）：给三条读路径加「累计已分配单元格数」预算（如 ≤ 4M 格/工作簿，对齐 sfpkg 512MB/8B 口径），超限即报错拒收整簿；顺带对行内**尾随空列**做裁剪（`normalizeHeaderRow` 已有同款思路）可显著降低正常文件的驻留。
- **Regression Test**：`tests/tst_io_outsource.cpp` 增补：fixture「N 行 × ss:Index=16384」→ 期望 `result.ok == false` 且 error 指明预算超限（旧码会尝试分配，测试夹具控制 N 使旧码在 CI 内存下失败或用 QSKIP 计数断言替代）；`tst_wellcomposite.cpp` 同款（`parseComprehensiveWellXml` 返回 false + errorMsg 非空）。

### FA-002 [P3] [VERIFIED] SpreadsheetML `.xml` 读取路径无文件大小闸（.xlsx 有 512MB、.xml 全裸）

- **Module**：src/io
- **File:Line/Function**：`src/io/outsourceworkbook.cpp:727-733`（`readWorkbook` xml 分支 `file.readAll()`）；`src/io/wellcompositexml.cpp:63-69`（`parseComprehensiveWellXml`）与 `:895-902`（`collectSheetRows`，同样 `readAll`）。
- **Root Cause**：`readXlsx` 经 `zipReadArchive`（≤512MB）受闸；`.xml` 分支直接 `readAll()`，随后 `EncodingDetect::decodeText` 还会再产生一份 QString 拷贝（UTF-16 态最高 2×）。LAS（500MB）、DLIS/LIS（`LasParser::fileSizeLimit`）、ZIP（512MB）均有闸，此路径是同类口径的漏网者。
- **Trigger Condition**：预览/导入一个超大 `.xml`（调用链同 FA-001：previewdoc/cuttingsdoc/mkproject/scanWorkbookDirectory）。
- **Observed/Expected**：观察：数 GB `.xml` → 等量（或 ×2–3）内存分配，GUI 线程冻结。期望：与 parseDoc 同款的「超限拒收 + 指明流式替代口径」。
- **Reproduction**：`mkproject`/预览面板打开一个 >2GB 的 `.xml`。
- **Impact**：OOM/长冻 UI；与 FA-001 叠加更糟（放大发生在整读之后）。
- **Existing Issue Match**：无（#466 是 LasParser 专属；#477 两文件条目主题不同）。
- **Active PR Match**：无。
- **Fix Strategy**：`readWorkbook` xml 分支与 `parseComprehensiveWellXml`/`collectSheetRows` 前置 `QFileInfo::size() > kMaxWorkbookBytes`（512MB，与 zip 层同口径）拒收；更优解是改 `QXmlStreamReader(QFile*)` 直读设备（流式，连闸都省——wellcompositexml 本就是流式状态机，只差喂入方式）。
- **Regression Test**：tst_io_outsource：>512MB（稀疏生成）`.xml` → `ok=false`、error 含上限文案；正常小文件回归不变。

### FA-003 [P3] [HIGH] `SegyReader::scanParallel` 任意失败都置 `m_lastScanPartial`，`openCached` 的顺序扫描回退分支不可达

- **Module**：src/io
- **File:Line/Function**：`src/io/segyreader.cpp:1104-1118`（合并循环对任何 `!sh.ok` 都 `m_lastScanPartial = true; return false;`）vs `src/io/segyreader.cpp:1419-1428`（openCached：`if (m_lastScanPartial) { save checkpoint; return false; }` 之后才是注释宣称的「非取消失败（坏道头读失败等）→ 落到顺序路径重试」）。
- **Root Cause**：scanParallel 的失败返回契约没有区分「用户取消」（应 checkpoint 续扫）与「分片读错误/道头损坏」（注释明言应回退顺序 open()——顺序路径能处理变道长、坏道跳过等更广布局）。由于取消与读错误都走同一条 `m_lastScanPartial=true` 返回路径，openCached 里的顺序回退分支对并行扫描失败**恒不可达**（死码）。
- **Trigger Condition**：大文件（>1MB、固定道长、标准 inline）并行扫描期间发生 I/O 读错误（网络盘/坏扇区/被并发截断）→ 整个 open 失败并落一个部分 checkpoint；用户重试时 `loadForResume` + `resumeScan` 也可能因余量非整道而拒绝，只能整体重扫。
- **Observed/Expected**：观察：失败路径与注释/设计意图相反。期望：读错误 → `resetState()` 后顺序 `open()` 重试（更宽容的扫描语义）。
- **Reproduction**：难以纯单测（需在探针通过后制造读失败）：可用自定义 QAbstractFileEngine 或在 tst_segy 中以可注入的 IO 包装复现；代码级判据已明确（两处控制流唯一汇合于 `m_lastScanPartial=true`）。
- **Impact**：健壮性缺口而非数据损坏：可恢复的瞬时读错误被当成不可续扫失败；无索引损坏风险（checkpoint 审计在 load 侧把关）。
- **Existing Issue Match**：无（#477 的 segy 条目是 open() 的 freezeCorners 半状态，已在树内修复，主题不同）。
- **Active PR Match**：无。
- **Fix Strategy**：scanParallel 增加出参或成员位区分 `cancelled` 与 `ioError`：取消 → 保留现 checkpoint 行为；读错误 → 不 checkpoint，返回后由 openCached 走既有 `resetState(); open(path,…)` 顺序回退（该分支已存在，只需让条件可达）。
- **Regression Test**：tst_segy 增补（需 IO 注入钩子）：并行路径读错误 → 断言最终经顺序 open 成功（或如实失败但不落 checkpoint）；取消路径行为保持（`lastScanPartial()==true` 且缓存含 checkpoint）。

### FA-004 [P3] [SUSPECTED] `catalogstore_json.cpp loadTables` 链接 `ord` 无上限 `resize`——损坏工程库可打挂工程打开

- **Module**：src/catalog
- **File:Line/Function**：`src/catalog/catalogstore_json.cpp:277-285`（`if (ord > out->links.size()) out->links.resize(ord + 1);`）。
- **Root Cause**：`ord` 直接来自 `entity_asset_links` 表列（项目 catalog.sqlite）。逻辑损坏（integrity_check 通过、但行内数值被改）的库给出 `ord = INT_MAX` 时，`QVector<EntityAssetLink>::resize(2^31)` 触发天文数字分配 → `bad_alloc`/OOM，工程打开即崩。同仓对不可信计数的既有纪律（segyindexstore `n > 1e8` 闸、sfpkg `#216/#382`、zip `kMaxTotalUncompressedBytes`）此处缺失。
- **Trigger Condition**：打开一个 `entity_asset_links.ord` 被破坏/手改的工程（自身 SQLite 可被外部工具编辑；也在「损坏工程恢复」威胁模型内）。正常写入路径 ord 恒连续，故无自然触发——因此标 SUSPECTED。
- **Observed/Expected**：观察（推演）：ord 巨值 → 巨量分配。期望：ord 与已读行数偏离过大（如 `ord > 已见行数 + 4096`）时按损坏行处理（跳过 + qWarning，同文件 unsafeVersionReason 的既有口径）。
- **Reproduction**：手工 sqlite 工具把某行 ord 改为 2000000000 后 `DataCatalog::open`。
- **Impact**：工程打开路径 DoS（单一坏行）；恢复链（.bak/JSON 兜底）本可救，但崩溃发生在兜底装载内部。
- **Existing Issue Match**：无（#476 catalog 条目是版本唯一约束/qHash/noteRead）。
- **Active PR Match**：catalogstore* 不在 #518 文件清单（清单为 catalog/datacatalog*）；`datacatalog.cpp` 在——但本修复点在 catalogstore_json.cpp，可隔离。
- **Fix Strategy**：一行闸：`if (ord < 0 || ord > out->links.size() + 4096) { qWarning(...跳过损坏链接行...); continue; }`（阈值取宽容常数，正常增量写永不越界）。
- **Regression Test**：tst_catalogstore：构造含 `ord=2e9` 行的 sqlite fixture → `loadTables` 成功返回且该行被跳过（旧码直接 OOM/异常）。

---

## 3. 排除项汇总（与现有 issue/PR 重合；只记录不改码）

格式：签名 → 归属。

**本树已修复确认（swarm issue 对应修复已落地，验证位置）**：
- paleoWriteFileAtomic/PaleoAtomicTransaction 只 flush 不 fsync → #461（已修：atomicfile.h:25-37 paleoSyncFile 走 fsync/_commit，写路径全接）
- PaleoProjectStore::commitAll 回调无 try/catch → #462（已修：paleoprojectstore.cpp:421-471 两单元均包）
- busy-token 跨工程残留（store 侧 setProjectPaths 不清 m_busy）→ #464（已修：paleoprojectstore.cpp:76-77）
- LasParser::parse() 旧入口无大小闸 → #466（尺寸闸已修 lasparser.cpp:206-214；**编码嗅探缺失仍在** → #466 残余，且 lasparser 属 PR #518 触碰文件 → EXCLUDED_ACTIVE_WORK）
- DLIS/LIS parseHeader 头扫描累积无上限 → #467（lrBuf/body 预算已修 dlisparser.cpp:1151、lisparser.cpp:474；**残余向量**：LisParser parseHeader 无文件大小闸时 `TifView::detect`（lisparser.cpp:253-297）段索引随文件线性累积（~1.9×fileSize，段+前缀和数组）→ 同根因归 #467 残余）
- zipExtractBytes 不校验 CRC32 → #477（已修 ziparchive.cpp:369-378）
- sfpkgreader NPY 字节序按主机序 → #477（已修 sfpkgreader.cpp:161-200 readEndian）
- segyreader open() freezeCornersFromFile 失败不清半状态 → #477（open() 侧已修 :634-641；**残余**：resumeScan 侧 :1257-1262 同款不清 → #477 家族残余）
- constraintstore 活动事务内 DDL（ensureAudit CREATE INDEX）→ #477（仍在位 :134-149，未修）
- sattrio SattrVolumeWriter::begin 先 remove 再截断 / readCell 无块缓存 → #477（sattrio 属 PR #518 触碰 → 双重排除）
- rasterpyramid 瓦片自愈只防单进程 → #477
- laswriter fmtValue ±Inf 写 "inf" → #477
- lasalias memo 随机淘汰 → #477
- lasparser parseRange 流式 issues 传 nullptr → #477
- cachecore 暂存名 16ms 窗口互踩 → #477（已修：进程内序号 cachecore.cpp:22-27,207-211）
- encodingdetect isStrictUtf8 尾截断返 true / looksLikeGbLead 死码 → #477（已修 :93-95；死码已删）
- wellcompositexml SpreadsheetML 数值全 Type="String" / 深度 2 位截断 / 无可识别表静默失败 → #477（已修 :655-669 writeCellF、:632-638）
- metastore qHash 连接名碰撞静默串库 → #476（已修 metastore.cpp:42-51 比对 databaseName）
- datacatalog_versions 缺 (asset_id, version_number) 唯一约束 → #476（已修：内存闸 :59-71 + ensureVersionNumberUniqueIndex catalogstore.cpp:457）
- wellsectionstore load() splitEscaped 丢空段 → #476（已修 :334-337 keepEmpty）
- mapversionstore publish()/snapshotInputForPublish() 90 行复制 → #476（已收口 validatePublishGates :733-797）
- datacatalog noteRead 非严格跨线程只告警（QVector COW 属主写+他线程读 UB 面）→ #476（仍在位 datacatalog.cpp:124-133，未修）
- releasestore/layouttemplatestore 无 readOnly 通道 → #476（**当前态未复核**——两文件本轮未逐行，归 #476 既有记录）
- welllogfacies 步长/缺口判据只取 curves.first() → #476（已修，见 :197 注释）；其模型兜底语义属 #486
- faultset branchesOfVertex 死代码/并发懒缓存/段内索引无界检 → #476 + domain/faultset.h 属 PR #518 → 双重排除
- timedepthmodel setCheckshots strict 不查首点负深度 → #476（本轮未逐行该文件，归既有记录；velocityBounds 属 #490 记录项）
- constraintstore 引号转义/层位 '|'/supercover/CRS 迁移族 → #488/#496（已确认在位：quoteSql、:349 '|' 拒绝、migrateLegacyCrsLocked 独占窗）
- 井名/资产重复、快照守卫等 opt-01/opt-02/opt-03 交付点 → #489/#490/#492（不重做）
- 主仓未提交导出簇（src/domain/exportrequest.h 等）→ EXCLUDED_ACTIVE_WORK（未触碰）

**本轮撞见但按判定规则归并既有 issue 的新观察（不进修复池）**：
- LIS parseHeader 的 TifView 段索引 O(fileSize) 累积 → #467 残余向量（同根因：头扫描无大小闸）
- resumeScan freezeCornersFromFile 失败半状态 → #477 家族残余（open() 侧已修，resume 侧未同步）

---

## 4. 未覆盖区域（如实清单）

以下文件本轮**未逐行**（仅头文件契约/调用点/grep 模式排查），深度结论不适用于它们：

- **src/io**：sattrio.cpp（经 #477/#518 排除项了解）、rasterpyramid.cpp（#477）、ingestplan.cpp、dataimport_folder/file/geodata/session/document/wells/pyramids.cpp（合计 ~3100 行）、cuttingsdoc.cpp、horizonbinner.cpp、geojsonaffine.cpp、faciescoveragereader.cpp、lascache.cpp、lasalias.cpp、lrucache.h、shacache.cpp、cachebudget.cpp、sfpkgwriter.cpp、perffixtures.cpp、benchreport.cpp、ioerrors/parserissues 内部头。
- **src/catalog**：catalogstore_crud.cpp、catalogstore_schema.cpp、catalogstore_internal.h、datacatalog_entities/assets/links/derivation/export.cpp、frameworkstore.cpp、realizationset.cpp、roleregistry.cpp、entityview.cpp、purgelease.cpp、catalogroles.cpp。
- **src/metadata**：releasestore.cpp、layouttemplatestore.cpp（#476 readOnly 项未复核）、faultsetstore.cpp、wellsitingstore.cpp、projectlock.cpp、processprobe.cpp、storeerrors_internal.h。
- **src/domain**：constraintsemantics.cpp 仅 300–450 区段逐行（其余接口级）；wellsection.cpp 仅 520–700 区段逐行；faultset.cpp、welllogfacies.cpp（排除项核对级）、receiptsemantics.cpp、welltopsedit.cpp、arearules.cpp（含 segy 索引规则消费面）、evidenceconflict.cpp、frameworkdiagnostics.cpp、sequenceframework.cpp、litholexicon.cpp（#476 已记无专属 tst）、interpretationevidence.cpp、singlefactorstrategy.cpp、faciescatalog.cpp、faciestaxonomy.cpp（grep 命中处已回读，均常量索引）、predictionevidence.cpp、projectclassifier.cpp、types.cpp、seismic/timedepthmodel.cpp（#476）、facieshierarchy.cpp、singlefactorrequest.cpp、seismic/sectionwellprojector.cpp、seismic/sectiongeometry.cpp、wellrecords.cpp、wellsitingplan.cpp、crossplotsamples.h、wellnumeric.h。

---

## 5. 统计

- 新问题（可修候选）：**4 条** —— P2×1（FA-001）、P3×3（FA-002 VERIFIED、FA-003 HIGH、FA-004 SUSPECTED）；无 P0/P1。
- 排除项：既有 issue 已修确认 20+ 项、残余归并 4 项、PR/未提交簇重合 4 类（详见 §3）。
- 覆盖：四域 ~48k 行中，逐行 ~13.5k 行（28 个高风险文件）+ 部分逐行 2 文件，其余接口+调用链级（§4 如实列差）。
