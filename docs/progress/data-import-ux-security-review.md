# 统一数据导入数据安全独立评审（第六轮，攻击性/故障注入视角）

对象：worktree `codex/data-management-import-ux`（PR #603），基线 master `e6837e14`。
评审方式：只读静态审查 + 故障注入推演；未编译、未运行测试、未改实现；本报告是唯一写入产物。
与既有五轮（review-1 / 2 / 2a / final / r2）的关系：本报告刻意不复述其结论
（预览指纹链、well_log 快照 TOCTOU 收口、dedup 目标校验、取消窗口、轴基准、
恢复清单验证等已被覆盖且已闭环）。以下全部为五轮**未记录**的发现，
每条标注「已证实」（代码路径逐步走通）或「理论」（依赖小概率前提）。

## 攻击面 1：TOCTOU 残留

### F1 [高 · 已证实] 执行期重新扫描目录：预览后新落盘的文件未经确认即入库

证据：
- 预览建 plan：`src/io/dataimport_folder.cpp:103`（`producePreview` → `planFor`）。
- 执行**重新**建 plan：`src/io/dataimport_folder.cpp:317-341`
  （`produceFolder` → `planFor(sess, dirPath, …)`，一次全新的目录扫描）；
  文件清单口径同理（`produceFiles` → `planForFiles`，344-385）。
- 确认表承诺「确认表逐行看到的就是将要执行的决策」（本文件 60-61 行注释）。
- `produceFromPlan`（390-435）只做 typeOverrides / forceImportPaths /
  skippedPaths / wellTargets / expectedSourceHashes 五张**按路径索引**的表——
  执行 plan 里多出的路径五表皆空：默认类型、自动归井、不跳过、无指纹复核。
- UI→执行闭包只传目录：`src/workflow/folderimport.cpp:457-466`（importFolder）、
  505-524（importFiles）。

触发路径：用户在确认对话框审阅期间（分钟级窗口），另一进程/同事/同步盘
向源目录拷入新文件 → 用户点「确认导入」→ 新文件不在用户审阅过的清单里，
却被完整执行（含 well_log 内容井名自动归井 `importInspectedLog`）。

建议（二选一）：(a) 执行前对「执行 plan 路径集 − 预览路径集」做差分，
多出一律 Skipped 并要求重新预览；(b) 冻结预览 plan 对象随确认传入执行，
不再扫描。差分逻辑放 io 层，不靠 UI 自律。

### F2 [高 · 已证实] 非 well_log 类型整链无内容指纹 + ShaCache 陈旧误去重

证据：
- 预览指纹只覆盖 well_log：`src/io/dataimport_log.cpp:109`
  （`fillLogPreview` 对非 well_log 行提前 return，`sourceSha256` 永不赋值）；
  UI 侧 `src/ui/dialogs/folderconfirm_preview.cpp:93-94` 只收集非空
  sourceSha256——故 `expectedSourceHashes` 对栅格/层位/井表/文档/地震恒空。
- 两道复核闸都对空 expected 放行：`dataimport_folder.cpp:501-507`
  （produceItem）、`dataimport_file.cpp:174-175`（importOneFile）。
- 非测井的 dedup 身份来自缓存哈希：`dataimport_file.cpp:169-171`
  （`ShaCache::sha256Hex(…, Fingerprint::MtimeSizeInode)`）、
  `src/io/ingestplan.cpp:542`（plan 期同口径）。
- 命中即静默跳过：`dataimport_file.cpp:271-300`（AlreadyStored，「字节已在库」）。

触发路径：robocopy /MIR、rsync -a 等同步工具默认**保留 mtime**——同尺寸
换内容且 mtime/size/inode 不变时 ShaCache 返回旧哈希 → plan 期 `重复→skip`
或执行期 `AlreadyStored` → 新内容永未入库，用户看到的行消息是「已在库」。
（对 well_log 无此问题：`logFileDigest` 恒新鲜读，dataimport_log.cpp:196-202。）
副作用：同一陈旧哈希会让井表流式复验**误报**「井表读取期间内容发生变化」
（dataimport_file.cpp:259）——新鲜流哈希 ≠ 陈旧缓存哈希，方向相反但同源。

