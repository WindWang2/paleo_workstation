# data-foundation 进度（wave/data-foundation）

日期：2026-09-28。方向：数据底座第二波（LAS/导入解阻、锁名副其实、腐败恢复、
原子写、schema 矩阵、幂等压力、welldist 算法核）。验证口径：ninja 全绿 +
ctest 83/83（验收清单 19 项全过，含新增 tst_ingestplan）+ check_layering 绿
（baseline 4 行未涨）。

## 完成项

### T1 LAS 解阻（服务侧）
- `LasParser::parseHeader`（io/lasparser）：header-only 入口——读到 ~A 段头
  即停，语义与全量 parse 同源（WRAP YES / 无 ~C 拒绝；~A 缺失不算失败，
  `sawAscii` 如实报）。出参 `LasHeaderInfo`（曲线名/井名/NULL 值）。
- `PreviewDocService`（additive）：静态 `lasHeaderAt` + 实例 `requestLas/
  releaseLas`（任务池异步整份解析，per-key 世代号压制陈旧结果，协作取消；
  无任务服务同步降级）+ `lasReady/lasFailed/lasCancelled` 信号。
  `lasAt` 签名未动（跨方向消费面零扰动）。
- 实测（tst_previewdoc，60k 行 × 6 曲线 ≈ 3.5MB 合成 LAS）：header-only
  **0-1ms** vs 全量 **~1160ms**——头部解析与数据行数解耦。
- **接缝（UI 方向待接线）**：`datapreviewtabs.cpp:1922`（单井曲线页）与
  `correlationpanel.cpp:546/558`（连井剖面）仍在 GUI 线程同步 `lasAt`——
  切换面 = 先 `lasHeaderAt` 铺曲线名，再 `requestLas` 补数据、结果挂
  `lasReady`。

### T2 文件夹导入解阻
- **plan 期搬出 GUI**：`IngestCatalogSource` 只读面（ingestplan.h）两个实现
  ——`LiveCatalogSource`（catalog 线程零拷贝直通）/ `CatalogReadSnapshot`
  （catalog 线程上 QVector COW 快照 O(1)，之后任意线程构建 plan）。
  `DataImportService::planFor`：快照那一跳是唯一跨线程接触；扫描/分类/
  哈希/身份匹配/去重复核全在调用线程。worker 调用（FolderImportWorkflow）
  从此不经 BlockingQueuedConnection——tst_ingestplan
  `planFromWorkerThreadNeedsNoGuiEventLoop`：GUI 事件循环不泵也能完成。
- **扫描期进度**：`IngestScanProgress`（哈希段逐文件回调 + 协作取消）。
  importFolder 的 progress 回调分两段：扫描段 total==0（跑马灯+文件名
  detail），执行段 total=行数。扫描取消 → `plan.cancelled` → 零行执行。
- **previewFolderAsync**（workflow）：任务池在场时预览扫描走 worker，
  `previewActiveChanged` 双发。
- **folderconfirm 细化**：`folderEstimateLabel` 大小估算行（human-readable +
  重复/枚举跳过计数；行大小进路径 tooltip）；「重复→跳过」行挂「仍导入」
  按钮（仅当壳给 `Hooks.importAllWithForced`，objectName `folderForceImportN`）
  → `importFolder(..., forceImportPaths, ...)` 翻判 as_new_version（同字节
  结局交内部 dedup）；`folderErrorReport` 失败行明细（≤5 行 + 余量计数）。
  既有四列结构与 folderRetry 行为零改动（tst_panels 全绿）。
- **接缝（壳侧待接线）**：`paleomainwindow_attach` 的文件夹导入入口换
  `previewFolderAsync`；确认表 Hooks 补 `importAllWithForced`（现有
  `importAll` 继续可用——不给新出口则「仍导入」按钮不出现）。

### T3 catalog.sqlite（TODOS P3 递延项：量化后维持递延）
实测（tst_catalog `thousandEntityQueryBudget`，合成 1000 井 / 2000 资产 /
3000 版本 / 4000 链接，catalog.json 2.3MB）：

| 操作 | 实测 |
|---|---|
| open()（全量 JSON 解析） | 44–79 ms |
| entities("well") 千实体扫描 | <1 ms |
| wellsMatchingName / linksForEntity / currentVersion / versionBySha256(miss) | <1 ms |
| save()（10001 行全量重写） | 85–156 ms |

结论：**触发条件未达**。读侧查询是内存线性扫（微秒级）；open 50ms 量级、
save 150ms 量级在本工区规模（≈60 文件）与 1k 实体合成规模之间无悬崖。
sqlite 可重建索引维持递延（ADR 0056 语义不变：catalog.json 唯一真相源）。
**落地触发线**（下次对账用）：open() > 500ms 或 save() > 1000ms（约 10k
版本量级），或列表查询在 UI 出现可感知卡顿（>50ms/次）。

