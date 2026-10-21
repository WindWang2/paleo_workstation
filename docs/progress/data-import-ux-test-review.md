# 数据管理导入 UX（PR #603）测试质量独立评审

评审对象：worktree `C:\Users\wangj.KEVIN\.codex\worktrees\data-management-import-ux\paleo_workstation`
评审方式：只读。逐场景映射到真实测试函数 + 断言密度抽查 + 旧测试 diff 分类 + 运行证据核对。
评审日期：2026-10-20

## 一、18 场景映射表

| # | 场景 | 测试函数（套件） | 覆盖质量 | 备注 |
|---|------|----------------|---------|------|
| 1 | 单个完整 XML 导入 | `completeXmlCreatesOneWellAndThreeReadableCurves`（tst_import_ux）；`xmlExactUnionAndUnits`/`existingRepositoryCompositeFixture`（tst_welllogtext） | 真实端到端 | 文件→catalog→井/资产/链接→`WellLogSet::readCurveTvd` 业务读回逐值断言 + 受管字节与源字节逐字节相等；解析器层另有仓库真实夹具 composite_sheet.xml 对拍 |
| 2 | 单条曲线文件导入 | `singleCsvCreatesReadableCurve`（tst_import_ux） | 真实端到端 | 导入→曲线索引→读回 md/values 逐值 |
| 3 | XML 与单曲线混合批量导入 | `mixedBatchAndLaterAdditionShareExistingWell`（tst_import_ux）；`folderConfirm_conflictChoicePreservesXmlWhileImportingCsv`（tst_panels，真 UI 对话框+真服务）；性能套件批量段 | 真实端到端 | 含 UI 层确认对话框驱动真实 FolderImportWorkflow |
| 4 | 同一口井多来源自动归并 | `mixedBatchAndLaterAdditionShareExistingWell`（tst_import_ux）；`representativeXmlBatchQueriesAndCancellation`（tst_import_performance，XML+24 CSV 归并到 1 井）；`realMixedImportOpensBothAssetsWithPerSourceMetricDepths`（tst_import_preview） | 真实端到端 | 断言 entities(well)==1、links、曲线并集 |
| 5 | 已有曲线与新增曲线共存 | `sameMnemonicDifferentValuesRetainsBothSources`；`mixedBatchAndLaterAdditionShareExistingWell`（tst_import_ux）；`newShaVersionDemotesPreviousPrimaryLink`/`consecutiveResolvedLasKeepsFirstPrimary`（tst_import） | 真实端到端 | 首份保 isPrimary/ordinal 0、后份非主 ordinal 1，两条都可读回原始值 |
| 6 | 重复导入相同文件 | `duplicateShaAcrossNamesDoesNotGrowEntitiesOrVersions`（tst_import_ux）；`sameShaReimportDoesNotDuplicate`/`folderPlanRerunRegistersNothingTwice`（tst_import）；`importFolderForceImportFlipsDuplicate`（tst_folderimport） | 真实端到端 | mutationSeq/资产/版本/链接/raw 文件数全零增长 |
| 7 | 同名不同内容冲突 | `sameMnemonicDifferentValuesRetainsBothSources`+`duplicateShaCannotSilentlyIgnoreDifferentTarget`（2 行 data）+`previewDigestRejectsChangedSourceBeforeAnyEntityIsCreated`（2 行 data，含同尺寸同 mtime 篡改）（tst_import_ux）；`folderConfirm_uncheckConflictSkipsIdentityRequirement`（tst_panels） | 真实端到端 | 预览 issue 文案、拒绝后目录零变化、保留两份且各可读回 10/90 |
| 8 | 井号相似但不能确定同井 | `similarWellNamesRequireExplicitSelection`（tst_import_ux，A1 vs A-1）；`ambiguousWellNameRequiresExplicitTarget`/`planAmbiguityRequiresExplicitTarget`（tst_import） | 真实端到端 | 拒绝→零入库→显式选井→持久化往返断言 |
| 9 | 深度单位不同 | `unitsAndSamplingRemainPerSource`（tst_import_ux，m+ft 混导，ft→m 换算 1e-9 精度）；`realMixedImport...`（tst_import_preview，UI 显示换算）；`unifiedFormatsReachMdConsumers` feet 行（tst_welllog_consumers） | 真实端到端 | 源文件保留 ft 原值、业务侧米制 |
| 10 | 深度采样间隔不同 | `unitsAndSamplingRemainPerSource`（tst_import_ux）；性能套件断 500 点 CSV 未被扩到 20000 点网格 | 真实端到端 | 按源保留采样，无重采样 |
| 11 | 缺失值和非法数据 | `missingSamplesRemainNan`；`invalidIndicesAndSamplesAreRejected`（7 行 data：非法/缺失/重复/倒序深度、非法值、列宽、缺分隔符）（tst_import_ux）；`lasParsesCurvesAndNulls`/`lasFailureModes`（tst_import） | 真实端到端 | NaN 位置逐点断言；拒收后目录/库零残留 |
| 12 | 损坏 XML 或其他异常文件 | `malformedXmlIsBlockedWithoutLosingValidBatchMember`（tst_import_ux）；`xmlFailures`/`malformedText`（tst_welllogtext）；`truncatedDlisRejectedBeforeRegistration`（tst_import）；`mixedGoodBadImportIsHonestAndLedgered`（tst_folderimport） | 真实端到端 | 坏行失败不影响好行入库；截断 DLIS 注册前拦截 |
| 13 | 导入中取消 | `cancelledSessionAndPlanCommitNothing`（session+plan 双路径）（tst_import_ux）；`cancelMidImportLeavesCatalogConsistent`+`folderImportProducedOnWorkerCommittedOnOwner` 取消段（tst_import）；`folderConfirm_closeInFlightCancelsOnlyOnce`（4 行 data：reject/close/Esc/析构）（tst_panels）；`batchCancellationHandleGuardsCommitAndKeepsSameSourceJob`/`queuedBatchCancellationHandleNeverStartsWorker`/`singleFileImportCancelledDiscardsCommit`（tst_folderimport）；性能套件取消段 | 真实端到端 | 取消后整批回滚、staging 删除、可重导；UI 关闭只取消一次且析构后异步回包安全丢弃 |
| 14 | 写入中异常及失败恢复 | tst_import_ux 恢复族 8 例：`destinationCollisionRollsBackAndCanRetry`、`partialMoveFailureRestoresEarlierFilesAndProtectsCollision`、`interruptedPartialMoveCleansOnlyThisSessionsUnregisteredBytes`、`catalogWriteFailureRollsBackMovedBytesAndCanRetry`（debugAbortJournalAfter 注入）、`interruptedUnregisteredBytesAreRemovedOnReopen`、`interruptedRegisteredBytesSurviveReopen`、`recoveryRefusesTamperedBytesAndPreservesEvidence`、`catalogMutationConflictUsesFreshSession`；tst_import：`failingCommitSurfacesErrorAndRollsBack`、`orphanedManagedFilesFromCrashedCommitDoNotBlockImport`、`missingManagedRawCanBeReimported`、`sweepStaleStagingRemovesLeftovers` | 真实端到端 | 模拟断电点（rename 与 journal 之间）用真实 intent 文件与真实受管字节布局，非 JSON 臆造；篡改字节重开被拒且证据保留 |
| 15 | 旧工程重新打开 | `oldLasProjectWithoutNewMetadataRemainsReadable`（tst_import_ux）；`oldLasWithoutStoredMetadataReopensAndDrawsOriginalGr`（tst_import_preview，真实 UI 画图）；`dataTree_legacyLogWithoutMetadataKeepsFilenameAndPrimary`（tst_panels）；`interruptedRegisteredBytesSurviveReopen`（tst_import_ux） | 真实端到端 | 无新元数据的旧 LAS：可读、可画、可与新导入共存、源字节不动 |
| 16 | 导入后进入测井模块使用 | `realMixedImportOpensBothAssetsWithPerSourceMetricDepths`+`managedHeaderMetadataAndRealUiKeepCurveSourcesConsistent`（tst_import_preview，经 DataPreviewTabs→WellCompositePanel 真实 UI 入口）；`unifiedFormatsReachMdConsumers`（7 行 data，PropertyModelWorkflow+SectionWorkbench 双消费端）（tst_welllog_consumers） | 真实端到端 | 非 helper 单测：开资产→面板 currentData 逐值断言 |
| 17 | 数据管理修改后业务模块正确刷新 | `reopenKeepsIdentityAndCatalogRemovalRefreshesBusinessIndex`（tst_import_ux：removeAsset→QSignalSpy changed→曲线索引 4→3→重开仍 3）；`dataTree_importedXmlAndCsvShowCurveSummaryAndKeepAssetActivation`（tst_panels） | 真实断言但面窄 | 仅覆盖 catalog 删除→曲线索引/数据树刷新；剖面/编图等业务模块对数据管理修改的刷新无断言（见缺失清单） |
| 18 | 单因素/QGIS/预测/编图兼容回归 | 单因素：tst_singlefactor_acquisition/tst_singlefactor_structural（未改，19 套件内跑绿）；QGIS：tst_mappingworkbench（qgis 标签，173s）+tst_import 全程 QgisRuntime；预测：tst_mappingworkbench 新增 3 例（`directWellPredictionRejectsUnsafeTextIndex` 4 行/`predictionPreparationRejectsNonMdTextWithoutMockFallback`/`predictionLogAcceptsUnifiedMdAndRejectsOtherAxes`）；编图：tst_mappingworkbench 既有面 | 部分覆盖 | 新导入的 XML/CSV 曲线→单因素/编图管线无端到端用例；19 套件未含 tst_mapping/tst_mappingpages/tst_composeworkflow/tst_faciesmapping_*（见缺失清单） |

