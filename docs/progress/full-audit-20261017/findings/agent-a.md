# Agent A（数据层）深度审计报告 — full-audit-20261017

审查对象：/home/kevin/projects/paleo_workstation-full-review-20261017（HEAD=c720aef5）
范围：src/domain/ src/catalog/ src/io/ src/metadata/（222 文件 / ~48k 行）
方式：只读静态审计，逐行读高风险函数 + 追踪真实调用链；未构建、未运行动态验证。
去重基线：COMPREHENSIVE_AUDIT_2026-10-16、SWARM_REVIEW_2026-10-15、排除矩阵
（#460–#504 中与本域相关项）、#476/#477 issue 正文、codex/audit-remediation 改动文件清单。

新发现计数：P1×2，P2×2，P3×4（另 1 条 P3 记为既有 #496 设计注记）。

---

## Findings

### A-01 — P1 / VERIFIED（代码链路完整推演；未动态复现）
- Module: src/io（constraintstore）
- File:Line: src/io/constraintstore.cpp:710-718, 813-857（`ConstraintStore::migrateLegacyCrsLocked` 的 `restoreBackup` 路径），#488 b2ed2da4 新增
- Root Cause: 提交后校验失败时的字节级还原 `restoreBackup()` 只做
  `QFile::remove(m_gpkgPath)` + `QFile::copy(backup, m_gpkgPath)`：
  1) **不删除旁边的 `-wal`/`-shm` 文件**。约束 GPKG 处于 WAL 模式（迁移自身
     line 681 的 `PRAGMA wal_checkpoint(TRUNCATE)` 与注释「V3 WAL 盲区」即为此
     而设；catalogstore_internal.h:123 也确认本仓 SQLite 家族用 WAL）。迁移事务
     的提交内容落在 `-wal` 里；还原只换主文件后，SQLite 下次打开会对着
     「还原后的旧主文件」重放遗留 WAL 的已提交帧（WAL 恢复只校验帧校验和链，
     不校验与主文件的同一性）→ 产出「备份页 + 迁移后页」的嵌合库：迁移
     部分复活/页级错配，constraint 行与 gpkg_spatial_ref_sys 可互相矛盾。
  2) **还原时本连接仍打开**：`Transaction transaction`（line 678）的作用域到
     函数结尾（line 861），`commit`（813）后并未 GDALClose；restoreBackup 在
     822/854 执行时该连接还持有旧（已迁移）数据库。Linux 上 remove 成功但句柄
     指向已 unlink 的 inode，连接关闭时的 checkpoint 写进孤儿 inode（无害但
     WAL 文件仍留盘，加剧 1）；Windows 上 remove 直接因共享冲突失败——「还原」
     在 Windows 上必然走 `restoreError` 分支（与 #499 同族的句柄未释放面）。
- Trigger: 用户点「为约束库登记本地网格坐标系…」（#496 接入的
  migrateConstraintLayerCrs）→ 迁移提交成功 → 提交后逐行校验失败（如
  GEOS/GDAL 层面任何一行 WKT 往返不一致、或 reopen 只读校验失败）→
  restoreBackup。这正是该函数设计要保护的数据安全路径。
- Observed vs Expected: 期望「从备份原样还原原文件」；实际 Linux 下产生
  嵌合库或迁移复活（静默），Windows 下还原恒失败并误报「原文件可能缺失」。
- Impact: 约束库（唯一写者资产）损坏/状态错乱；审计事件（action=
  crs_migration，before/after）与实际盘上状态脱节。
- Existing Match: 部分——Windows 句柄半边与 #499（GPKG 删除失败）同族；
  `-wal/-shm` 遗留重放与「迁移还原窗内并发提交覆盖」均为 NEW 根因。未列入任何排除项。
- Fix Strategy: restoreBackup 在 remove 主文件的同时 remove
  `<path>-wal`/`<path>-shm`（或改用 Sqlite `VACUUM INTO`/备份 API）；还原前
  先 GDALClose 掉 transaction.ds（把 Transaction 析构/显式 close 提到还原之前）；
  还原成功后按 pre 快照再验一遍。