### T4 ProjectDirLock 名副其实（isHeld 接全部写路径）
- `DataCatalog::setLockedReadOnly`（实例级，open() 不清除）：save() 恒拒
  （锁错误文案），mutator 因 save 失败回滚内存；`refusesWrites()` 扩义。
- `LayerManifest/MapVersionStore/PaleoProjectStore::setReadOnly`：upsert/
  remove / saveVersion / recordLayoutProduct / enqueueWrite / saveAll /
  commitAll（gpkg/qgz/journal 全落盘面）如实拒绝；读面照常。
- `DataImportService::importOneFile` 锁降级早拒（算 SHA/复制字节之前），
  不留 artifacts/raw 孤儿。
- AppContext（组装根）：tryLock 失败 → 四面降级 + `projectReadOnlyChanged`
  信号 + `isProjectReadOnly()` 出口；只读实例跳过 manifest rehydrate 与
  wells.geojson 重写（可复用产物直接实例化）；QgsMessageLog 用户可见告警。
- 实测（tst_metastore）：双开第二实例拒 + 错误带持有者（pid@host, app）；
  **残留锁**——手写「持有者已退出」锁文件（QLockFile 三行格式：
  `pid\n应用名\n主机名`，注意 line2=appname）→ tryLock 自动回收；
  解锁后重取。tst_catalog/tst_import 覆盖 catalog/导入侧拒绝面。
- **接缝（壳侧）**：主窗标题「（只读）」/保存动作禁用可绑
  `AppContext::isProjectReadOnly`（paleomainwindow 侧）。

### T5 catalog 腐败恢复（.bak 回退）
- open() 主文件解析失败且 .bak 可用 → 从 .bak 装载、`recoveredFromBackup()`
  为真、`backupRecovered(reason)` 信号（DataImportService → PreviewDocService
  转发，`catalogRecoveredFromBackup`）——UI 状态面据此告警。
- **只对解析失败生效**：schema 不匹配（未来版本）不走回退——.bak 与主文件
  同代，回退等于静默降级数据（tst_catalog 断言拒开）。
- .bak 语义（实测演练）：恢复点 = 最后一次成功 save 的**上一代**——最近一轮
  增量如实丢失；恢复后续存把恢复内容轮转成新 .bak，链可持续（连续两轮
  损坏→恢复演练过）。主/bak 都坏 → 拒写态 + 双文件点名。

### T6 原子写全覆盖审计
grep `QIODevice::WriteOnly` 全量过堂，状态：
- **已修（本波）**：`datacatalog.cpp writeWellsGeoJson`、`io/geojsonaffine.cpp`
  → QSaveFile（setDirectWriteFallback(false)）。写失败不截断旧文件
  （tst_catalog：目录只读 → 旧文件字节原样）。
- **既有合规**：catalog save / commit journal / project file / qgisprojectservice
  （QSaveFile 或 partial+paleoReplaceFile 模式）。
- **显式豁免**：`crashreport.cpp:388/392` 活性旗标（半截旗标语义仍是
  「曾运行」，恰是脏退出检测要的）；`dataimportservice.cpp` 受管 RAW 的
  `.partial`+rename（原子模式本身）。
- **非本方向裸点（接缝表，owner 见下）**：`workflow/mappingworkflow.cpp:402`
  井点 GeoJSON（datacatalog 同函数的 QSaveFile 修法可直接照抄，编图方向）；
  `ui/pages/datalist.cpp:209`（linkage notes，best-effort 日志语义）；
  `ui/layers/layerpropertiesdialog.cpp:336`、`ui/layers/layertreepanel.cpp:386`
  （style .qml 导出，用户可见产物 best-effort）。

### T7 schema 迁移矩阵扩全（tst_metastore）
- user_version ∈ {0(遗留/absent), 1(当前), 2, 99} × {LayerManifest,
  MapVersionStore, ReleaseStore}：0→采纳并保持可写（写后重开幂等、数据
  在）；1→原样通过；>1→拒开 + **零表创建**（sqlite_master 断言）。
- **挖出并修掉一个真洞**：三个 store 的 `ensureOpen` 原来只在首次 open 时
  查 user_version——拒开后连接留在 QtSql 注册表处于 open 态，之后的读调用
  （all()/latest()）经缓存连接**绕过版本门建表**。修法：ensureUserVersion
  移到连接确保之后每次执行（一次 PRAGMA，幂等便宜）。
- .bak 链恢复演练见 T5。

