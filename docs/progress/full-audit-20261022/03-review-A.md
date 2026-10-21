# 03-review-A — 数据 / Catalog / IO / Metadata / 工程生命周期深审

只读 subagent A（2026-10-22 轮）。基线 b301f2e8，增量窗 093346e6..b301f2e8
（#603 / #610 / #623 / #624 / #625 / #626 中触达本模块者为 #603 主体 +
#610 局部 + #623 修复）。源码只读，未执行构建。

## 1. 覆盖清单

### 1.1 增量深审（逐行 / 调用链追踪）

| 文件 | 方式 | 风险级 | 测试对应 |
|------|------|--------|---------|
| src/io/welllogread.{h,cpp} | 逐行 | 高（格式分派门面） | tst_welllogtext / tst_welllog_multiformat / tst_welllog_consumers |
| src/io/welllogtext.cpp + _internal.h + _convert.cpp | 逐行 | 高（新解析器，预算/合并/索引对齐） | tst_welllogtext |
| src/io/dataimport_log.{h,cpp} | 逐行 | 高（井归属/井名判定/dedup 目标校验） | tst_import_ux（similarWellNames 等 34 例） |
| src/io/importclassification.{h,cpp} | 逐行 | 中 | tst_import_ux（largeWorkbookClassification…） |
| src/io/importrecovery.{h,cpp} | 逐行 | 高（崩溃恢复删除面） | tst_import_ux 6 例崩溃恢复（929–1049 行） |
| src/io/dataimport_session.cpp | 逐行 | 高（produce-then-commit 核心） | tst_import_ux / tst_import_performance |
| src/io/dataimport_file.cpp | 逐行（importOneFile + attachResolvableLinks） | 高 | tst_import_ux / tst_import |
| src/io/dataimport_folder.cpp | diff + produceFromPlan/produceItem 区段逐行 | 高 | tst_import_ux（lateArrivingFilesAreSkipped…） |
| src/io/dataimportservice.{h,cpp} | 头全读 + setProjectDir/sweep 区段逐行 | 高 | tst_import_ux（reopen 系列） |
| src/io/dataimport_wells.cpp | diff（importWellLogFamily 重写 + F5 坐标回填） | 中 | tst_import_ux（wellheadImportBackfills…） |
| src/io/ingestplan.cpp | diff + executeIngestPlan/rollbackFailedRow/propose* 逐行 | 高 | tst_folderimport / tst_import_ux |
| src/io/outsourceworkbook.{h,cpp} | diff（readSpreadsheetMlWorkbook 适配器） | 低 | tst_io_workbook_edges |
| src/io/wellcompositexml.cpp | diff + 离散曲线/层序道区段 | 中 | tst_wellcomposite_* |
| src/catalog/datacatalog.{h,cpp} | diff + createStagingCopy/checkpoint/rollback/applyJournal 逐行 | 高 | tst_catalog* / tst_import_ux |
| src/catalog/datacatalog_entities.cpp | diff + wellsMatchingName/normalizeWellName | 中 | tst_catalog_catalogindex |
| src/catalog/datacatalog_export.cpp | diff（#623 GeoJSON 环数组修复，验证正确） | 低 | #623 自带测试 |
| src/catalog/datacatalog_versions.cpp | resolvedVersionPath/isSafePathSegment（路径安全核查） | 高 | tst_catalog_integrity |
| src/domain/projectclassifier.cpp | 全文 | 中 | tst_import_ux（quotedCsv…） |
| src/domain/lithologystyle.{h,cpp} | 全文（新文件） | 低 | tst_wellcomposite_visual / tst_welllog_defaultview |
| src/domain/welllogfacies.{h,cpp} diff | 逐行（depthUnitToMeters/normalizeDepthCurve） | 中 | tst_wellfacies |
| src/domain/importrows.h / wellcompositemodel.h / wellrecords.cpp diff | 逐行 | 低 | tst_domain_importrows / tst_wellcomposite |
| src/metadata/atomicfile.h | 全文 | 高（原子落盘基元） | 既有 fault-point 测试 |
| src/workflow/folderimport.cpp（调用方契约） | 255–430 行区段 | 中 | tst_folderimport |