- Regression Test: 测试注入使提交后校验失败（现有 m_renameProbe 风格钩子或
  直接喂一行 WktOptions 往返不稳定的几何），断言：还原后重开库逐行 == pre
  快照、且盘上不存在遗留 `-wal`；Windows 侧断言 remove 失败时错误信息不谎称
  「原文件可能缺失」。

### A-02 — P2 / VERIFIED
- Module: src/io（constraintstore）
- File:Line: src/io/constraintstore.cpp:681 `sql(transaction.ds, "PRAGMA wal_checkpoint(TRUNCATE)")` 返回值被丢弃
- Root Cause: 迁移的「字节级备份」以 checkpoint 成功为前提（注释明说
  「备份前先把已提交数据 checkpoint 进主文件，字节级备份才完整」），但
  checkpoint 失败（另一连接持有读锁时 SQLITE_BUSY 是常态——QGIS 画布上正开着
  constraints 图层）时返回 false 被静默忽略，随后照常 `QFile::copy` 主文件。
  备份于是缺 WAL 中已提交的迁移前数据；line 707-709 的尺寸比对只验证拷贝
  完整性（backup.size == 主文件.size），检不出「主文件本身缺 WAL 内容」。
  若后续提交后校验失败走 restoreBackup → 迁移前最近提交被无声清掉。
- 对照：同仓 catalogstore.cpp:463-473 对同一操作做了正确处理——
  `ckptOk` 不真则「跳过备份拷贝」并告警。constraintstore 没套用同款纪律。
- Trigger: 迁移时有别的连接（QGIS provider / 另一进程）对 GPKG 保持读快照
  → checkpoint BUSY → 校验失败路径触发还原。
- Impact: 还原后丢最近已提交约束（静默数据丢失）。
- Existing Match: NEW（#477 只覆盖 constraintstore 的「活动事务内 DDL」一条）。
- Fix Strategy: 参照 catalogstore：`ckptOk` 为假时 fail-fast（报「无法独占
  检查点，请关闭其他打开该库的窗口后重试」），绝不带着不完整备份继续。
- Regression Test: 用第二个连接持有读事务后调 migrateLegacyCrs，断言迁移
  被拒绝且未产生 .crs-backup-*。

### A-03 — P1 / VERIFIED（调用链与索引时序完整推演；未动态复现）
- Module: src/catalog（datacatalog 版本生命周期/血缘）
- File:Line: src/catalog/datacatalog_versions.cpp:96-109（`addVersion` supersede
  级联）× src/catalog/datacatalog_derivation.cpp:46-74（`markStaleDownstreamOf`）
  × src/catalog/catalogindex.cpp:77-89（`versionAdded` 先于级联注册 parent 边）
- Root Cause: addVersion 先 `m_versions.append(v)` + `versionAdded`（把
  v.parentVersionIds 的边注册进 `childrenByParent`），再对同资产所有
  versionNumber 更低的旧版本跑 `markStaleDownstreamOf(pid)`；闭包按
  parentVersionIds 反查，**新加入的 v 自己就是 pid 的孩子**。当 v.stage ==
  "DERIVED" 且其父版本在同资产且 versionNumber 更低时，v 被自己触发的级联
  打上 `extra["stale"]=true`、`staleReason="上游版本 X 已被同资产新版本 v
  取代"`——自指的荒谬标记，且随同一事务落盘。
- 两个已确认生产触发点：
  1) `src/workflow/registration.cpp:115-129`（临时配准派生）：`d.stage="DERIVED"`、
     `d.assetId = src.assetId`（同资产）、`d.versionNumber = src.versionNumber+1`、
     `parentVersionIds={src.id}` → **每个临时配准版本出生即 stale**。
  2) `src/workflow/assetops.cpp:184-195`（版本回滚）：`nv.stage = target.stage`、
     同资产、`newNumber = current+1`、`parentVersionIds={target.id}`。代码在
     addVersion 前显式 `nv.extra.remove("stale"/"staleReason")`，注释明说
     「带着『已被取代』标记会让 removeStaleVersions 视当前版本可清」——但该
     remove 发生在级联**之前**，级联随后把标记原样加回。对 DERIVED 资产的
     回滚，防御被自身击败。