建议：dedup 决策与 AlreadyStored 出口前，对源文件做一次真哈希复核
（库内副本复核已是真哈希，见 `ingestplan.cpp:826-835` 与
`datacatalog_versions.cpp:218-240`——源侧唯独这里信缓存，口径分裂）；
或确认表对所有类型行采指纹（非测井前缀哈希+大小亦可，比 MtimeSizeInode 强）。

### F3 [低 · 已证实] 重试/改类型路径的指纹覆盖核对（结论：基本闭环，一处陈旧回退）

- 行重试带指纹：`folderconfirm_results.cpp:154-156` →
  `paleomainwindow_project.cpp:78/143` → `FolderImportWorkflow::importFolderRow`
  （`folderimport.cpp:395-411`）→ `runInlineImport` 注入
  `expectedSourceHashes`（383）→ `produceItem` 501 行复核。已贯通。
- 「改类型」重导：retryRow 的 `force` 与指纹同传；预览期非测井行本就无指纹，
  执行期测井一律新鲜哈希——无陈旧窗口。
- 残留面：`hooks.importRow`（无指纹旧钩子）在生产两处接线点均未 wire
  （只 importRowWithWell 被接），目前是死代码/测试面；服务级
  `DataImportService::importFolderRow`（dataimport_folder.cpp:693-708）与
  `folderRowFor`（681-691）不接受指纹——显式单文件「仍导入」语义下可接受，
  但建议在头文件注释写明「此入口无预览指纹」，防未来 UI 误接。

## 攻击面 2：井误并

### F4 [高 · 已证实] 「新建井：X」撞名被静默并入已有井 X

证据：
- `src/io/dataimport_log.cpp:165-166`：target 带 `new:` 前缀 →
  `ensureWell(name)`；
- `ensureWell`（23-45）：`wellsMatchingName` 唯一命中且 trimmed 大小写不敏感
  相等 → **直接返回已有井 id**——不区分「用户明确新建」与「自动归井」两种意图；
- UI「新建其他井…」输入框不查重：`folderconfirm_preview.cpp:44-58`
  （`findData(target)` 只查组合框内重复，不对 `availableWells` 查撞名）。

触发路径：已有井 A1（用户知道那是**另一区块**的 A1）→ 导入曲线时故意选
「新建井：A1」→ 曲线挂到旧 A1，行结果显示成功，无「未建新井」提示。
对照：dedup 校验 `validateDuplicateLogTarget`（dataimport_log.cpp:209-219）
对 `new:` 撞名是「解析到既有井再核链接」的拒绝语义——两条路径对同一前缀
语义不一致。

建议：`new:` 语义改为「撞名即拒绝并提示改选已有井/改名」；或 UI 输入时
即时撞名禁用 + 执行层兜底拒绝。

### F5 [高 · 已证实] 测井先建井（无坐标）后，井位表永不回填坐标

证据：
- 测井零匹配建井：`dataimport_log.cpp:38-44`，`coordinateStatus="unknown"`、
  无 `hasSurface`、无坐标；
- 井位表命中已有井：`src/io/dataimport_wells.cpp:132-135`
  （`matches.size()==1` → `wid = matches.front()`，**只挂链接，实体坐标
  原样保留**）；全仓 `coordinateStatus` 写点仅三处（本处 + dataimport_wells
  建井两处），无任何回填/更新路径；
- 消费面：`src/workflow/sectionworkbench.cpp:223-224` 只收
  `ok/untransformed`——`unknown` 井被剖面静默排除，地图无井位。

触发路径：先导 A1 的测井 CSV（内容井名建井）→ 再导含 A1 坐标的井位表 →
井位表报告「挂上既有井」成功，但 A1 永远无坐标。导入顺序决定数据完整性，
无任何告警。本 PR 之前井只能由井位表创建（恒带坐标），该洞是
「测井内容建井」新行为引入的交互缺陷。

