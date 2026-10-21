# 深度代码审查报告 A（full-audit-20261021）

审查人：只读深度审查 Agent A。基线：BASE e6837e14 工作树（只读）。
范围：src/domain/（全部，重点 faultset/welltops/sidecarvocab/evidenceconflict/wellcompositemodel/importrows/seismic/）、src/catalog/（全部）、src/io/（全部）、src/metadata/（全部）。
方法：按 02-agent-briefing——逐文件读头注释（职责/契约）→ 实现 → 高风险函数逐行 → grep 追调用方/被调用方。所有发现给出真实触发路径；与已知 147 项（02a）、上两轮已修/登记项、三支活跃分支触碰面（02-excluded-files.txt）逐条对照。

总评：该四个目录的整体工程质量明显高于仓库平均水平——staging 副本 + journal 重放 + 世代号（DataCatalog）、journal-first 幂等提交（PaleoProjectStore）、损坏自愈缓存（cachecore/segyindexstore #217/#477）、原子替换（atomicfile/paleoWriteFileAtomic）在数据层贯彻得系统且自洽。本轮确认大量"疑似"路径实际已被历史 issue 修复（#476 连接名碰撞、#467 无界拼接、#217 解压尺寸、#384 维度溢出、#169 批次回滚、#383 锁自旋、T33 TOCTOU 终验）。新发现集中在**规模化放大**与**损坏输入的边界校验**两处缝隙。

---

## Findings

### A-01｜ShaCache 逐 miss 全表同步落盘——批量导入 O(N×表容量) 写放大，且持锁做磁盘 IO
- **Severity**: P2 ｜ **Confidence**: VERIFIED（代码路径实证；量级为算术推演，未实测）
- **Module/File**: `src/io/shacache.cpp:153-187`（miss 路径）、`shacache.cpp:241-258`（saveDiskLocked）、`shacache.h:78-79`（m_diskLimit=65536 / m_memLimit=16384）
- **函数**: `ShaCache::sha256Hex` / `ShaCache::saveDiskLocked`
- **根因**: 每次缓存 miss（即每导入一个新文件）在持有 `m_mutex` 的临界区内执行 `saveDiskLocked()`——把**整个磁盘层 m_disk（上限 65536 条，JSON 每条约 120–200 字节 ≈ 8–13 MB）**全量序列化并同步写盘（QSaveFile）。这不是增量追加，而是全表重写。另外到达容量后每次 miss 还要跑 O(m_diskLimit) 的最近命中扫描做收缩（:171-186）。
- **触发条件/执行路径**（真实调用链）：
  1. `DataImportService::setProjectDir` → `ShaCache::shared().setDiskFile(<project>/artifacts/index/sha.json)`（dataimportservice.cpp:95）；
  2. 文件夹导入 plan 期逐文件哈希：`buildIngestPlan` → `ShaCache::shared().sha256Hex(path, …, MtimeSizeInode)`（ingestplan.cpp:543-544）；
  3. 单文件导入面同源：`importOneFile` → `ShaCache::shared().sha256Hex`（dataimport_file.cpp:161）。
  新文件全部 miss（表越大写越大）：N 个新文件的导入 = N 次全表 JSON 序列化 + 同步写。表 3 万条（≈5 MB/次）时导入 1 万个文件 ≈ **50 GB 同步写 IO**，外加每次 O(3 万) 的 QJsonObject 组装；全部发生在 worker 线程持锁期间，任何并发的 sha 查询（如另一导入会话）在磁盘写期间被阻塞。