## 二、断言密度抽查（无摆设性测试）

- tst_import_ux.cpp（抽 5 例）：`completeXmlCreatesOneWellAndThreeReadableCurves` 16 断言；`duplicateShaAcrossNamesDoesNotGrowEntitiesOrVersions` 11；`previewDigestRejectsChangedSourceBeforeAnyEntityIsCreated` 约 18（含 sha 对拍、mtime 保持篡改）；`partialMoveFailureRestoresEarlierFilesAndProtectsCollision` 15；`recoveryRefusesTamperedBytesAndPreservesEvidence` 10。全部断言到值/计数/字节/信号，无恒真断言。
- tst_import_preview.cpp（4 例全查）：`realMixedImport...` 循环内约 30 断言（逐深度/逐值/单位/hint 隐藏）；`unsafeSourceAxisIsExcludedButValidSiblingRemainsVisible` 10 行 data × 约 14 断言；`managedHeaderMetadata...` 约 20；`oldLas...` 约 15。QgisRuntime 真实初始化。
- tst_import_performance.cpp（1 例）：约 60 行为断言（解析值、NaN 位置、目录计数、线程违规数、取消后零残留）+ 计时/内存仅 qInfo 报告，唯一墙钟闸门是 perfbudget 放宽后的 QTRY 超时。是「带性能观测的行为测试」，不是仅耗时摆设。