### 1.2 接口/契约层覆盖（无增量模块）

- src/catalog：catalogindex.h（无名字键索引——updateEntity 行内改写不破坏索引，已验证）；
  catalogstore_crud/json/schema 的 isSafePathSegment 反序列化校验点；purgelease /
  realizationset / roleregistry 接口面（spot check）。
- src/metadata：paleoprojectfile（missingMembers 兼容面）、mapversionstore
  （:386/:1046 事务 + qScopeGuard rollback 模式抽查）、projectlock/paleoprojectstore
  生命周期签名面。本窗口 src/metadata 零 diff，上轮审计（20261021）已全量覆盖，
  本轮仅按重点面抽查未再发现新问题。
- src/domain：types/wellsection/faultset 等未变更文件仅接口面过目。

### 1.3 未核查区域（如实申报）

- dlisparser/lisparser/lasparser 内部（窗口内无改动；上轮 A-02 已在册）。
- segyreader/sfpkg/ziparchive/cachecore/lrucache（#582/#587/#588 排除族或无改动）。
- dataimport_geodata/document/pyramids（无改动，仅调用面过目）。
- outsourceworkbook readSpreadsheetMl 内部实现（既有代码，kMaxWorkbookBytes 预算已核）。
- src/metadata 各 store 的逐行复审（零 diff，见 1.2）。

## 2. Findings（按严重度）

分级说明：本轮无 P0/P1/P2 级新发现——#603 的事务核心（intent 恢复、行级回滚、
conflict 闸、move→journal→rollback 链）经 tst_import_ux 34 例（含 6 例崩溃注入）
验证为自洽且自愈。以下 8 项全部 P3。

---

### A-01 井名唯一规范化命中时错误文案谎报「对应多口井」

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/dataimport_log.cpp:37-44 `ensureWell`
- Root Cause: `wellsMatchingName` 按 normalizeWellName（去空格/连字符/下划线+小写）匹配；
  命中恰 1 口但原文名不同（如文件井名 `A-1` vs 库内 `A1`）时，先落入
  `matches.size()==1 && 原文精确比较` 的否定分支，再被
  `if (!matches.isEmpty())` 分支以「对应多口井」文案拒绝。
- Trigger Condition: 单文件导入 well_log，内部井名与已有井规范化同、原文名异。
- Observed/Expected: 文案「井名 A-1 对应多口井，请选择已有井」/ 应区分
  「存在相似井名（1 口）需显式选择」与「多口井」两种情形。拒绝行为本身是 F4
  设计且被 `similarWellNamesRequireExplicitSelection`（tst_import_ux.cpp:594）
  锁定——测试只断言失败不断言文案。
- Reproduction: 先导入 well 名 `A1` 的 CSV 日志，再导入井名 `A-1` 的 CSV 日志，
  观察错误文案。
- Impact: 用户被指示「多口井」却只有一口，误导排查方向；预览侧文案
  （fillLogPreview「已有井名相似或对应多口井」）是含糊但正确的对照。
- IssueMatch/PRMatch: 无冲突（新 #603 代码）。
- FixStrategy: ensureWell 内对 matches.size()==1 与 >=2 分开给文案。
- RegressionTest: tst_import_ux 增加「单模糊命中」用例断言文案含「相似」不含「多口」。

### A-02 新文本曲线读取面只认 UTF-8，与工区 GB18030 现实割裂

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/welllogtext.cpp:69-71 `Detail::readText`
- Root Cause: readText 用 QStringDecoder(Utf8) 严格解码，失败即拒
  （「文本不是有效 UTF-8 编码」）；而同一工区井表生态（wellfileparsers +
  encodingdetect.h）明确声明「工区里以 UTF-8 与 GB18030 两种面目出现」并支持
  GB18030 解码。含中文（井名/表头「深度」）的 GB18030 CSV/TXT 无法作为曲线导入。