- **Observed vs Expected**: 期望"算一次大文件的哈希值得立即落盘"（注释原文）以容错崩溃；实际实现把容错成本做成 O(N×容量)。期望 miss 摊还 O(1)~O(log)，实际 O(表大小) 磁盘写 + O(表大小) CPU。
- **影响面**: 大/中型工区首次文件夹导入（"从工区文件夹导入"是一等入口）在导入中途越导越慢；机械盘/网络盘上可感知冻结；ssd 寿命损耗。正确性无损（QSaveFile 原子，坏表自愈）。
- **排除比对**: 147 项库无 shacache 条目（B.3 是 dataimport_geodata 大小闸、B.11 是 cataloghealth 大小写比对，均不同根因）；上两轮已修 A-14 是 storagegovernance sidecar、B-04 是 sattrio 取消谓词，不相交。02-excluded-files.txt 不含 shacache.cpp。**新发现**。
- **建议修复**: ① 最小改——落盘去抖（脏标记 + 定时器/QElapsedTimer 间隔 ≥1s 或每 K 次 miss 落一次，析构/setDiskFile/invalidate 强制刷）；② 或 sha.json 改追加式行格式（QSaveFile 全量替换仅在与去抖联合时保留）；③ 收缩循环从"每次 miss"改为"每次落盘前一次"。回归测试：tst_cache_io 补"连续 N 次 miss 只允许 ≤N/K 次写盘"（计数 QSaveFile open 或注入 clock）。

### A-02｜DLIS 逻辑记录 trailer/pad 剥离按累计缓冲校验——损坏的续段可切掉前段字节（跨段 chop）
- **Severity**: P3 ｜ **Confidence**: VERIFIED（代码与 RP66 语义比对成立；"静默错值"后果 SUSPECTED——多数形状会落到解析报错）
- **Module/File**: `src/io/dlisparser.cpp:1186-1213`（对照 `src/io/lisparser.cpp:440-460` 同族正确实现）
- **函数**: 匿名 `walk()` 中 LRS trailer 剥离段
- **根因**: 跨段逻辑记录（LR 分段，前驱/后继位）的字节体累积在 `lrBuf` 中；每个段到达后从 `lrBuf` 尾部剥 trailer（checksum 2B + traillen 2B + padcount）。padcount 取自段尾一个**任意值字节（0–255，文件可控）**，随后 `trim += padCount` 只做 `trim > lrBuf.size()` 的总量校验（:1206）——没有按**本段 bodyLen** 封顶。多段 LR 中，损坏/构造的 padcount 可超过本段实际 padding，把**前一段的字节**一并 chop 掉。
- **触发条件**: 任一含跨段 EFLR（大 CHANNEL/FRAME 集，walk 注释 :1058-1060 明示该形态常见）的 DLIS，其某个后继段的 pad 计数字节损坏（或恶意构造）。命中后：lrBuf 内容被错误截短 → `parseEflr` 要么结构报错（多数），要么在剩余字节恰好仍是合法 EFLR 结构时解出**错误属性值**（井名/单位/维度/表示码——SUSPECTED 分支）。
- **Observed vs Expected**: 期望与姊妹实现同口径——lisparser 的 PR 段 trailer 在**段内**扣除（`dataLen = len - 4 - trailer`，lisparser.cpp:455-460，先算 dataLen 再 append），天然不可能跨段；dlisparser 却对累计缓冲做总量校验。
- **影响面**: 损坏文件鲁棒性（B 类 io-robustness）。无内存安全问题（chop 上限受缓冲长度约束）；调用链 `WellLogRead::parseCurves/readWellInfo → DlisParser::parse/parseHeader → walk`，生产可达（井曲线导入/预览）。
- **排除比对**: 147 项库无 dlisparser 条目；A-11（上轮已修）是 segyreader 变道长；H1 是 ziparchive 回绕。dlisparser.cpp 不在 02-excluded-files.txt（未被三支分支触碰，高价值区）。**新发现**。
- **建议修复**: 剥离前校验 `trim <= bodyLen`（本段体长，`segLen-4`），或在 chop 前用 `lrBuf.size() - trim >= lrBufBaseOffsetOfCurrentSegment` 等价判据。回归：构造双段 EFLR、后继段 padcount=255 的损坏件，断言拒绝并报"填充字节计数越界"。