## 三、性能用例真实性

- 数据为**合成**（流式生成的完整 SpreadsheetML：20000 采样 × 8 曲线；24 个 CSV × 500 采样），非仓库真实样本；结构与 fixtures 同源、含 NaN 与中文 sheet 名，形状有代表性。
- 断言以行为正确性为主（见上），耗时/内存仅观测不卡线——定位正确。
- 仓库真实样本覆盖另有两处：tst_welllogtext `existingRepositoryCompositeFixture`（fixtures/outsource/composite_sheet.xml）；tst_import 用 testdata/project_area 真实件（A1.Las/ExportWellHead.dat 等）。但**真实整工区冒烟 `folderImportRealAreaSmoke` 被 `PALEO_REAL_PROJECT_AREA` 环境门控，未设置即 QSKIP**——本次 19 套件日志无法证明它跑过（大概率跳过）。

## 四、旧测试删改断言分类（git diff master -- tests/）

| 位置 | 分类 | 说明 |
|------|------|------|
| tst_import.cpp `ambiguousWellNameYieldsUnresolvedLinks`→`ambiguousWellNameRequiresExplicitTarget` | 适配新行为+加强 | 旧「静默未决链接」契约被「必须显式选井」取代；新增 QSignalSpy、拒绝零入库、显式选井后持久化往返 |
| `unmatchedWellLogLeavesUnresolvedLink`→拆为 `contentWellNameCreatesNewWellWithoutFilenameGuess`+`filenameOnlyLogRequiresExplicitTarget` | 适配+加强 | 内容井名优先于文件名；无井名不猜井 |
| `reimportAttachesNowResolvableLink`→`reimportContentIdentifiedLogKeepsExistingAssociation` | 适配 | 重导不再自动补挂（`!linkAttached`）；断言仍含 AlreadyStored/消息文案/持久化 |
| `consecutiveUnknownLasStayUnresolved`→`consecutiveContentNamedLasCreateDistinctWells` | 适配+加强 | |
| `planAmbiguityStaysUnresolved`→`planAmbiguityRequiresExplicitTarget` | 适配 | |
| `truncatedDlisImportNotesReason`→`truncatedDlisRejectedBeforeRegistration` | 加强 | 软备注变硬拒收，断言零资产零链接 |
| `folderImportWellHeadsFirstAndIsolatesFailures` 资产 6→5、链接 8→7 | 适配+加强 | 缺井名 ghost.las 不再留孤儿资产，并新增 `sourceUri` 不含 ghost.las 的断言 |
| `folderImportProducedOnWorkerCommittedOnOwner` 取消语义 | 适配 | 「已处理行随提交保留」→「整批回滚」；mutationSeq/资产数/信号数不变断言全保留 |
| tst_welllogset.cpp 拒收理由文案 | 适配 | "no curve definitions"→"数据列数与表头不一致"（新文本解析器），拒收事实仍断言 |
| tst_panels.cpp 未决资产视图 Z9.las→tops.dat | 适配（边界） | well_log 按设计不再产生未决行，改用 tops 走同一 UI 路径；注释明记决策。不算放水，但「las 未决行进未决视图」这条旧面随之消失 |
| tst_import.cpp 汇总 | 净加强 | 删 83 断言、增 131 断言；删除项几乎全部属于已删除的「静默未决」旧契约 |

