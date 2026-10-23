# 03-review-C — workflow 增量深审

审查窗：`d699694c..HEAD`（#630–#647）。只读，未构建、未跑测试。
范围：`constraintwellfactors.cpp`、`evidenceinteraction.*`、`folderimport.*`、
`mappingevidencereview.cpp`、`mappingworkbench.h`、`offlinebasemapimport.*`、
`paleomapimport.cpp`、`paleomaplibrary.h`、`projectsetup.*`、
`projectsetupmaps.cpp`、`stratigraphicwebsession.cpp`。

## Findings

### C-1 / P2 / HIGH / workflow

- File: `src/workflow/constraintwellfactors.cpp:229`（`sourceFor`）；对照 `src/io/wellfactorfile.h:6`、`src/io/wellfactorfile.cpp:46`
- 函数：`sourceFor`（`ConstraintWorkflow::extractWellFactorsImpl` 的 worker 计算段调用）
- 根因：导入的井层单因素行只要存在，就把 `maintainedThickness` / `maintainedSand` 置真。同函数对井点属性表的注释写明「行存在 ≠ 维护过数值」，空行按存在性判断会永久挡住分层/岩性回算（D61）。新分支把这个已修过的口径又用在导入行上。`wellfactorfile` 的契约是缺测保持空值、不拒绝整文件。
- 触发条件：`*_单因素.csv` / `*_factors.csv` 里某井某层有记录，但 `sand_thick` 或 `log_layer_thickness_md` 单元格为空（解析器不写入该键）；该井在工程里仍有顶底分层 MD 和/或岩性段。用户走「推荐地质口径（自动）」提取砂厚、砂地比或地层厚度。
- Observed：`maintained*` 为真后，`base - top` 与 `sandIn` 都不会写入属性（`:273`、`:279`）。随后若仍无数值，原因被改成导入行的 `quality`（可为空字符串，`:292`）。该井不参与插值。即便导入行其实带了 `log_layer_thickness_md`，`sourceOfField`（`:397`）仍把层厚来源标成「井点单因素属性表」，而不是导入文件的 `source`。
- Expected：缺测保持空，回落到工程分层 MD / 解释岩性，与属性表「维护 = 数值可用」同一口径；有导入数值时来源应指向该文件，而不是属性表。
- 调用链：约束页「提取并保存井点数据」→ `extractWellFactorsRequested(factorId, horizon, wellFactorParams)` → `extractWellFactorsImpl` 在 `PaleoTask` worker 上 `decided.insert("factorId")` 后调用 `sourceFor` → `ProjectDataFacade::importedWellFactors` 读版本 `extra.wellFactors`（`dataimport_file.cpp` 在 `readWellFactorFile` 成功后写入）→ `computeExtraction` 因 `maintained*` 不再取分层/岩性。
- 排除比对：不是 #593 空棒 0.0（这里是缺值被排除，不是写成 0）。不是 #556（失败经 `*error` 返回，不靠 `qWarning`）。不是 #645 已列症状；若 `tst_wellattributes` 的断言依赖「缺测仍从分层回算」，可作 #645 的一条根因线索，但本条本身是产品口径错误，不并入 #645。
- 修复策略：`maintainedThickness` / `maintainedSand` 只在导入行或属性表对应字段 `parseNumeric` 成功时为真。`quality` 只附加到已采用的导入数值，不要在回算之前覆盖 `thicknessReason` / `sandReason`。层厚来源与砂厚一样区分「导入单因素文件」和「井点属性表」。
- 回归测试思路：造一份必需列齐全、但目标井 `sand_thick` 与 `log_layer_thickness_md` 为空、`quality` 为「未测」的 CSV；同井有可计算的顶底 MD 和砂岩段。自动提取砂厚/砂地比/地层厚度应得到分层与岩性值，而不是空值加 quality。另测两列都有数值时不得被属性表空行盖掉，且来源字符串含导入文件而不是「井点单因素属性表」。

### C-2 / P2 / HIGH / workflow