- Observed vs Expected: 新派生/回滚版本是刚从当前源算出的**当前版本**，
  不应带 stale；实际出生即 stale 并持久化。
- Impact: entitypanel.cpp:1210/1513、datapreviewtabs.cpp:505 显示「已失效」
  徽标；mapversioncontroller.cpp:481（stale DERIVED 计数治理提示）、
  cataloghealth.cpp:57、storagegovernance.cpp:119、derivationgraph.cpp:111
  （stale 种子）全部误报；`removeStaleVersions`（UI 治理删除入口）把当前
  版本列为可删——用户删除后画布上的临时配准图层指向已删文件（断引用）。
- Existing Match: NEW（#476 的 currentVersion tie-break/唯一约束是另一问题；
  tst_welltopsedit.cpp:453 只钉了非 DERIVED 编辑版本不 stale，本路径无测试）。
- Fix Strategy: addVersion 的 supersede 级联排除 v.id 自身（闭包种子传入
  `excludeId`，或级联前先记 v.id、markStaleDownstreamOf 跳过）；或规定
  「新版本自身的 parent 不参与本次 supersede 源集合」。同步给
  registration/assetops 补断言。
- Regression Test: 新用例：同一资产 RAW v1 → addVersion(DERIVED v2,
  parents={v1}, versionNumber=2) → 断言 v2.extra 无 "stale"；assetops 对
  DERIVED 目标回滚后断言当前版本无 stale 且 mapversioncontroller 计数为 0。

### A-04 — P2 / HIGH
- Module: src/io（geojsonaffine）× src/workflow（registration 调用点）
- File:Line: src/io/geojsonaffine.cpp:120-140 `geoAffineTransformFile`（`in.readAll()`
  + 全量 QJsonDocument DOM，无大小闸）；生产调用 src/workflow/registration.cpp:107
  （`geoJsonBounds` :196-205 同款无闸，services/previewdoc.cpp:336 直通）
- Root Cause: LAS 家族有 500MB 闸（LasParser::fileSizeLimit，dataimport_file.cpp:194
  / dataimport_geodata.cpp:175 都套用了），井文本 #387 也补了闸；GeoJSON 仿射
  配准路径没有：用户导入的大幅面 GeoJSON（矢量导入走 OGR 流式不受限，RAW 副本
  可为任意大小）在临时配准时整读进内存并建全量 DOM（DOM 常为文件 3-10 倍内存）。
- Trigger: 对大体量 GeoJSON 资产执行「临时配准」。
- Impact: GUI 线程长冻结 + OOM（与 #466/#467/#469 同类攻击面但为不同格式与
  路径，未被任何既有 issue 覆盖）。
- Existing Match: NEW。
- Fix Strategy: 入口加 `LasParser::fileSizeLimit()` 同口径闸（或流式
  SAX 变换）；`geoJsonBounds` 生产面统一走已有的
  Streaming::geoJsonBoundsStreaming（previewdoc.cpp:354 已示范）。
- Regression Test: 造 >limit 的 GeoJSON 调 registration，断言如实报
  「过大」且不建派生目录/不注册版本。

### A-05 — P3 / SUSPECTED
- Module: src/io（constraintstore）
- File:Line: src/io/constraintstore.cpp:514-529（`copyAudit` 分页循环）
- Root Cause: `historyForHorizon(horizon, 200, offset)` 按 `ORDER BY fid DESC
  LIMIT/OFFSET` 分页读取源库；分页期间源库并发追加审计事件会使 OFFSET 位移
  （事件被跳过；重复则被 event_id 去重挡住，跳过无兜底）。冻结快照拷贝与
  用户编辑并发时可能少拷事件。
- Trigger: 快照 copyAudit 进行中，同一层位发生新的约束编辑并提交。
- Impact: 发布快照审计面不完整（静默少事件）。
- Existing Match: NEW（#477 未含此条）。
- Fix Strategy: 改键集分页（`WHERE fid < lastSeen ORDER BY fid DESC`），或
  分页前在源库取一个只读事务快照。