**放水项：未发现。** 唯一命名债：`newShaVersionDemotesPreviousPrimaryLink` 名与现语义相反（现为首份保主），建议改名但不影响断言有效性。

## 五、运行证据

`build/final-tests-v2.log`：Windows worktree 构建目录、19 套件按 1/19…19/19 串行跑、**100% passed**（总 326s；tst_mappingworkbench 173s、tst_import 71s、tst_folderimport 21s、三个新套件 10/7/16s）。注意这是**定向子集**（测试编号到 #425，全量远不止 19 个），不能当作全量回归绿证据。

## 六、缺失/薄弱场景清单（按风险排序）

1. **场景 18 编图/单因素与新导入曲线的端到端**（中）：单因素两套件不断言 well_log 内容；无「导入 XML/CSV→单因素成图/编图消费」用例；19 套件未含 tst_mapping/tst_mappingpages/tst_composeworkflow/tst_faciesmapping_*，建议合并前在 CI 或本地全量 `ctest -LE 'core|perf'` 补一次全量绿证据。
2. **场景 17 业务模块刷新面窄**（中）：仅曲线索引+数据树；剖面（wellsection）、编图画布等在数据管理删除/重导后的刷新无断言。
3. **真实样本性能/冒烟缺位**（中低）：性能用例全合成；`folderImportRealAreaSmoke` 环境门控大概率跳过，真实 XML 大文件的解析性能与分类正确性无跑过证据。
4. **命名债**（低）：`newShaVersionDemotesPreviousPrimaryLink` 名实不符。

## 结论

18 场景全部有真实断言覆盖（无「缺失」），场景 1–16 为强端到端；17、18 为部分覆盖。抽查 11 个用例断言密度高、无摆设；旧测试删改全部为「适配新行为」或「加强」，未发现放水。主要残余风险集中在场景 18 的全量回归证据与编图/单因素集成面。