建议：井位表命中 `coordinateStatus=unknown`（或 `!hasSurface`）的已有井时
回填坐标与状态（含 georeference 换算），或至少落 issue + 台账可见告警。

### F6 [中 · 已证实] ensureWell 匹配口径边界（信息性结论 + 一个误导文案）

- 规范化口径：`DataCatalog::normalizeWellName`（datacatalog_entities.cpp:69-80）
  去空白/`-`/`_` 并小写——「A-1」「A_1」「a 1」互撞；
- 唯一规范化命中但原名不严格相等（如已有「A-1」、内容「A1」）→ 拒绝并入
  （dataimport_log.cpp:31-37），方向安全，但错误文案恒为「对应多口井」，
  单井相似名场景误导排障（测试 similarWellNamesRequireExplicitSelection 走的
  正是这条路）；
- 规范化零匹配但名字为 junk（如 `-`、`___`）→ 规范化后为空针 → 零匹配 →
  建名为 `-` 的井。建议 ensureWell 入口对「规范化后为空」的名字拒绝建井。
- 同名不同井（两区块皆 A1）按名并入是**设计语义**（名称即唯一身份），
  预览行显示「井 A1（既有）」由用户确认兜底——维持设计但建议文档显式声明
  该风险（多区块工区）。

## 攻击面 3：取消/中断一致性

### F7 [中 · 已证实] shp 族成员补齐在 produce 期直写工程目录，绕过 staging/journal/取消

证据：`dataimport_folder.cpp:545-546`（dedup 跳过行）、565-566（幂等跳过行）
→ `copyBundleMembersIntoVersion`（642-675）；对已提交版本
`cat->versionFilePath(v)` 解析到**工程目录**（staging 副本对非本批版本走
`m_dir`，datacatalog.cpp:177-181）→ `QFile::copy` 直写 + 置只读。
该写发生在 worker 线程、commit 之前：不入 journal、不进 commit-intent.json、
取消/失败/冲突重试均不回滚（`rollbackFailedRow` 只管暂存根，
ingestplan.cpp:866-907）。

触发路径：文件夹导入含 shp 族重复行（已有版本缺 .dbf/.shx）→ 成员补齐已
落盘 → 用户在执行段取消 → 整批按承诺「零入库」，但已提交版本目录里多了
新成员文件。「与旧行为一致」的注释成立，但它与本 PR 新增的「整批取消不入库」
承诺直接冲突，且恢复链（writeImportIntent/recoverOne）永远看不见这些字节。

建议：成员补齐改为暂存+提交期落位（与本批 staging 同生死），或在
failSession/discardStaging 面显式补删本次新增成员；至少文档声明此例外。

### F8 [中 · 已证实] 恢复失败 = 工程目录整体拒写（可用性陷阱）

证据：`src/io/importrecovery.cpp:56/63`（单条记录 hash 不符/删不掉 →
`fail`）→ `recoverImportIntents` 返回 false（104）→
`src/io/dataimportservice.cpp:103-104`：`m_catalogReady = false` →
全部导入入口拒绝（dataimport_file.cpp:144-148），错误文案要求用户
「检查文件权限或恢复记录后重试」。

触发路径：提交中途崩溃 + 被杀软/备份工具动过一个已搬文件（hash 不再吻合）
→ 下次打开工程：恢复整体失败、staging 全部保留、导入面关闭，直到用户手工
清理 `artifacts/staging/<uuid>/commit-intent.json`。fail-closed 方向正确
（证据保全），但把单文件级异常放大成工程级拒写，且无自助恢复出口。

建议：改为隔离降级——把无法处理的 intent 改名 `commit-intent.blocked.json`
保留证据、告警后继续打开；或提供「放弃该批次暂存」的用户确认入口。

### F9 [低 · 理论] 空 journal 会话跳过 mutationSeq 冲突检查 + textWellInput 先复制后登记的孤儿 RAW 窗口