- Regression Test: 后台线程边写审计边 copyAudit，断言事件数一致。

### A-06 — P3 / SUSPECTED（可达性未证实）
- Module: src/io（lascache）
- File:Line: src/io/lascache.cpp:32-56 vs 85-99（encodeDoc/decodeDoc）
- Root Cause: 头部 `rowCount = max(各曲线长度)`，encode 按各曲线**实际长度**
  写字节，decode 统一按 `rowCount×8` 推进并 `vals.resize(rowCount)`。若任一
  曲线短于最长曲线：短曲线尾部被零填充（0.0 是合法物性值，伪造数据），
  且 pos 越过实际写入量，**后续所有曲线整体错位读**（指纹校验仍通过——
  静默全文档污染）。
- 可达性核查：lasparser 各解析路径逐行对所有列 append（缺列补 NaN，
  lasparser.cpp:284-295、:692-706、:907、:1025），等长成立时不可达；
  ragged 只能来自未来解析器改动或外部写缓存。属潜伏地雷而非现行 bug。
- Existing Match: NEW。
- Fix Strategy: encode 时断言/补齐各曲线等长（短的补 NaN 而非零）；或头部
  记录每曲线各自长度。
- Regression Test: 直接构造 ragged LasDoc 走 encode→decode，断言往返逐值相等。

### A-07 — P3 / VERIFIED（行为确认；定性为设计取舍）
- Module: src/domain（constraintsemantics）
- File:Line: src/domain/constraintsemantics.cpp:654-663（structural_idw 停线
  域外拒绝，#496 a1efbd74 新增）vs src/algorithms/singlefactor/structural.cpp:409-431
- Root Cause: 引擎侧停线判据是 `GEOSDistance(line, domain) > softRadiusAuto`
  才跳过（软半径可达即消费）；计划侧用「不触域（!inDomain）即拒
  outside_compute_domain」。落在域外但距域 ≤ softRadius 的停线：引擎消费、
  计划拒绝——receipts::compareWithPlan 会报「计划不消费，实际被消费」差异。
  代码注释自认「计划用触域作保守下界」，是显式取舍而非疏漏。
- Impact: 计划/回执一致性告警在该几何带内常态性误报（仅提示面，无数值影响）。
- Existing Match: #496（设计注记；非缺陷修复项）。
- Fix Strategy: 若要消除误报，计划侧同样用「与域 bbox 距离 ≤ softRadius」
  判据（softRadius 在计划期可用 barrierShapeRadius 近似）。
- Regression Test: 域外 softRadius 内停线一条，断言计划与引擎回执同判。

### A-08 — P3 / VERIFIED（轻微，接口契约面）
- Module: src/domain（receiptsemantics）
- File:Line: src/domain/receiptsemantics.cpp:406-411（`consumedIds.insert(rowIdentity(...))`
  不查空——空串会入集并在 :526-532 生成 id 为空的「计划未登记」伪差异；
  上游 normalize 家族多数已滤空，composed 版本 version_id 为空的行可触发）；
  :308-319（normalizePrediction 把 `missing=true` 的输入同样放进
  accepted_inputs，与头注「accepted=实际冻结并成功打开」语义相悖）。
- Impact: 证据面板偶发空 id 差异行、prediction 的 accepted 面虚大。
- Existing Match: NEW。
- Fix Strategy: consumedIds 收集时跳过空 id；prediction accepted 过滤
  `missing==true` 行（保留在 declared）。
- Regression Test: 构造 missing=true 输入的 evidence 走 normalize，断言其
  不在 accepted_inputs。

### 记录不报（核对后判定非缺陷/已有覆盖）
- frameworkstore.cpp:259-265 批次 flush 失败不删刚写文件——孤儿目录由
  #155 `scanManagedSeqFloor` 容忍（datacatalog.cpp:36-65），口径自洽。
- constraintsemantics.cpp segmentTouchesRect（a1efbd74 主体）：闭矩形契约、
  退化单点段、NaN 上游 geometryValid 闸均正确；interpretive_boundary 在
  structural_idw 无域闸与引擎 explicitSoftBoundaries 无距离闸（structural.cpp:433-440）
  两侧对称一致，非缺陷。