### A-03｜dedup 补挂路径对井文本整体 readAll 二次解析——同一文件导入流内两套内存口径
- **Severity**: P3 ｜ **Confidence**: VERIFIED
- **Module/File**: `src/io/dataimport_file.cpp:432-474`（`attachResolvableLinks`；readAll 位于 :441 well_stratification、:455 time_depth、:466 well_head）
- **函数**: `DataImportService::attachResolvableLinks`（唯一调用点 :257，importOneFile 的 dedup 分支）
- **根因**: importOneFile 主路径已按 #561/#387 把井文本改为**流式逐行解析 + 500 MB 闸**（:196-231）；但随后的 dedup 命中分支（同字节重导入）调用 `attachResolvableLinks`，对同一文件再 `f.readAll()` + `parseWellTopsText/parseTimeDepthText/parseWellHeadText` 整读整解析（无流式、无独立大小检查——仅因上游先过闸而受 500 MB 间接约束）。
- **触发条件**: 重导入一个已在库的大井表（dedup 命中，"字节已在库"路径）。500 MB 文件的 readAll + 逐行 QStringList/结构体构造 ≈ 1–3 GB 瞬时内存，发生在导入 worker；与刚完成的流式首遍（O(64KB) 滚动）形成同一导入流程内两种内存口径。
- **Observed vs Expected**: 井文本解析已统一流式（wellfileparsers 的 parse*Device 族），dedup 补挂是唯一残留的整读消费点。期望复用 `parseWellTopsDevice` 等流式入口。
- **影响面**: 大井表重复导入时的内存尖峰/worker 延迟；无正确性问题。
- **排除比对**: B.3/B.4（147 项）是"无大小闸整读"（dataimport_geodata 图例字典 / cuttingsdoc csv），本条是"已有闸路径下的二次整读"，根因不同（闸缺失 vs 复用缺失）；dataimport_file.cpp 不在排除清单。**新发现**。
- **建议修复**: attachResolvableLinks 的三个井表分支改用 `parse*Device(&f, nullptr, &report, &ioErr)` 流式入口。回归：tst_import 补"dedup 路径解析报告与首遍一致"。

### A-04｜LibreOffice 转换队列 start 端缺少 RAW 存在性复核——排队期间源版本被删后以空参数起 soffice，且 lineage 字段带陈旧值
- **Severity**: P3 ｜ **Confidence**: VERIFIED（守卫缺失实证；触发依赖排队窗口）
- **Module/File**: `src/io/dataimport_document.cpp:295-321`（startNextConversion）、:243-293（queueConversion 对照）
- **函数**: `DataImportService::startNextConversion`
- **根因**: queueConversion 在**入队时**检查 RAW 存在（:269 `rawAbs.isEmpty() || !QFile::exists(rawAbs)` → "原始文件缺失"）；startNextConversion 是串行队列的**出队执行点**（可能在长队列/长转换后数分钟），重新查 RAW（:306-312）却**无任何存在性检查**。两个后果：
  1. 出队时 RAW 已被删（调用链真实存在：dataops「物理删除」→ `DataCatalog::removeAssets`，datacatalog_assets.cpp:85-161，可删资产及其全部版本）→ `rawAbs` 为空 → `m_convOutFile = outDir + "/.pdf"`（`QFileInfo("").completeBaseName()` 为空串，生成**隐藏文件名**）→ `m_convProc->start(converter, {…, ""})` 以空输入参数起 soffice → 退出非零 → 用户看到的是原始 stderr 文本而非"原始文件缺失，无法转换"的既定文案；
  2. `m_convRawVersionId` 成员只在 RAW 命中时赋值（:310），未命中时**保留上一单/上一工程的旧值**，`finishConversion` 若走到 addVersion（本形状走不到，但 `setProjectDir` 换工程后 :74-82 清队列不清该成员，构成跨工程脏状态面）会把陈旧 parentVersionIds 写进血统。