证据：`dataimport_session.cpp:452`（`m_catalog->mutationSeq() != s.baseSeq
&& !ops.isEmpty()`——空 ops 会话不查冲突）；`dataimport_file.cpp:370-371`
textWellInput 在 storeManagedRaw（354）**之后**才 addAsset。若 staging 副本
上 addAsset 失败（纯内存校验，实际几乎不可达）：暂存 RAW 已存在而 journal
为空 → `rollbackFailedRow` 按 AddVersion ops 收集目录收不到它
（ingestplan.cpp:876-891）→ 批量路径下 `writeImportIntent` 把它写进清单 →
commitImport 的半截 journal 拦截（445）因 `ops.isEmpty()` 不触发 →
moveStagedFiles 搬进工程、applyJournal(空) 成功 → **失败行的 RAW 字节入库
且永不登记**。

建议：commitImport 增加「有暂存文件但 journal 为空」的显式拒绝+清理
（一行不变式检查），把理论窗口钉死。

### 本面已走通、无新发现的点（与终审一致，仅记录推演结果）

- 崩溃矩阵四点位（intent 后/move 中/move 后 journal 前/journal 后）：
  recoverOne + sweepStaleStaging 组合后均可收敛到一致态；
  `stagedSource` 存在性判别（importrecovery.cpp:53-54）防止把未搬出的
  暂存当目标误删；已注册版本按 `registered` 跳过。
- stagingRoot 按 UUID 隔离（dataimport_session.cpp:279-281），恢复不跨会话；
  跨进程并发由 catalog 写锁 + `isLockedReadOnly` 早拒（dataimport_file.cpp:151-152）。
- 双会话提交：commitImport 强制 owner 线程（419-423）天然串行；冲突检查
  （452-465）+ workflow 侧 Conflict 重试新基线（folderimport.cpp runImportJob
  的 mayRetry 分支）；已取消作业不重试。未发现可执行漏洞。
- `recoverOne` 先全量验证后删除、`resolvedVersionPath` 逐段拒符号链接与
  `..`（datacatalog_versions.cpp:344-375）——未证实任何跨根删除路径。

## 攻击面 4：覆盖与丢失

### F10 [低 · 理论] POSIX 上 moveStagedFiles 的 exists→rename 竞争可覆盖第三方同名文件

证据：`dataimport_session.cpp:326-341`——先 `QFileInfo::exists(dst)` 拒绝、
后 `QFile::rename(f, dst)`；POSIX `rename(2)` 原子覆盖已存在目标，两步之间
窗口内落入的同名文件会被覆盖（Windows 上 `QFile::rename` 对已存在目标失败，
无此窗口）。进程内 owner 串行 + UUID 目录使前提苛刻（需外部进程恰好同刻
向 `artifacts/raw/<ast>/<ver>/` 落同名文件）。

建议：POSIX 侧用 `renameat2(…, RENAME_NOREPLACE)` 或 link+unlink 兜底，
或在 rename 成功后立即对 dst 做一次来源哈希比对（与 intent 记录不符即回滚）。

### 本面核对为安全的点

- moveStagedFiles 对已存在目标一律拒绝（326-330）——用户手工放进 raw/ 的
  同名文件不被覆盖（代价是整批失败直到人工清障，可用性小刺，方向安全）；
  提交失败 `removeMovedFiles` 只删本次 moved 清单内的文件（347-364）。
- `paleoReplaceFile`（atomicfile.h:52-63）在 import 面只用于 staging 内
  UUID 新目录（storeManagedRaw），无既有目标可撞；MOVEFILE_WRITE_THROUGH
  保证落盘序。
- 受管文件只读位的清除点共 5 处（dataimport_file.cpp:207 快照/362 错配暂存、
  dataimport_session.cpp:220 discardStaging/352 removeMovedFiles、
  importrecovery.cpp:62 恢复删除、ingestplan.cpp:896 行回滚）——全部位于
  「即将删除该文件」的路径上，删除失败时文件以可写态残留但
  recoveryPending/intent 保留，重开工程时恢复链补刀，窗口自愈。