- File: `src/workflow/projectsetup.cpp:130`、`:143`；`src/workflow/projectsetupmaps.cpp:41`
- 函数：`ProjectSetupWorkflow::applySources`、`importMaps`
- 根因：文件导入的 `CancelImport` 被丢掉。回调不看取消，只要会话还在就调用 `importMaps`。`success` 只影响最终 `finished` 的布尔值，不阻止下一阶段。地图任务取消只停止后续层位组，不撤回已提交的组和已写入的工程设置。
- 触发条件：新建或补充工程同时有资料文件和勾选的古地理图。任务中心取消「导入所选文件」（`runImportJob` 认 `task->cancelRequested()`，`commitImport` 见 `session.cancelled` 后 `failSession`，整批不入库，`s.error` 为「已取消，本批数据未入库」）。回调仍进入 `importMaps`。
- Observed：文件批次回滚了，但古地理图导入照常开始并按组 `applyJournal`。工程目录、`project.paleo` 的 `sourceStats` / `georeference` 在导入开始前已经写盘（`projectsetup.cpp:117`–`:124`），取消不恢复。最终 `finished(..., false)`，用户看到失败，图件却已在库。
- Expected：取消这一轮引导导入应停止后续阶段；至少不应在文件批被用户取消后再开图件任务。已创建工程是否删除可以另议，但不得把「已取消」变成下一阶段的启动条件。
- 调用链：`PaleoMainWindow::runNewProject` → `create` / `applySources` → `QTimer::singleShot` → `FolderImportWorkflow::importFiles` → `makeImportJob` → worker `produceFiles` → GUI `commitImport`（取消则 `failSession`）→ `job->finish` → `projectsetup.cpp:130` 回调 → `importMaps` → `PaleoMapLibrary::importMaps`。
- 排除比对：文件批本身的取消语义是对的（`dataimport_session.cpp:426`），不是 #588 孤儿登记的同一条写入。不是 #556。与 #638 的退出崩溃不是同一条链。补充导入幂等见排除疑点，本条不是双登记。
- 修复策略：保存 `CancelImport`，并在工程关闭 / 工作流析构时调用。回调若 `error` 含取消或行结局为取消，发 `finished(..., false)` 后返回，不要 `importMaps`。地图组取消同理，不要把已取消当成「继续下一组」之外的唯一停止点却又自动开了下一阶段。
- 回归测试思路：无任务服务的同步路径之外，用 `PaleoTaskService` 建工程，预览含一个井文件和一个古地理图。`importFiles` 返回的取消函数在 produce 完成前调用。断言 catalog 无新井资产、无 `paleogeographic_map`，且 `finished` 的 success 为 false。再测只取消地图任务时，已入库的文件批可以保留，但不得再开下一层位组。

### C-3 / P2 / HIGH / workflow

- File: `src/workflow/projectsetup.cpp:57`；`src/qgis/qgisprojectservice.cpp:312`（调用方依赖的旧语义，本增量新暴露）
- 函数：`ProjectSetupWorkflow::create` → `QgisProjectService::createProject` → `applySources`
- 根因：`create()` 在坐标预检通过后先 `mkpath` + `createProject`，再 `applySources`。`createProject` 在 `notifyAboutToClose` 并写出 `.qgz` 之后，`project.paleo` 写失败只进 `lastErrors`，仍然 `return true`（`:310`–`:328`）。`applySources` 接着 `readProjectFile`，读失败就返回 false。没有关闭新工程、没有恢复上一工程、没有删目录。
- 触发条件：引导对话框已接受，`maybeSaveProject` 已过，目标路径预检通过，但 `project.paleo` 写失败（磁盘满、权限、目录在 `mkpath` 之后变为不可写）。UI 直接调用 `create()` 且预览里有问题图件时也会在 `createProject` 成功后被 `applySources:85` 拒绝——生产对话框在 `finish()` 挡了图件问题，这条不是主路径。
- Observed：`create()` 返回 false，主窗口弹出「新建工程」警告并 `deleteLater` 工作流（`paleomainwindow_projectsetup.cpp:26`）。此时上一工程已经 `notifyAboutToClose`，当前会话已是新工程，目录和 `.qgz` 已在。用户再试同一路径会得到「该位置已有工程」。`invalidCoordinatesDoNotCreateEmptyProject` 只覆盖了创建前的 CRS 拒绝。
- Expected：与该测试同一契约：创建失败不得留下空目录、不得切换离开上一工程。`project.paleo` 写失败应让 `createProject` 失败并 `failAfterClose`，或 `create()` 在 `applySources` 失败时删除刚创建的工程并重新打开上一路径。
- 调用链：`runNewProject` → `ProjectSetupDialog::accept` → `maybeSaveProject` → `ProjectSetupWorkflow::create` → `QgisProjectService::createProject` → `applySources` 读清单失败 → 主窗口只警告。
- 排除比对：不是 #587「开库先写后验」的 catalog 路径；这里是工程双件（`.qgz` + `project.paleo`）的新引导编排。不是 #595 析构不断信号。
- 修复策略：`create()` 把图件问题检查前移到 `createProject` 之前。`createProject` 写清单失败应失败返回并走已有的 `failAfterClose`。`applySources` 失败时若本函数刚刚创建了工程，删除该目录并恢复 `previous` 路径（调用前保存）。
- 回归测试思路：在 `createProject` 写出 `.qgz` 之后让 `writeProjectFile` 失败（或注入只读目录）。断言 `create()` 为 false、上一工程路径仍是 `project.projectPath()`、目标目录不存在或至少当前会话未切过去。