- **触发条件**: 文档 A 的转换在队（前序转换耗时最多 kConvDefaultTimeoutMs=120 s），期间用户对资产 B…—准确说对本资产——执行物理删除，随后本单出队。窗口窄但为真实用户操作序列。
- **Observed vs Expected**: 期望出队端复核与入队端同一守卫（含 `rawAbs.isEmpty()` 与 `m_convRawVersionId` 置空/校验），失败走 `failNow(tr("原始文件缺失…"))`。
- **排除比对**: #555（进程组回收/看门狗）与 FE-004/R2-SF-05 已修项均不覆盖此守卫；147 项库无 dataimport_document 条目；文件不在排除清单。**新发现**。
- **建议修复**: startNextConversion 的 RAW 循环后补 `if (rawAbs.isEmpty() || !QFile::exists(rawAbs)) { m_convPending.remove(m_convCurrent); failNow 同款文案; return; }`，并在未命中时 `m_convRawVersionId.clear()`；setProjectDir 一并清 `m_convRawVersionId/m_convCurrentVersionId`。回归：tst 中 stub 转换器 + 入队后删资产，断言收到 documentPdfFailed("原始文件缺失") 且未创建 `/.pdf`。

### A-05｜cachecore putStr 对 >64KB 字符串写短长度前缀——编码侧长度字段与实写字节数不一致（防御性，当前调用面不可达）
- **Severity**: P3 ｜ **Confidence**: VERIFIED（代码）；**可达：否**（当前唯一受影响调用是 segyindexstore 的 canonicalPath）
- **Module/File**: `src/io/cachecore.cpp:356-361`（putStr）
- **函数**: `cacheio::putStr` / 对应 `cacheio::str`（:418-431）
- **根因**: `putU16(out, qMin<qsizetype>(0xFFFF, utf8.size()))` 把长度截到 65535，但 `out->append(utf8.constData(), utf8.size())` 仍写**全部**字节。若某字符串 UTF-8 超 64 KB，落盘的长度前缀（0xFFFF）小于实写字节数，解码侧按前缀推进后残留错位字节，后续字段全部解析失败（该缓存条目整体自愈删除，不会崩溃）。
- **触发条件**: 经 `SegyIndexStore::encodePayload` → `cacheio::putStr(index.ident.canonicalPath)` 写入超长规范路径（>64 KB 路径在 Windows/共享盘理论上限内但实际不可达）。
- **排除比对**: 147 项库与排除清单均无 cachecore 条目；#217/#477 修的是读侧尺寸与暂存名。**新发现（加固级）**。
- **建议修复**: 超长时截断内容或直接判损坏拒写（返回 false），保证「长度字段 == 实写字节数」不变量。

---

## 排除比对记录（发现→已知库，重要佐证）