- `discardStaging`/`sweepStaleStaging` 的 removeRecursively：根路径恒为
  `projectDir/artifacts/staging/<uuid>`（不可注入）；junction/symlink 处理依赖
  Qt 语义（现代 Qt 对 junction 按链接删除不深入）——攻击者需先有工程目录
  写权限，理论低危。建议补一个「staging 内塞 junction」的删除方向测试钉死
  Qt 行为假设。

## 攻击面 5：解析器安全

### F11 [中 · 已证实] 井名长度无上限：单行井名可膨胀到 ~64MB 并写入 catalog

证据：
- 文本 `# Well:` 行（welllogtext.cpp:84-91）、WELL 列单元格
  （welllogtext_convert.cpp:57-60）、SpreadsheetML 表名井名约定
  （welllogtext_convert.cpp:23-30）均无长度上限；文件级 64MB 闸
  （welllogtext.cpp:147-151）是唯一天花板；
- `DataCatalog::addEntity` 只校验 id（datacatalog_entities.cpp，
  `e.id.isEmpty() || hasEntity(e.id)`），name 不校验；
- 后果链：64MB 井名 → catalog JSON 膨胀 → `wellsMatchingName` 每次导入
  对每个既有井跑 `normalizeWellName`（O(名长)）→ 后续所有导入慢性劣化；
  UI 组合框/标签渲染巨串卡死。

建议：解析层（readText/appendRow/sheetWellName）或 ensureWell 入口限长
（建议 ≤128 字符 + 拒绝控制字符），超限如实报错。

### 本面核对为安全的点

- 大小闸在所有统一入口生效：WellLogText::parse 是唯一进入
  readText/convert 的门（welllogread.cpp:17-31 的 parseAdditional），
  64MB/400 万格/256 列三闸（welllogtext.cpp:147-151、102、61；
  welllogtext_convert.cpp:90-91、155-156、162-163 并集放大闸）；
  LAS/DLIS/LIS 另有 500MB 同口径闸（lasparser.cpp:20/650、
  lisparser.cpp:1017、dlisparser.cpp:1354）。
- SpreadsheetML：QXmlStreamReader 流式、不展开 DTD 自定义实体（billion
  laughs 无落点）；逐 token 迭代无递归（深嵌套不爆栈）；ss:Index 行/列
  上限 + MergeAcross 跨度闸 + 累计格数预算（outsourceworkbook.cpp:44-53、
  151-300），padTo 分配被列上限钉住。
- 数值注入：索引与曲线值均 `toDouble(&ok)` + `std::isfinite` 双闸
  （welllogtext_convert.cpp:63-65、75-77）——`nan`/`inf`/超长数字串
  （溢出 ok=false 或 inf 被 isfinite 拒）均如实失败；`NaN/NA/NULL` 哨兵
  词表显式（41-45）。LAS 侧 fastDouble 快路注释声称与 QByteArray::toDouble
  逐位一致（lasparser.cpp:71-87），未深入复验（既有代码，非本 PR 增量）。

## 攻击面 6：错误路径资源泄漏

### F12 [低 · 已证实] well_log 快照落在 %TEMP%，删除失败只告警不兜底

证据：`dataimport_file.cpp:203`（`QTemporaryDir(QDir::tempPath() +
"/paleo-log-import-XXXXXX")`）。所有 fail 分支靠 unique_ptr 析构清理；
解析器均为 QFile 短句柄（无 mmap——dlis/lis/las/shacache 全仓 grep 零命中），
泄漏主因是 Windows 上 Defender/索引器对新落盘临时文件的瞬时占用 →
`~QTemporaryDir` 删不动 → 告警（测试日志所见即此）+ %TEMP% 垃圾累积
（无重试、无清扫——`sweepStaleStaging` 只管工程内 staging）。

