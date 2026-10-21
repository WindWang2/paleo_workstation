# 统一数据导入独立验收评审（2026-10-20 / 2026-10-21）

评审人：独立验收评审员（只读，未修改被审代码）。
评审对象：worktree `C:\Users\wangj.KEVIN\.codex\worktrees\data-management-import-ux\paleo_workstation`，
分支 `codex/data-management-import-ux`，单笔提交 `9aba6893`，79 文件 +6302/−966，对应 GitHub PR #603。
评审基线：`origin/master = e6837e14`（经 `git merge-base` 核实，分支基于最新 master）。
所有结论均以真实代码 / 测试 / 日志 / git/gh 只读命令核实，未采纳 PR 描述与 docs 自评。

## 逐条裁决表（14 条完成标准）

| # | 标准 | 结论 | 证据（文件:行号） | 备注 |
|---|------|------|------|------|
| 1 | 已阅读最新 master、相关 PR、Review、Issues 和开发分支 | 完成（一处小缺口） | `docs/progress/data-import-ux-audit.md:3`（基线 e6837e14，与实际一致）；`audit.md:35-44` 记录实查 #580/#581（gh 核实均 OPEN，标题相符）、收尾查 #599/#600/#601（gh 核实均 OPEN 未合并）。审计声明与仓库实际状态相符。 | 完成标准要求核对到 #602；audit 未点名 #602（该 PR 2026-10-20 22:44 +0800 创建，修 #582–#597 全项目评审项）。audit 以「其余开放审计 issues 不纳入」一句兜底，#602 本体（fix PR）未被点名评估重叠。轻微缺口，不阻断。 |
| 2 | 基于真实源码确定最小改造范围 | 完成 | 抽查 3 条：① audit.md:10「sniffWellXml 表名仅精确匹配」→ 属实，`src/domain/wellrecords.cpp:17-21` kSheetNames 精确表 + diff 显示本次才补 `测井曲线-`/`离散曲线-` 前缀（wellrecords.cpp:50-51）；② audit.md:11「分派只有 LAS/DLIS/LIS」→ 属实，`src/io/welllogread.h:21-29` 本次新增 SpreadsheetXml/Text 枚举；③ audit.md:15「folderconfirm 先下一步再确认」→ 属实，旧 `folderNextButton`/`folderBackButton` 已删除，`tests/tst_panels.cpp` 新测试显式断言两者不存在（folderConfirm_directPreviewAndExplicitWell 内 QVERIFY）。 | 问题清单每条均带真实代码位置，抽查全中。 |
| 3 | 没有盲目重建数据管理架构 | 完成 | 新增文件全部作用于既有 DataCatalog：`src/io/dataimport_log.cpp:175-193` 只建 EntityAssetLink（role=well_log）；`src/io/importrecovery.cpp:70-107` 只清理 staging 孤儿；`src/io/importclassification.cpp:7-35` 纯分类函数。无新数据库/新持久化 schema——grep 新文件零命中 QSqlDatabase/CREATE TABLE/Kafka/S3/WITSML；CMake 变更仅 `CMakeLists.txt:1564-1565` 登记两个 extra-*.cmake，无新依赖。staging/journal 复用：`src/io/dataimport_session.cpp:282`（createStagingCopy）、:441（journal 重放）。 | 无平行数据树。版本 extra 新增 logCurves 等键是既有 QVariantMap 扩展，非新 schema。 |
| 4 | 完整 XML 和单条曲线进入统一数据管理体系 | 完成 | 统一分派门面 `src/io/welllogread.cpp:34-59`（detect）、:186-190（parse 的 SpreadsheetXml/Text 分支）；XML→公共契约适配 `src/io/welllogtext_convert.cpp:130-191`（输出 LasHeaderInfo+LasCurve，工作表 `测井曲线*`/`离散曲线*`，welllogtext.cpp:161）；文本单曲线 `src/io/welllogtext.cpp:67-107`（WELL 列/井名列识别、索引列强制），`welllogtext_convert.cpp:122-123` 只要求「索引+至少一条曲线」，单曲线不被当无效。分类接入 `src/domain/projectclassifier.cpp:108-125`（内容自声明优先于扩展名，LAS 头/XML/索引表头）。全部经 importOneFile 的 well_log 分支 → `dataimport_log.cpp:161 importInspectedLog` → well_log 资产+链接。 | — |
| 5 | 同一口井的数据合理关联 | 完成 | `src/services/welllogset.cpp:138-232 prepare`：按井收集全部已决 well_log 链接、跨文件并集曲线、主文件/ordinal 排序；`welllogset.cpp:91-130 disambiguateAcrossFiles`：跨文件重名加 `@basename`/`#versionId` 别名，两份都保留。XML 与后补 CSV 同井并集有测试：`tests/tst_import_ux.cpp:965 oldLasProjectWithoutNewMetadataRemainsReadable`（旧 LAS 工程重开后导入 CSV 落同一 wellId，:1010 `QCOMPARE(h.wellId(), well.id)`）。 | — |
| 6 | 不确定的井匹配不自动错误合并 | 完成 | 内容井名唯一精确匹配才自动建议：`src/io/dataimport_log.cpp:131-136`（wellsMatchingName==1 且名称精确相等才 suggestedWellId）；多匹配/零匹配 → `needsWellChoice`，:153-158 明示「系统不会自动合并」「文件名仅作线索」。执行侧不猜：`dataimport_log.cpp:23-45 ensureWell`（歧义拒绝、不建井）、`src/io/ingestplan.cpp:380-391 proposeWellLog`（只用内容井名，文件名不在候选列；匹配到但名称不等即清 entityId 标歧义）、`ingestplan.cpp:265-296 proposeWell`（≥2 候选即 entityAmbiguous 不再试下一个）。文件名补挂通道已删除并有防回归测试 `tst_import_ux.cpp:920 retypedReferenceCannotAttachLegacyLogByFilename`。 | 语义从「未决链接入库」改为「无显式选择不入库」，属有意收紧，docs/import-well-logs.md:59 记录旧未决关联修复出口。 |
| 7 | 预览、校验和可靠提交机制 | 完成 | 预览零写入：`dataimport_log.cpp:105-159 fillLogPreview` 只读 catalog/文件；预览期双指纹（:112/:121 前后 SHA 不一致即 importBlocked）。执行复核：`src/io/dataimport_file.cpp:174-175`（expectedSourceSha256 比对）、:203-209（QTemporaryDir 私有快照+快照 SHA 复核）、:356-365（受管 RAW SHA 终验，不符即撤暂存拒绝）；批量入口 `src/io/dataimport_folder.cpp:502-508`（expected hash 前置）。提交=staging+journal+事务：`dataimport_session.cpp:279-282` staging 副本、:441 journal 重放、取消/失败整批回滚（tst_import.cpp 取消用例断言 mutationSeq 不变、staging 目录清除）。崩溃恢复 `src/io/importrecovery.cpp:32-67`（commit-intent.json 清单+SHA 吻合才清理，验证先于删除）。 | — |
| 8 | 重复导入和数据冲突合理处理 | 完成 | 同字节跳过：`dataimport_file.cpp:271-301`（versionBySha256 命中 → AlreadyStored，零新资产/版本/链接；well_log 去重不补挂，:288）；冲突目标前置拒绝 `dataimport_log.cpp:204-225 validateDuplicateLogTarget`（显式目标与既有归属不同即拒绝、不隐式建井；单文件与批量 `dataimport_folder.cpp:509-518` 两路均前置）。同名曲线不覆盖：`dataimport_log.cpp:143-147`（冲突列名预告「保留两份并标明来源」）、`welllogset.cpp:76-82`（文件内重名 `#列号`）、:117（跨文件 `@basename`）。冲突行可跳过（确认表复选框，folderconfirm.cpp:97-99）/重试（folderconfirm.cpp:210-227 失败行重试按钮）。 | — |
| 9 | 普通导入流程最多三步 | 完成 | 单对话框直接确认：`src/ui/dialogs/folderconfirm.cpp:385`「确认导入」按钮直连 beginImport（:397），无「下一步」中转（grep folderconfirm* 零命中「下一步」；测试断言 folderNextButton 不存在）。无歧义行零字段：`folderconfirm_preview.cpp:95-96`（仅 needsWellChoice 才插井选择控件）、:149-151（手改类型为测井才补控件）；确认按钮在井未决时禁用并给因（:172,:179-181）。 | — |
| 10 | 数据管理界面简单自然、符合现有习惯 | 完成 | 复用既有树与对话框：`src/ui/pages/datalist_logsummary.cpp:8-40` 只扩展既有 describeWellAssetNode（显示「测井数据 · N条曲线」，版本号/来源仅进 tooltip，不作树节点）；datalist_tree.cpp 净收缩（1094→1059 行）；folderconfirm 为原对话框重构（拆分 preview/results 两文件），无新窗口体系。grep datalist_tree.cpp 增量无 versionId/sourceId 节点。 | — |
| 11 | 现有工程及核心业务兼容 | 完成 | 无强制迁移；旧版本缺新 extra 的回退：`src/services/welllogset.cpp:176`（storedHeader 失败 → WellLogRead::parseHeader 整解析兜底）、:272-283（storedHeader 缺 logCurveNames 即返回 false）。测试：`tests/tst_import_ux.cpp:965 oldLasProjectWithoutNewMetadataRemainsReadable`（extra.size()==0 旧工程重开、readCurveTvd 正常、后续 CSV 导入挂同井）、`tst_panels.cpp` dataTree_legacyLogWithoutMetadataKeepsFilenameAndPrimary。注意：导入语义有意收紧（歧义/无井名不再以未决链接入库，须显式选择）——属功能裁决而非兼容破坏，docs/import-well-logs.md 已记。 | — |
| 12 | 通过本地编译、核心测试及回归 | 完成 | `build/final-build.log`（2026-10-20 23:05）：785/785 步全部完成，末步链接 tst_wellsection_workflow.exe，无错误尾。`build/final-tests-v2.log`（2026-10-21 00:10，提交 00:11:59 前 2 分钟，同一代码树）：19/19 套件 100% 通过，含新增 tst_welllogtext(#235)/tst_import_ux(#236)/tst_import_preview(#237)/tst_import_performance(#238)；final-tests.log 另有一轮全绿（非单次侥幸）。提交时间与日志时间相邻，无先提交后补测的迹象。 | r2 声明「本轮未编译」——指 r2 静态审查自身，最终构建/测试日志存在且晚于终审。file_budget 闸有一项红，见标准13备注。 |
| 13 | 完成独立 Review–Fix–Review 闭环 | 完成（一项文档遗留） | 链条：review-1（R1–R8，含「预览自动归井被固化为显式覆盖」等真实代码定位）→ review-2 → review-2a（R1b 预览/执行指纹窗口）→ review-final（:6「静态放行」，逐条给修复后代码证据，含 R1b 已闭环：快照+expected 贯穿）→ 终审后增量复审 r2（:127 放行，遗留 3 项非阻断）。r2 遗留核对：(a) welllogset.h:34 format 行内枚举不全——已修（现列全 6 值，welllogset.h:34）；(b) audit.md:12「XML 依赖文件名主名回退」陈旧措辞——**未修**（该行原样仍在，描述的是改造前状态，r2 自评为轻微）；(c) file_budget `constraintpage_interpolation.cpp:fn:updateEngineRows 230 行`红——经核实为 master 既有债（本分支 diff 不含该文件；master baseline 0 条该函数登记；本地复跑 check_file_budget.py 复现），分支未靠补登基线转绿，符合 #539 纪律。 | 闭环成立；遗留 (b) 为文档措辞，不阻断。 |
| 14 | 已提交独立 PR | 完成 | `gh pr view 603`：state=OPEN、mergedAt=null、base=master、head=codex/data-management-import-ux、commits=[9aba6893] 单笔。`git merge-base HEAD origin/master` = `origin/master` = e6837e14，基于最新 master；`git worktree list` 证实分支独立于主 checkout。 | — |

## 执行约束核对（4 条）

| # | 约束 | 结论 | 证据 | 备注 |
|---|------|------|------|------|
| C1 | 工作树独立于主 checkout | 遵守 | `git -C C:\Users\wangj.KEVIN\projects\paleo_workstation status --short` 输出为空（干净）、log 停在 e6837e14（=origin/master）；`git worktree list` 主 checkout 与 codex/data-management-import-ux worktree 并列独立。 | 本验收报告是按任务要求写入主 checkout 的 docs/progress/，属评审产物而非被审改动。 |
| C2 | 无过度设计（新数据库/微服务/Kafka/S3/WITSML 全对象/权限平台） | 遵守 | CMakeLists.txt diff 仅登记两个 extra-*.cmake（无 find_package/新第三方库）；新 io 文件 grep 零命中上述关键词；sniffWellXml 对 WITSML 根如实判 Unknown 不冒充支持（wellrecords.cpp:31-36 及 audit.md:10 的一致声明）。 | — |
| C3 | 测试失败不靠删测试/删断言解决 | 遵守 | 抽查三处：① tst_welllogset.cpp oracle6（:370-372）：断言从 LAS 时代的「no curve definitions」改为新文本解析器的「数据列数与表头不一致」——垃圾文件仍被拒收并告警，junkPath 仍在告警 blob 断言中，验证意图保留，属适配新行为；② tst_panels.cpp 两例（1828-1833/1862-1866 区）：未决场景夹具从 Z9.las 换成 tops.dat 引用未知井——因 well_log 语义收紧后不再产生未决行，改用仍有未决语义的分层数据验证同一「查看未决」页面，断言强度不变（rowCount==1+内容断言）；③ tst_import.cpp 取消语义（diff 483-513 区）：从「已处理行随提交保留」改为「整批回滚」，断言反而加强（新增 mutationSeq 不变、assets 数不变、importedSpy 计数不变、staging 目录清除）。其余重写（ambiguousWellNameRequiresExplicitTarget 等）是把「未决入库」改为「拒绝+显式选择后成功」的双向断言，含 catalog.json 往返持久化验证——语义收紧而非放水。 | 所有被改断言均保留或加强了原始验证意图。 |
| C4 | （隐含）基线纪律——只缩不涨 | 遵守 | tools/file-budget-baseline.txt：删 1 行（folderconfirm.cpp:fn:buildFolderConfirmDialog 已拆没）、收缩 3 处（278→263、1094→1059、676→638），无新增登记；tools/ui-size-baseline.txt 1094→1059 收缩。本地复跑：check_layering.py --strict 通过；check_ui_size.py clean（10/10）；check_file_budget.py 唯一红项为 master 既有债（见标准13(c)），未靠补登转绿。 | — |

## 总结论

**有保留达成。** 14 条完成标准全部达成（无「未完成」；第 1、13 条各带一处轻微文档/范围缺口），4 条执行约束全部遵守。

### 保留项清单（均非阻断）

1. **标准 1**：audit 未点名核查 PR #602（2026-10-20 22:44 +0800 创建的全项目评审修复 PR，与本分支潜在文件重叠未评估）；audit 以「其余开放审计 issues 不纳入」兜底，但 #602 是 fix PR 而非 issue。合并前建议快速比对重叠面。
2. **标准 13(b)**：audit.md:12「XML 依赖文件名主名回退」措辞陈旧（r2 已指出，未修；该行描述改造前状态，当前实现无此回退）。建议顺手修订，不影响代码正确性。
3. **标准 13(c) / 约束 C4**：file_budget 闸 `src/ui/pages/constraintpage_interpolation.cpp:fn:updateEngineRows`（230 行）红——已用真实证据确认为 master 既有债（本分支未触碰该文件、未补登基线）。该红会让本分支 file_budget 闸维持红，处置须在 master 侧或合并前由作者知会，**不应**由本分支补登基线转绿。
4. **语义变更知情确认**：well_log 导入的歧义/无井名处理从「未决链接入库」收紧为「拒绝+显式选择」，单文件 API（无 targetWellId 的 importProjectFile）对歧义井名现在返回失败。测试与文档（docs/import-well-logs.md:59）已同步，属有意裁决；合并前建议产品侧知悉。
5. **执行验证时间线**：final-tests-v2.log（00:10）早于提交时间（00:11:59）约 2 分钟——测试跑的是提交前同一工作树，内容一致，无证据表明提交引入了测试未覆盖的改动；如严格要求「测试覆盖提交字节」，可在 CI 车道复核。