- Trigger Condition: 目标工区常见的 GB18030 编码文本曲线文件（纯 ASCII 表头可过，
  带中文内容即拒）。
- Observed/Expected: 拒绝+明确报错（诚实失败）/ 生态一致性应沿用
  EncodingDetect::decodeText 或至少在报错中指引转码。
- Reproduction: 写 GB18030 编码 `# Well: 某1井` + `深度,GR` CSV，previewFiles
  → importBlocked，错误「文本不是有效 UTF-8 编码」。
- Impact: #603 新能力对中文工区一半真实文件不可用（可人工转码绕过；LAS 不受影响）。
- IssueMatch/PRMatch: 无冲突（新读取面）。
- FixStrategy: readText/sniffText 换用 EncodingDetect::decodeText（UTF-8 优先、
  GB18030 兜底），保持严格校验其余不变。
- RegressionTest: tst_welllogtext 增加 GB18030 用例（井名+中文索引列）。

### A-03 分类器索引词表与读取器词表不一致（井深 / 分号）

- Severity: P3 | Confidence: VERIFIED | Module: src/domain + src/io
- File:Line/Function: src/domain/projectclassifier.cpp:157-162（indexHeader/spacedIndex）
  vs src/io/welllogtext_internal.h:20-26 `isIndex`、src/io/welllogtext.cpp:94-96（分隔符）
- Root Cause: 分类器把 `井深` 当索引词、把 `;` 当合法分隔符提议 well_log；
  读取器 isIndex 不认识 `井深`，readText 只支持 tab/逗号/空白。分号文件在
  whitespace 模式下整行成单列，报「需要索引列和曲线列」；`井深` 文件报
  「缺少明确索引列 DEPTH/…/深度」（列表里也没列井深）。
- Trigger Condition: 井深表头 CSV；分号分隔 European 风格 CSV。
- Observed/Expected: 预览期被提议为「测井曲线」、执行期拒绝，错误文案与预览
  分类自相矛盾 / 两词表应同源。
- Reproduction: `井深,GR\n100,5\n` 或 `DEPTH;GR\n100;5\n` 走 preview→import。
- Impact: 确认表体验割裂（用户被系统提议后又被系统拒绝）；无数据损害。
- IssueMatch/PRMatch: 无冲突。
- FixStrategy: 共享一个索引词/分隔符常量表（domain 提供、io 消费），或从分类器
  删去井深/分号。
- RegressionTest: 两类文件各加「提议==可导」断言。

### A-04 dedup 补挂路径仍整读 readAll（上轮 A-03'21 解冻后未收敛）

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/dataimport_file.cpp:480 / :494 / :505
  `attachResolvableLinks`
- Root Cause: well_stratification/time_depth/well_head 三族 dedup 命中后
  `f.readAll()` + 全量重解析取井名（井表大小闸 500MB，readAll 峰值等量内存）；
  主路径 #561 已改流式，well_log 已被 #603 移出该入口，剩下三族未动。
  20261021 登记册 A-03 因 #603 重做该函数而 BLOCKED，#603 落地后此面仍在。
- Trigger Condition: 重复导入大井表文件（dedup 命中）。
- Observed/Expected: readAll 峰值 ≤ 文件大小 / 流式或复用导入期解析结果。
- Reproduction: 导入 100MB well_head 后再次导入同一文件，观察内存。
- Impact: 交互期百 MB 级 RSS 尖峰；无正确性损害。
- IssueMatch/PRMatch: 承接 20261021 A-03（BLOCKED 解除）；与 #561/#603 相关但
  不重叠已登记 issue。
- FixStrategy: 与 #561 同款 parseXxxDevice 流式化。
- RegressionTest: 大文件 dedup 内存/时间预算测试（tst_import_performance）。

### A-05 文本/XML 曲线一次导入周期全量解析 ≥3 次

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/ingestplan.cpp:384 `proposeWellLog`（plan 期
  readWellInfo→WellLogText::parse 全量校验）+ src/io/dataimport_log.cpp:112-135
  `fillLogPreview`（preview 期 inspectLog 全量 + 双份全文件 SHA）+
  src/io/dataimport_file.cpp:203-220（commit 期快照再全量）。