判断：良性为主（不影响正确性），但大批量导入 + 杀软环境下会持续累积
孤儿临时目录。建议：快照改放 `stagingRoot` 下（随 discardStaging/sweep/
恢复链天然兜底），一石二鸟把该告警面归零。

### 本面核对为安全的点

- storeManagedRaw 的 partial 在写失败/读失败/rename 失败三分支都先
  close 后 remove（dataimport_session.cpp:154-176）；成功路径
  partial→rename→只读，无句柄跨越。
- writeImportIntent 用 QSaveFile（importrecovery.cpp:86-91），失败不留
  半截 intent；records 为空时正确地不落文件（85）。
- ImportSession 析构 `!committed && !recoveryPending` 才清暂存
  （dataimportservice.cpp:187-190）——恢复凭据与暂存同生死，方向正确。
- 附注（非本 PR 引入）：`sweepStaleStaging` 只在 `!prepared` 打开路径跑
  （dataimportservice.cpp:110-118），prepareOpen 快路径下无 intent 的
  陈旧 staging 永不清扫——慢性磁盘泄漏，建议后续顺手修。

## 攻击面 7：测试盲区（对照上述发现，按风险排序）

已有真实故障注入覆盖（不在盲区）：预览后换源（tst_import_ux.cpp:358-406）、
去重零增长/目标冲突（281-357）、取消窗口与整批丢弃（661-686）、
目标碰撞回滚（687-738）、部分移动恢复（739-822）、篡改保留证据（868-891）、
改类型不凭文件名补挂（920-963）、符号链接目标拒绝（tst_import.cpp:1293）、
孤儿不阻塞导入（1216）、staging 清扫（1253）、并发不死锁（2427）等。

| 序 | 缺失测试场景 | 对应发现 | 风险 |
|----|--------------|----------|------|
| 1 | 预览生成后向源目录/清单新增文件 → 确认导入 → 断言新文件未入库（且提示重新预览） | F1 | 高 |
| 2 | 非测井文件保 mtime+size 换内容（模拟同步工具）→ 断言不误命中 dedup/不静默跳过 | F2 | 高 |
| 3 | wellTargets 传 `new:<已有井名>` → 断言拒绝或显式合并提示；UI 层撞名禁用 | F4 | 高 |
| 4 | 先导测井建井、再导含坐标井位表 → 断言井获得坐标（或至少有可见告警/issue） | F5 | 高 |
| 5 | dedup 跳过行带 shp 族缺失成员 + 执行段取消 → 断言工程目录无新增成员 | F7 | 中 |
| 6 | recoverOne 单记录损坏（hash 不符）→ 断言工程可打开、其余 intent 仍处理、坏 intent 被隔离 | F8 | 中 |
| 7 | 超长井名（如 1MB `# Well:` 行/WELL 列/ss:Name）→ 断言解析或建井处拒绝 | F11 | 中 |
| 8 | staging 内预置 junction/symlink → 断言 discardStaging/sweep 不越根删除 | 攻击面4 | 低 |
| 9 | 「有暂存文件但 journal 为空」不变式 → 断言 commitImport 拒绝并清理 | F9 | 低 |
| 10 | QTemporaryDir 删除失败注入（持句柄）→ 断言有兜底清理或快照已移至 stagingRoot | F12 | 低 |

## 总结论

五轮审查闭环的 well_log 指纹链与恢复清单验证经本轮攻击性复核依然成立
（快照-解析-复制同源、恢复先验后删、跨根删除未证实）。本轮新增 2 个高严重度
**已证实**缺陷（F1 预览≠执行计划、F2 非测井 dedup 陈旧误命中）、2 个高严重度
数据完整性缺陷（F4 新建井撞名静默并入、F5 井坐标永不回填），及中低危若干。
建议合并前至少修复 F1/F2/F4/F5 并补测试盲区表中 1-4；F7/F8/F11 可随
下一批导入加固处理。本报告未编译、未运行测试；执行验证责任仍在作者。