### T8 staleness 补漏（sha 复验入口审计）
失配入口清单与处置：
- previewdoc `verifyExternalSha`（既有，sha 失配 → markDownstreamStale）；
- `requestSection` worker（既有，previewdoc.cpp:505 区域）；
- **新增闸**：`ensureDocumentPdf`（外链 document RAW 转换前复验；失配 →
  documentPdfFailed + 下游标过时）；
- **新增闸**：`seismicTieMarker` 的分层/时深读取（外链版本先过
  verifyExternalSha，失配即不作标定依据）。verifyExternalSha 转 const
  （m_shaVerified mutable）以供 const 读路径。
- **接缝（UI 侧）**：datapreviewtabs buildWellBody 的静态解析
  （wellTopsAt/timeDepthAt/wellHeadsAt 路径不经复验）——外链井文件被改
  的场景建议经门面的 verify 面包一层（UI 方向）。

### T9 别名下线
`DataImportService::FolderRowResult/FolderPreviewRow` using 别名删除；全部
调用点直呼 `domain/importrows.h` 裸类型（io 两文件 10 处 + tst_import 22 处 +
tst_perfbudget 1 处 + tst_panels 接缝 2 处）。

### T10 ingestplan 幂等压力（tst_ingestplan 新建）
- 重复执行：第二/三跑全行 Skipped，资产/版本/链接零增量，revision 不涨；
- 中断续跑：执行段取消保留已处理行 → 重跑收敛到与一次跑完**完全相同**的
  目录状态（资产/版本/链接/井数逐项对账）；扫描段取消零行执行；
- 部分失败：单文件权限收回 → 行 Failed 不传染 → 修复重跑补齐；
- 快照等价：活对象 plan 与快照 plan 逐项一致。

### T11 welldist 算法核
- `PaleoWellDistanceAlgorithm`（paleo:paleo_welldist，src/algorithms/
  welldist.cpp + PaleoProvider 注册）：输出栅格每格 = 格中心到最近井点的
  **精确**欧氏距离（INPUT CRS 单位；O(rows·cols·points) 直算，非 cell-snake
  近似；网格 = 输入范围外扩 10%，同 ConstraintIDW 约定）。参数面
  INPUT/CELL_SIZE/OUTPUT（无 FIELD——距离不用属性）。
- harness：12×2 格对解析期望 hypot 逐格对拍（tol 1e-4）+ 确定性重跑 +
  空输入拒绝。
- **契约状态：草案**（docs/progress/mapping.md 不存在，无登记契约）。
  接缝：`singlefactordef.cpp`（编图方向）welldist 条目翻转 processingAlgId
  （现仍 paleo:paleo_constraint_idw + 标注「距离变换待接入」）+
  `algoparamschema.cpp` 补参数面。**confidence 引擎维持 blocked**：全算法
  无置信度输出面（ONNX 只读首张量、paleo:* 确定性单输出——与 TODOS
  「置信度伴生栅格」递延同因），不造假数据。

## 顺手修掉的既有缺陷
- **tst_folderimport / tst_previewdoc 空转**：缺 `private slots:`，两套件
  在 master 上只跑 init/cleanup 的假绿（folderimport 7 用例、previewdoc
  7 用例从未执行）。补声明后如实执行；folderimport 的
  importFolderRowForceType 原断言错误（无井时 TD 必然 Unresolved）——按
  两阶段语义修正（先导井口再断 Imported）。
- metastore 缓存连接绕过 user_version 门（见 T7，三 store 修复）。

## 验证
- `ninja` 全绿（447 target）；ctest 83/83（offscreen）；验收清单 19 项：
  tst_import tst_catalog tst_metastore tst_entityview tst_roles
  tst_commitcoord tst_staleness tst_relocate tst_segy_fixture
  tst_projectparsers tst_horizonbinner tst_previewdoc tst_projectdata
  tst_perfbudget tst_algorithm_harness tst_folderimport tst_services2
  tst_panels + tst_ingestplan（新增）。
- `python3 tools/check_layering.py` 绿；baseline 4 行未涨；新文件
  （src/algorithms/welldist.cpp）头三行 `// 层：数据`。

## 遗留
- 接缝表（各条内嵌在上文）：UI lasAt 消费点切换 / folderconfirm 壳接线 /
  welldist 翻转 / mappingworkflow 井点 GeoJSON 裸写 / UI style 导出裸写 /
  isProjectReadOnly 主窗态 / buildWellBody 静态解析复验。
- catalog.sqlite：递延维持，触发线见 T3。
- 大 LAS 的 GUI 卡顿根因已移除（header 0-1ms）；UI 侧切换后「UI 线程延迟
  悬崖」TODOS 条目可关闭。