- Root Cause: readWellInfo 对 Text/XML 走 parseAdditional 全量验证只为取井名；
  预览再验一次（有理由）；提交快照再验一次（安全链要求）。外加 fillLogPreview
  对同一文件 3 趟全读（before-digest / parse / after-digest）与 produceItem 的
  expected-digest、importOneFile 的 sourceSha——单个 64MB 文本日志一轮
  「预览→确认」约 6–8 趟全文件 IO。
- Trigger Condition: 目录导入含 N 个文本/XML 曲线文件（LAS 的 plan 期仅头扫，
  不受影响）。
- Observed/Expected: plan 期井名提取应为头扫描级（#Well: 行 / WELL 列 / 工作表名）。
- Reproduction: 100×64MB CSV 日志目录 previewFolder 计时。
- Impact: 大目录预览秒到分钟级延迟；64MB/4M 格预算使上界可控。
- IssueMatch/PRMatch: 无冲突。
- FixStrategy: WellLogText 增加井名快照通道（只读头部/工作表名）供 plan 期用；
  fillLogPreview 复用 plan 期 sha（已有 F2 真复核）替代 before-digest。
- RegressionTest: tst_import_performance 增加文本日志目录预览预算项。

### A-06 空白分隔模式缺 kMaxColumns 逐格早退

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/welllogtext.cpp:38-65 `split`
- Root Cause: 分隔符分支在循环内检查 `cells.size() > kMaxColumns`（:61）即返
  false；空 白 分隔分支在函数入口直接 `split(QRegularExpression)` 返回，无列数
  上限——病态单行 64MB 文件先物化整份 QStringList，再由 readText 的 kMaxCells
  （:101-102）总量闸兜底。
- Trigger Condition: 无 tab/逗号的超宽单行文本文件。
- Observed/Expected: 两模式同列预算 / 空白模式同样早退。
- Reproduction: 构造 64MB 单行空白分隔文件调 WellLogText::parse，观察峰值内存
  （可达文件体积数倍后才被拒）。
- Impact: 内存放大（受 64MB 文件闸约束上界）；无越界。
- IssueMatch/PRMatch: 无冲突。
- FixStrategy: 空白分支改逐 token 循环并复用同一列数检查。
- RegressionTest: 超宽单行用例断言快速失败。

### A-07 恢复失败与 catalog 打开失败共用同一错误通道

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/dataimportservice.cpp:104-113 `setProjectDir`
- Root Cause: recoverImportIntents 失败（intent 损坏 / 目标字节被改 / 文件删不掉）
  时 `m_catalogReady=false` → 发 `catalogOpenFailed` + 填 `catalogOpenError`
  ——catalog 实际已成功打开、读面完好，仅导入面被安全停摆；错误文案与信号却
  告诉 UI「打开失败」。硬停语义本身被
  `recoveryRefusesTamperedBytesAndPreservesEvidence`（tst_import_ux.cpp:1027-1049）
  锁定为有意设计。
- Trigger Condition: 上次进程在提交中段崩溃后，落位字节被改动或 intent 损坏，
  再开同一工程。
- Observed/Expected: UI/诊断面把「catalog 打开失败」当存储损坏排查 /
  应区分「提交恢复未完成（staging 残留）」与「catalog 打开失败」两通道。
- Reproduction: tst_import_ux 1027 例同法（改 moved 字节后 reopen），看状态栏
  文案来源。
- Impact: 误导诊断方向（用户可能重建工程）；恢复路径本身正确、不动数据。
- IssueMatch/PRMatch: 无冲突（#603 新面；#587 是 catalog 开库族问题，不覆盖此
  通道语义）。
- FixStrategy: catalogOpenError 置空、新增 recoveryPendingError 信号/查询口，
  UI 以「待恢复」态呈现。
- RegressionTest: 断言恢复失败时 catalogOpenError 为空且新通道非空。

### A-08 F1「预览后出现」Skipped 行排在结果行首，违反 importFolder 行序契约