1. **EXCLUDED_ACTIVE_WORK（H4 同根因，修复覆盖面提示）**：`src/io/dataimport_document.cpp:259-268`（queueConversion 取**第一条** RAW 并 break）与 `:305-312`（startNextConversion 同型）在 `relocateVersionSource`（:514-582，追加新 RAW、旧死路径记录留表）之后，锚定回最早（死路径）RAW → 转换永远报"原始文件缺失"。这与 147 项 **H4**（datapreviewtabs_content.cpp:33-39 取第一条 RAW）为同一根因家族，按简报纪律不作为新发现报出；但**H4 的修复（"改取最高号 RAW"）必须同时覆盖 dataimport_document.cpp 的两处锚定点**，否则重定位后 PDF/XLSX 预览转换仍死锁。登记于此供修复分支对齐。
2. **EXCLUDED（上轮登记未修 A-16，复核仍成立）**：`src/catalog/frameworkstore.cpp:259-265`——`batch.flush` 失败（endBatch 落盘失败 → #169 内存回滚）时，先前 `paleoWriteFileAtomic` 写出的 `abs`（:174）不在任何清理路径上，字节留存、目录无记录（孤儿）；其余失败点（:154/:165/:180/:198/:210/:251）均已 `batch.abort()+QFile::remove(abs)`。孤儿序号碰撞由 `scanManagedSeqFloor`（datacatalog.cpp:36-65）兜住，不卡死后续导入。
3. **EXCLUDED（上轮登记未修 A-12，复核仍成立）**：`src/metadata/mapversionstore.cpp:324-384`——freezeDir 冻结复制发生在 `db.transaction()`（:386）之前；除 insert 失败路径的 removeRecursively 外，事务外的失败窗口仍可留冻结目录。
4. **复核为已修/设计内，未报**：metastore.cpp:39-51 已按 #476 拒绝 qHash 连接名碰撞串库；dlisparser #467 累积预算、#384 维度哨兵、64 位 slotBytes（:926-934）均在位；datacatalog endBatch #169 回滚含索引恢复（:721）；partialread WriteGuard #383 死循环修复与 Windows 句柄时序注释在位；cachecore #217 解压尺寸闸与 #477 暂存名在位；sfpkgreader #216 除法比较 + #382 元素预算在位；attrgridout/meanStep 空轴由几何校验前置拦截；timedeptool first()/last() 均有 ≥2 样点守卫；faultset twtMin/Max 空棒 0 值与 kExtrapolatedPointIndex 死哨兵为已知 L2.1/L2.2（在排除文件内，未重复报）。
5. **审查后放弃的候选**（证据不足或影响不可达，列出供后续深挖）：mapversionstore saveVersion 在 PDF 文件缺失时仍落 frozen 版本（saved_pdf 空 + 行带 pdfAssetId）→ 该版本永久不可发布——判为诚实拒绝（错误文案"PDF 指纹与成果版本绑定不一致"语义正确，用户出路=重导出+存新版本）；documentPdfPath/workbookXlsxPath 取首条 DERIVED 而非最新——多 PDF 版本在现行 Ready 门下不可达；purgelease key 依赖 QDir::current()——全仓无 setCurrent 调用；nicestep 极值 range 的 NaN→int 转换 UB——数据面不可达；LayoutTemplateStore/ReleaseStore create 的 maxSeq 扫描+INSERT 非事务——单写实例锁下无竞态。

---

## 覆盖清单