- updateVersionExtra 批量重载（datacatalog_versions.cpp:273-274）比单键重载
  少一行 `m_versions.at(row).id == versionId` 复核——索引一致时不可达，P4 级
  防御不对称，不计。
- paleoprojectfile.cpp / projectlock.cpp / mapsnapshot.cpp /
  mapversionstore 发布-journal-恢复三段（commitPublicationPlacement /
  recoverInterruptedPublications）逐行审过：路径闸（pathInsideProject、
  .staging- 前缀、sha 比对、幂等 complete）与失败清理自洽，未发现新问题。
- releasestore #491 增量（manifestCorrupt/diff 拒损坏载荷）：正确；idA==idB
  时 found<2 走显式错误分支，无越界。
- welllogfacies（#486）/timedepthmodel（#490）/predictionevidence（#493）/
  wellcompositemodel/wellsection（#490）增量 diff 复核：与注释口径一致。

---

## 覆盖清单

深度（逐行）：
- src/domain/constraintsemantics.cpp（860 行，#488+#496 全部增量含 segmentTouchesRect/
  decideByMethod 各方法分支；对照 src/algorithms/singlefactor/structural.cpp:355-455 引擎侧）
- src/domain/receiptsemantics.cpp（541 行，#494 新文件）、src/domain/evidenceconflict.cpp（403 行，#494 新文件）
- src/io/constraintstore.cpp（947 行，#488 新增 inspectCrsStorage/migrateLegacyCrs 逐行 + append/
  load/remove/updateParameters/replaceHorizon/copyAudit/journal/Transaction）
- src/metadata/paleoprojectfile.cpp（410）、projectlock.cpp（127）、mapsnapshot.cpp（128）、
  mapversionstore.cpp（publish/commitPublicationPlacement/recoverInterruptedPublications 段 ~350）、
  layermanifest.cpp（258）
- src/catalog/datacatalog.cpp（768，生命周期/线程纪律/staging/batch/commitStore）、
  datacatalog_versions.cpp（469）、datacatalog_derivation.cpp（112）、
  catalogindex.cpp（208）、frameworkstore.cpp（271）、purgelease.cpp（22）、
  catalogstore.cpp（openExisting/recover/backup-rotate/migrate 段 ~200）
- src/io/geojsonaffine.cpp（231）、wellfileparsers.cpp（398）、lascache.cpp（编码/解码段 100）、
  sfpkgwriter.cpp（appendNumeric 120）

diff 级复核（git show）：a1efbd74(domain)、ddd1325e(#494 新文件全文另读)、
66e79e61、e83b73d7、91fd9432、84df0e59、b2ed2da4（constraintstore 并入全文审）。

接口/模式扫描（grep+抽样读，未逐行）：datacatalog_assets/entities/links/export、
catalogstore_crud/json/schema、catalogstore_internal.h、metastore、releasestore、
layouttemplatestore、faultsetstore、wellsitingstore、wellsectionstore、realizationset、
entityview、catalogroles、roleregistry、entityview；io 侧 horizonbinner（头部+索引面）、
segyindexstore、shacache、cachebudget、attrgridout、cuttingsdoc、streaming、partialread、
timedeptool、outsourceworkbook、ingestplan、dataimport_file/geodata（大小闸面）、
dataimport_folder/session/document/wells/pyramids、dataimportservice、encodingdetect、
lasparser（仅 ragged 曲线核查）、dlisparser/lisparser/sfpkgreader/segyreader/ziparchive/
lasalias/laswriter/wellcompositexml/cachecore/rasterpyramid（既有 issue 覆盖面，只做排除核对）。

测试对应性核查：tst_welltopsedit.cpp:453（非 DERIVED 编辑版本不 stale）、
tst_perf_catalog.cpp:440-530（supersede 回滚面）、tst_catalog*、tst_import_geoaffine、
tst_singlefactor_constraintreceipts（#496 域契约测试存在且与引擎侧对称）——
A-03 场景（同资产 parent 的 DERIVED 新版本）无任何测试钉住。