- Severity: P3 | Confidence: VERIFIED | Module: src/io
- File:Line/Function: src/io/dataimport_folder.cpp:403-431 + :466-467
  `produceFromPlan`；契约 src/io/dataimportservice.h:90-93
- Root Cause: F1 门把迟到行直接 `sess.rows.append(row)`（:427），执行行随后
  `sess.rows += executedRows`（:467）——迟到 Skipped 行位于 well_head 行之前；
  契约承诺「well_head 行在前、其余随后、Skipped 行缀在最后」。
- Trigger Condition: 确认执行期目录重扫出现预览后新增文件（F1 门触发）。
- Observed/Expected: [迟到Skipped, 执行行…, 枚举期Skipped] / Skipped 全部缀尾。
- Reproduction: 预览后向源目录拷入新 CSV 再确认导入，检查返回行序与台账。
- Impact: 结果表/台账行序观感漂移；无数据损害（行内容正确）。
- IssueMatch/PRMatch: 无冲突。
- FixStrategy: 迟到行先进暂存表，最后与 plan.skipped 合并追加。
- RegressionTest: tst_import_ux lateArrivingFilesAreSkippedNotImported 增加行序断言。

---

### 观察项（不立案）

- O-1 storeManagedRaw 落位前仅 flush 无 fsync（dataimport_session.cpp:169-171）：
  窗口前既有 + #587 fsync 排除族；dedup 库内复核可自愈。EXCLUDED_EXISTING_ISSUE(#587)。
- O-2 非井类输入 hash→copy 间 TOCTOU（dataimport_file.cpp:170/357-359 仅对
  textWellInput/wellLogInput 复核 sha）：窗口前既有，版本记录与落盘字节仍自洽。
- O-3 ShaCache miss 全表序列化写放大（20261021 A-01，P2 在册未修）本轮未见
  改动，仍成立。
- O-4 F5 坐标回填会顺带覆写 kb/td（dataimport_wells.cpp:134-160）：仅当井
  coordinateStatus=unknown 或 !hasSurface——可达性依赖「有 kb/td 无坐标」的
  非常规态，判断不可达，不立案。
- O-5 wellcompositexml 离散曲线 sentinel 用 `v > -900`，welllogtext 用
  -999.25/-9999.0→NaN：两读取面 sentinel 词表微差，无实际冲突数据形态。

## 3. 模块健康结论

**总体判断：健康。** #603 是本窗口唯一大增量，其核心承诺「预览-确认-提交安全链」
经代码追踪与 tst_import_ux 34 例（指纹复核 F2/F3、迟到文件 F1、井名防撞 F4/F6、
崩溃恢复 6 例含 tamper 拒绝、conflict 闸、行级回滚、目的碰撞回滚）与
tst_welllogtext 格式契约对照，事务链自洽：

- 路径安全：resolvedVersionPath 逐段 isSafePathSegment + 符号链接拒绝 + canonical
  包含性检查，恢复面（importrecovery）经同一闸，无穿越。
- 原子性：staging→rename→journal 重放→失败双路回滚（removeMovedFiles +
  intent 恢复），崩溃任一点可收敛，已注册字节永不误删。
- catalog 并发：staging 副本 + mutationSeq/epoch 闸 + updateEntity 新枚举尾插，
  索引（无名字键）不受行内改写影响。
- 行级隔离：rollbackFailedRow 检查点含全部 COW 容器与 journal，无半截资产入库。

弱点集中在**生态一致性与重复劳动**而非事务正确性：编码（A-02）、词表（A-03）、
错误文案（A-01/A-07）、行序契约（A-08）、以及同一文件在 plan/preview/commit 三段
的重复全量解析与哈希（A-04/A-05）。均为 P3，可随下一个 io 打磨批次收敛。

修复池建议：8 项 VERIFIED 全部入池；优先序 A-02 > A-05 > A-03 > A-01 > A-07 >
A-04 > A-08 > A-06（按真实数据可用性→交互延迟→文案→硬化的顺序）。