### 逐行/函数级深读（头注释+实现全读，高风险函数逐行）
- **src/metadata/**（12 文件）：mapversionstore.cpp（1084 行全读，含 commitPublicationPlacement/recoverInterruptedPublications 双向推演）、paleoprojectstore.cpp、metastore.cpp、projectlock.cpp、paleoprojectfile.cpp、layermanifest.cpp、faultsetstore.cpp、layouttemplatestore.cpp、releasestore.cpp、processprobe.cpp、mapsnapshot.cpp（对照 A.4 已知）、wellsitingstore.cpp（对照 A.6 已知，接口级）；atomicfile.h 对照 A.7 已知（接口级）。
- **src/catalog/**（14 文件）：catalogstore.cpp（对照 A.1/H2 已知后全读）、catalogstore_crud.cpp、catalogstore_schema.cpp、catalogstore_internal.h、catalogstore.h、datacatalog.cpp、datacatalog_versions.cpp、datacatalog_assets.cpp、datacatalog_links.cpp、datacatalog_entities.cpp、datacatalog_derivation.cpp、frameworkstore.cpp（对照 A-16）、purgelease.cpp、roleregistry.cpp、realizationset.cpp。
- **src/io/**（21 文件）：dlisparser.cpp（1462 行全读）、wellfileparsers.cpp（含流式嗅探/收割状态机逐行）、dataimport_folder.cpp、dataimport_session.cpp、dataimport_file.cpp、dataimport_document.cpp、dataimportservice.cpp、segyindexstore.cpp、cachecore.cpp、shacache.cpp、partialread.cpp、welllogread.cpp、laswriter.cpp、attrgridout.cpp、timedeptool.cpp、wellrecords.cpp；lisparser.cpp（PR 段装配/trailer/#467 段落精读）、sfpkgreader.cpp（NPY shape/dtype/#216/#382 段精读）、streaming.h（接口契约）；ziparchive/segyreader/lasparser/encodingdetect/cuttingsdoc/constraintstore/ingestplan/wellcompositexml/horizonbinner/sattrio/dataimport_geodata 为排除文件，按已知项对照+定点复核（rollbackFailedRow 实现精读于 ingestplan.cpp）。
- **src/domain/**（14 文件）：faultset.cpp（772 行全读）、wellsection.cpp（736 行全读）、deviationsurvey.cpp（418 行全读，含 tvdToMd 二分契约）、seismic/sectionwellprojector.cpp、seismic/nicestep.cpp、interpretationevidence.cpp（262 行全读，审计事件链验证）、frameworkdiagnostics.cpp（主体）、faciestaxonomy.cpp（全读，含 upgrade 去重环推演）、wellrecords.cpp、faciesconsistency/facieshierarchy/paleomapfacies（接口级+first()/last() 触点精读）；evidenceconflict/welltopsedit/welllogfacies/wellnumeric/sequenceframework/litholexicon/wellcompositemodel/sidecarvocab/timedepthmodel/welltracelocate 为已知项/排除文件，按 02a 对照不重复报；importrows.h 为纯 DTO（与 L5.9 测试缺口已知项一致）。

### 接口级（头契约+关键调用点，未逐行）
catalogindex.cpp（A.2 已知）、catalogstore_json.cpp（A.3/H2/L3.9 已知，loadTables 调用面）、catalogroles.cpp、entityview.cpp、datacatalog_export.cpp、datacatalog.h、outsourceworkbook.cpp（FA-002 闸与 .tmp 家族复核）、sfpkgwriter.cpp、geojsonaffine.cpp（kMaxGeoJsonBytes 闸复核）、rasterpyramid.cpp、lascache.cpp、lasalias.cpp、cachebudget.cpp、lrucache.h、perffixtures.cpp、benchreport.cpp、faciescoveragereader.cpp、dataimport_wells.cpp、dataimport_pyramids.cpp、dataimport_geodata.cpp（排除文件，闸对照）、crossplotsamples.h、singlefactorrequest.cpp、singlefactorstrategy.cpp、types.cpp、arearules.cpp、receiptsemantics.cpp、predictionevidence.cpp、constraintsemantics.cpp（checkConstraintPart 自交扫描线段精读，其余接口级）、wellcompositemodel.cpp（C.9/D.9 已知）、evidenceconflict.cpp（C.1 已知）、welltopsedit.cpp（C.3/L2.3 已知）、sequenceframework.cpp（C.3 i18n 已知）、litholexicon.cpp（C.2 已知）、faultset.h、wellsection.h、deviationsurvey.h、seismic/timedepthmodel.cpp（L1.2 已知）、seismic/welltracelocate.h（L1.1 已知）。

### 未覆盖区域（如实登记）
- src/io/lisparser.cpp 主体（1155 行中仅精读 PR 装配与大小闸段——曲线解码/PR 变体分支未逐行）；streaming.cpp 实现体（仅头契约+两处 BOM 触点）；rasterpyramid.cpp/lascache.cpp/lasalias.cpp/sfpkgwriter.cpp/geojsonaffine.cpp/outsourceworkbook.cpp 实现体未逐行（大小闸与原子写触点已核）。
- src/domain/receiptsemantics.cpp（541 行）、predictionevidence.cpp、singlefactorstrategy.cpp、constraintsemantics.cpp 主体（859 行中约 500 行）、arearules.cpp、faciescatalog.cpp、facieshierarchy.cpp、paleomapfacies.cpp 实现体、wellsitingplan.cpp、sequenceframework.cpp 主体——接口级。
- vendor/（SBM/QGIS）内部不审（纪律）；tests/ 仅在排除比对时定点引用。