## 排除疑点

1. **重复导入双登记（projectsetup / paleomap）**  
   排除。`produceFiles` 对同一路径 + sha256 走「已在库，幂等跳过」（`dataimport_folder.cpp:617`）。`PaleoMapImport::prepare` 按 `sourceUri + horizon` 命中已有资产，指纹与 `normalization_policy` 一致则 `unchanged`。`tst_projectsetup.cpp:364` 对同一预览再 `applySources` 断言资产数不变。路径字符串若第一次和第二次不一致（符号链接 / 非规范路径）理论上会新建资产，扫描与导入都用 `absoluteFilePath`，本增量没有第二条规范化口径，未见实际双写链。

2. **扫描失败留下半成品**  
   排除。`scan` 只跑 `scanProjectSources`，取消只往预览 `errors` 写「已取消扫描」并返回，不写工程。析构只把 `m_stop` 置真。`QFutureWatcher` 是子对象，销毁时 Qt 断开 `finished`，不会在半析构对象上发预览。

3. **导入失败被当成成功**  
   排除（与 C-2 的「取消后仍继续」不同）。文件批失败时 `error` 非空或行结局为 Failed，`finished` 的 success 为 false。未决行计入摘要，不把 success 抬成 true。空路径走 `importMaps`，无图时 `writeProject` 失败会把 success 收成 false。

4. **会话切换时 `finished` 丢失**  
   不编号。`projectsetup.cpp:131` 与 `projectsetupmaps.cpp:17`、`:38` 在 `sessionId` 变化时直接 return，不 `emit finished`。工作流父对象是主窗口，要到退出才销毁，用户也收不到结果。文件批会被 `commitImport` 的 epoch 检查丢掉，在途图件组会被 `PaleoMapLibrary` 的 `projectDir` / task session 判取消。这是通知漏洞，不是写进新工程。触发要用户在导入中途关工程，未单独开 Finding。

5. **folderimport 线程与活 catalog 写**  
   排除。增量只把 `tableMappings` 拷进 `ImportSession`，worker 上 `produceFiles` → `produceItem` 读 `s.tableMappings`，写入的是 session 的 staging `DataCatalog`。`dataimport_geodata.cpp:509` 只用该映射补 CRS 字符串，再 `projectRasterFile` 写暂存栅格，`addVersion` 仍在 staging 副本。提交仍在 owner 线程 `commitImport`。没有新的活 catalog 写。

6. **析构时仍在飞的导入任务（#638 线索，不新开 Finding）**  
   SUSPECTED，归属 #638，不臆造 Windows 复现。  
   调用链：`applySources` 的 `QTimer` → `importFiles` → `makeImportJob` / `runImportJob` 把 `PaleoTask` 放在 `FolderImportWorkflow` 的任务池里；`ProjectSetupWorkflow::~` 只置 `m_stop`，不调用被丢掉的 `CancelImport`。应用退出时子对象逆序销毁，引导工作流先于 `FolderImportWorkflow` 销毁；`PaleoTaskService::~` 在 `shutdown` 超时后会把仍在跑的池孤儿化（`paleotaskservice.cpp:224` 自己写了 worker 捕获裸指针的 UAF 风险）。`finished` 槽连在 `FolderImportWorkflow` 上，对象销毁后 Qt 会断开，正常拆卸路径看不到 projectsetup 回调的 UAF。与 #638 的 `tst_import` / `tst_folderimport` / `tst_sectionlifecycle` 退出 `0xC0000409` 同族，本机不能证实就是这一条。

7. **offlinebasemapimport：失败当成功、写到工程外、缺 CRS 当成功、中文路径**  
   排除。  
   - 失败不当成功：worker 返回非空错误 → `PaleoTask::applyFinish` 置 Failed；取消且错误为空 → 先看 `m_cancel` 置 Cancelled（`paleotaskservice.cpp:197`）。发布段只在 `Succeeded` 时 `updateMapConfiguration` + `writeProject`，失败走 `failPublication`，`done(false, error)`。对话框取消/销毁连到 `requestCancel`（`offlinebasemapdialog.cpp:68`）。  
   - 不写到工程外：临时目录模板是 `<工程>/basemap/.import-XXXXXX`，`isValid()` 失败即返回；发布用 `rename` 到 `<工程>/basemap/<fingerprint>`。`QTemporaryDir` 不会在模板失败时改落到系统临时目录。  
   - 缺 CRS：`PaleoOfflineRaster::validate` 在图层 CRS 无效时返回错误，worker 失败，不发布。工程未配准且勾选「导入后显示」在起任务前拒绝。不显示时允许只复制进工程，对话框文案写明了这一点，不是把失败当成功。  
   - 中文路径：复制、哈希、`rename` 都走 `QString`。校验失败会返回错误，不会改报成功。未在 Windows 上跑，不能排除 GDAL 打开中文路径失败，但那是失败可见，不是成功或写到工程外。  
   SUSPECTED（不编号）：worker 里 `PaleoOfflineRaster::validate` → `offlineBasemap` 会 `new QgsRasterLayer` 并在 worker 上析构（`offlinebasemapimport.cpp:35`、`offlinebasemapvalidation.cpp:13`、`projectmapreference.cpp:191`）。QGIS 图层对象不是线程安全的。本函数在返回前销毁图层，没有把 worker 上的 `QgsCoordinateReferenceSystem` 留到线程退出之后（不是 #634 那种「退出后仍用 CRS」）。未见因此把失败写成成功。不单开 Finding，避免把未证实的崩溃写成 #638。

8. **证据推迟（#644）丢草稿 / 撤销错版本 / 空证据写成已审核**  
   排除。  
   - `featureEvidenceSummary` 只读图层属性，剥掉空间查询键并标 `spatial_state=deferred`，不写要素。  
   - 保存走 `changeEvidence`：按 fid 重新 `read`，确认解释时对带来源图元的记录做完整 `resolveEvidenceSource`；`spatial_state==unknown` 且正在确认则拒绝（`mappingevidencereview.cpp:679`）。页面把 `expected_revision` 从快照解释对象带上，摘要和详细路径都从同一 `paleo_interpretation` 读出，修订号不会因推迟而换成另一版。  
   - 撤销是 `beginEditCommand` / `destroyEditCommand` 的 QGIS 命令栈，不回放摘要快照。  
   - 草稿：`setDetailsEnabled` → `refresh` → `detailsChanged` → `renderEvidenceSnapshot`。重建列表包在 `QSignalBlocker(m_evidence)` 里，`currentRowChanged` 不会把 `m_evidenceText` / `m_evidenceReason` 刷回已存文本。  
   - 无记录的摘要没有 `feature_id`，`nativeSelectionMatchesEvidence` 为假，审核按钮不开。没有把空证据写成 Confirmed 的写路径。  
   多选时 `capture` 用 `evidence()` 覆盖 `records`，仍只影响视图，不落盘。

9. **stratigraphicwebsession 四行 diff**  
   排除。只在 `start()` 前插入 `PYTHONUTF8=1`。没有新 `connect`，没有新进程所有权。析构仍 `m_process.disconnect(this)`，并按进程组 SIGINT/SIGKILL（Windows 上 terminate/kill）回收 `ownsService()` 的进程。`fail()` 仍断开并 abort `m_reply`。Linux 上该变量与缺省 UTF-8 等效，不改变会话生命周期。

10. **层违规**  
    排除。上述增量文件没有 `#include` `ui/` 或 `QtWidgets`。`projectsetup` 的 `QFutureWatcher` / `QTimer` / `QtConcurrent` 是 QtCore。`offlinebasemapimport` 只包含 qgis 封装头与 `QDir`，没有直接 `Qgs*` 头；worker 里的 `QgsRasterLayer` 在 qgis 封装函数内部，见疑点 7，不是 workflow include ui。

11. **只 qWarning（#556）**  
    排除。新引导、底图、证据摘要、井因素提取都把错误放进 `*error`、任务错误或 `finished` / `done`。`folderimport.cpp:363` 的台账 `qWarning` 是旧路径，且台账失败不改变导入返回值；同 #556，不在本增量的必经且无返回值的新链上。

12. **paleomap 政策哈希从 2 改为 3 并加入 `project_crs`**  
    不是本报告的 Finding。旧版本的 `normalization_policy` 对不上时会再出一版派生，这是 #637 换 CRS 后重整理的意图。若测试把版本号冻在政策变更前，见 #645 节。

## #645 归属分析

本切片没有把 #645 已列症状再编号。对照：

- 相分类阈值个数 / 图例、岩性道标题、assetpaging 类型、onlinebasemap 像素：不在这批 workflow 文件的变更里。
- 导入版本计数：`paleomapimport.cpp` 把政策哈希从 `normalizer: 2` 改成 `normalizer: 3` 加上 `project_crs`。已入库图件再整理时，指纹未变也会因政策不等而新增派生版本，而不是 `unchanged`。这更像测试期望没跟上政策迁移，不是静默双资产（同一 `sourceUri+horizon` 仍命中原资产，`addAsset` 只在 `current.id.isEmpty()` 时发生）。未读失败套件的断言正文，不能把某一条 linux 红标成产品缺陷或过期测试的终审。
- `tst_wellattributes`：C-1 会让「缺测单因素行 + 仍有分层」的井不再回算。若那 3 个失败断言的是回算值，C-1 是候选根因；若断言的是文案或版本计数，则不是。未打开失败日志，不归入 #645 结案。

## 覆盖清单

| 文件 | 深度 |
|---|---|
| `projectsetup.cpp` / `.h` / `projectsetupmaps.cpp` | 逐行（新文件全文）+ 对话框、`runNewProject`、`createProject`、`commitImport`、`PaleoMapLibrary::importMaps` |
| `folderimport.cpp` / `.h` | 逐行 diff；作业队列、`makeImportJob`、`runImportJob`、`produceFiles` / `produceItem` 的 tableMappings 与提交线程读到调用点 |
| `offlinebasemapimport.cpp` / `.h` | 逐行全文 + `PaleoTask::applyFinish`、`validate`、`offlineBasemap`、对话框取消接线 |
| `paleomapimport.cpp` / `paleomaplibrary.h` | 逐行 diff + `prepare` 全文到派生版本登记；`importMaps` 发布段逐行 |
| `evidenceinteraction.cpp` / `.h` | 逐行 diff + 全文 |
| `mappingevidencereview.cpp` | `featureEvidenceSummary` 逐行；`featureEvidence`、`changeEvidence`、`resolveEvidenceSource` 入口、`appendEditEvidence` 读到写路径。文件其余部分接口级 |
| `mappingworkbench.h` | 逐行 diff（仅声明） |
| `constraintwellfactors.cpp` | diff 所在的 `sourceFor`、`computeExtraction` 取值段、`extractWellFactorsImpl` 派发段逐行；保存与插值后半接口级 |
| `stratigraphicwebsession.cpp` | 四行 diff 逐行；`launch` / `fail` / 析构逐行。其余会话状态机接口级 |

未核查：未构建、未跑 ctest、未在 Windows 上打开中文路径底图。未读 #645 失败套件的断言与日志。`mappingevidencereview.cpp` 约 250–490 行与 780–850 行、`constraintwellfactors.cpp` 保存井点之后的插值段，只按调用关系扫过，没有逐行。
