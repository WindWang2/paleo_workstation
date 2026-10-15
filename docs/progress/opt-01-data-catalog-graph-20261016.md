# opt-01 数据管理 / 资产版本 / 资料图谱 / 导入预览深度闭环（2026-10-16）

- 分支 `goal/paleo-opt-01-data-catalog-graph-20261016`，worktree
  `../paleo_workstation-opt-01-data-catalog-graph`（独立 `build/`，Release + Ninja，GCC 14 /
  Qt 6.8.2 / QGIS 4.2.3 / GDAL 3.10.3）。
- **BASE_SHA** = `dbb03269ca1809145e1ee39b699c6fd08916a317`（启动时 `git fetch --prune` 后 origin/master）。
- 逐轮证据账本：仓库根 `.goal-loop-ledger-opt-01.md`。

## R0 基线冻结

### 排除矩阵（EXCLUDED_ISSUES 指纹 → 本方向避让）

启动时 `gh issue list --state open --limit 500`：**26 个 open（#460–#485）全部排除**，不修其子项、不评论。
与本方向写入域相邻、需逐项避让的指纹：

| Issue | 指纹（文件/函数/症状） | 本方向处理 |
|---|---|---|
| #461 | `metadata/atomicfile.h` paleoWriteFileAtomic 只 flush 不 fsync | 不碰 atomicfile；本轮台账仍用 QSaveFile（既有口径） |
| #462 | `metadata/paleoprojectstore.cpp` commitAll 回调无 try/catch | 不碰 |
| #464 | mapversioncontroller_async busy-token 跨工程 | 不碰 |
| #465 | mappingworkbench_edit saveEditingVersion 部分完成 | 不碰 |
| #466/#467 | LasParser::parse 旧入口 / DLIS-LIS parseHeader 无界 | 不碰 io 解析器 |
| #472 | Qgs* 越层 | 不新增 Qgs* 直连（本轮改动零 Qgs 引用） |
| #476 | `catalog/datacatalog_versions.cpp` versions 缺 (asset_id,version_number) 唯一约束、currentVersion tie-break；`datacatalog.cpp:124-133` noteRead 跨线程；metastore/releasestore/layouttemplatestore | **datacatalog_versions.cpp / datacatalog.cpp 本轮零改动** |
| #477 | io：ziparchive CRC、sfpkgreader 字节序、segyreader freeze、constraintstore DDL、lasparser parseRange、laswriter Inf、sattrio、rasterpyramid、wellcompositexml、encodingdetect、cachecore、lasalias | 不碰 |
| #480 | workflow：mapversioncontroller、mappingworkbench、constraintworkflow、wellsectionworkflow、constraintfactorjobs、faultinterpretation、mappingworkbench_predict | 不碰（本轮 workflow 改动仅 folderimport/importledger/assetops） |
| #481 | ui 主壳 attach_* / paleodecorations / wellsection | 不碰 paleomainwindow* |
| #482 | `pages/datalist.cpp:655,661` static_cast 布局 / findChild；`datalist_undo.cpp:64-177` undo_stack 持久化散落；previewmappage 鹰眼按键；seismicsection* | **datalist.cpp 零改动；datalist_undo.cpp 只改 `showImportLedger`（行 ~495，不在 64-177 区段）**；previewmappage 零改动 |
| #483/#484/#485 | qgis/app、构建/CI、总追踪 | 不碰 |
| #460/#463/#468–#471/#473–#475 | ONNX、3D 书签、SGS、远端响应、随机数、wavelet、attachWorkbench、symbol-audit、已知红 | 不相交 |

**Open PR**（`gh pr list --state open`）：#486 `codex/predict-fallback-gl-watchdog`
（welllogfacies.cpp、seismic3dviewpanel.{h,cpp}、tst_wellfacies.cpp）、#487 `codex/facies-legend-groups`
（qgisstyleservice.cpp、tst_services2.cpp）——**零文件交集**。

**已合并 PR 去重**（读最近 35 个）：#450（方向 01 前一轮：A1 文件夹失败行回滚 / A2 单文件取消不提交 /
A3 批量改型视图 / B1 undo vault 原子写 / B2 open 删除面 / B3 回滚 extra / C1 pushCommands / C2 QPointer）、
#433（图谱预算诚实性·导出契约·LRU·回滚血缘·sidecar 原子写）、#424（资料图谱）、#437/#455/#456（图谱证据）、
#459（测区图自愈）、#454（工程门/拖拽导入）。本轮候选逐条对照上述交付点，**不重做**。
`docs/progress/data-asset-graph-deepening-20261013.md` 余项 1（图谱返回栈 4/6 维不对称）经复核：
当前实现入栈时压入的是完整 6 维 `m_queryState`，返回恢复同一份——「不对称」实为「搜索词变化不单独
入栈」的设计取舍，**不算缺陷**，未改；本轮另行发现并修复的是「单入口压入中间态」（U4）。

### 写入白名单（本轮实际触碰）

`src/workflow/{folderimport,importledger,assetops}.*`、`src/ui/pages/{datagraphpage}.*`、
`src/ui/pages/datalist_undo.cpp`（仅 showImportLedger）、`tests/tst_{folderimport,io_importledger,assetops,assetpaging}.cpp`、
新增 `tests/tst_datagraphpage.cpp` + `cmake/extra-data-asset-deepening.cmake` 一行注册、本文档与账本。
`src/qgis/` 只读；根 CMakeLists.txt / AGENTS.md / DESIGN.md / jobrunner / paleomainwindow.h 零改动。

### 调用链映射（文件 → 入口 → 调用 → 持久化/显示 → 测试）

| 链路 | 生产入口 | 调用 | 持久化 / 显示 | 既有测试 |
|---|---|---|---|---|
| 工程打开/新建 | `PaleoMainWindow::openProject*`（paleomainwindow_project.cpp） | `ProjectOpenWorkflow::openPath` → `QgisProjectService::openProjectAsync/createProject` → 空目录 `folderImportRequested` | `.paleo` 工程文件 `sourceAreaRoot`（stampSourceArea） | tst_folderimport::stampSourceAreaDirGate、tst_projectsvc、tst_shell_projectlifecycle |
| 文件夹导入 | 确认表 `PaleoFolderConfirm`（ui/dialogs/folderconfirm.cpp）hooks | `FolderImportWorkflow::importFolder` → `DataImportService::produceFolder`（worker，staging）→ `commitImport`（GUI 单事务）→ `recordLedger` | catalog（SQLite/journal）+ RAW 受管副本 + `.paleo/import_ledger.json` | tst_folderimport、tst_import、tst_io_importledger |
| 单文件/拖拽导入 | datalist_dnd / attach_data | `FolderImportWorkflow::importFile`（discardOnCancel） | 同上（无台账） | tst_folderimport::singleFileImportCancelledDiscardsCommit |
| 井口回查→预览 | folderconfirm.cpp:570 `hooks.importedWellHead` | `FolderImportWorkflow::importedWellHeadAsset` | `m_previewTabs->openAsset` | tst_folderimport::importedWellHeadAsset* |
| 版本回滚/对比 | datalist_undo（版本历史对话框） | `assetops::rollbackToVersion / compareVersions` | 新版本行 + 受管拷贝；对比对话框 | tst_assetops |
| 回收/物理清理 | datalist_undo（回收站）/ 存储治理对话框 | `StorageGovernanceController::purgeAssets/scan/confirmPreview` → `assetops::purgeAssets/purgeManagedFiles` | catalog 删除 + 受管字节清理 | tst_storagegovernance、tst_assetops、tst_assetpaging::governanceUi* |
| 资料图谱 | DataPage → `DataGraphPage`（openRelations/openLineage 由 attach_data 接线） | `DataGraphWorkflow::request`（180ms 合并、JobRunner、8 槽 LRU）→ `datagraph::buildIndex/query` | QGraphicsScene 渲染、返回栈、导出 JSON（QSaveFile） | tst_datagraph（服务+工作流），**页面零测试（本轮补 tst_datagraphpage）** |
| 导入台账查看 | datalist「导入台账」按钮 / attach_data `imports` 动作 | `DataListPanel::showImportLedger` → `ImportLedger::load` | ImportLedgerDialog | tst_folderimport::mixedGoodBadImportIsHonestAndLedgered、tst_io_importledger |
| 资料预览 | DataPreviewTabs（datapreview/*） | PreviewDocService → 各 tab | 只读 | tst_datapreview |

## R2 可执行 backlog（三态分类）

**A. 已实现且有测试（不动）**：文件夹失败行回滚（#450 A1）、单文件取消不提交（A2）、批量改型视图（A3）、
open 删除面复位（B2）、回滚携带 extra（B3）、pushCommands（C1）、图谱预算诚实性/导出同源/LRU/回滚血缘（#433）、
sidecar 原子写+损坏告警（#433 轮 4）、台账行级结局（方向 30 + mixedGoodBadImportIsHonestAndLedgered）。

**B. 已实现待加强**：
- 资料图谱页零页面级测试 → 本轮新增 tst_datagraphpage（随 U4）。
- 台账损坏仅 qWarning，查看器显示「0 批」→ U2 补可解释原因。
- purge 与 recycle_bin.json 无对账（2026-10-13 余项 4）：现已有 `assetsPurged` 信号由 datalist 消费，
  需进一步核实跨会话残留（候选 R-8，待下一批）。

**C. 新发现、未立案、未被占用的缺陷**（每条：文件 / 原因 / 边界 / Oracle / 测试 / 重叠核对）：

| # | 级别 | 文件:函数 | 原因 | Oracle / 测试 | Issue/PR 重叠 |
|---|---|---|---|---|---|
| U1 | P1 一致性 | workflow/folderimport.cpp `importedWellHeadAsset` | 只按文件名找第一份 well_head：工区不同子目录同名井口表 → 第二行打开第一份资产预览 | 同名两份各自回到 sourceUri 一致的资产；歧义返回空 — `tst_folderimport::importedWellHeadAssetResolvesSameNameBySourcePath` | 无 |
| U2 | P5 可观察性/零破坏 | workflow/importledger.cpp `load/record` + datalist_undo `showImportLedger` | 损坏台账读出空历史，下一批 record 用「仅本批」覆盖 → 全部审计历史静默蒸发；查看器无原因 | 原字节逐字备份恰一次、新台账可读回、备份不可得拒写且原件不动、查看器给原因 — `tst_io_importledger::corruptLedger*`、`tst_assetpaging::importLedgerViewerReportsCorruptLedger` | #461 是 atomicfile fsync，不同函数/文件 |
| U3 | P1 生命周期 | workflow/folderimport.{h,cpp} `m_svc`、finished 处理器 | 服务析构后 finished 走「已关闭」分支仍经裸 `m_svc` 写台账（悬垂指针）；未提交的行仍报 Imported（假成功） | 回调收到丢弃原因、无行报入库、源文件字节不变、后续作业不触悬垂 — `tst_folderimport::folderImportSurvivesServiceTeardownHonestly` | #464 是 busy-token，不同 |
| U4 | P2 交互 | ui/pages/datagraphpage.cpp `openLineage` | 先按「资料关联」request 再切「成果追溯」再 request：中间态被压栈，「返回上一图谱」落到用户未请求过的视图 | 概览→追溯→返回=概览且返回键灭；关联→追溯→返回→返回逐级 — `tst_datagraphpage` | #424/#433 未覆盖返回栈 |
| U5 | P1 安全/一致性 | workflow/assetops.cpp `compareVersions` | 受管版本 resolvedVersionPath 为空（绝对路径/符号链接逃逸）时回落原始 v.path：读出工程外文件、或相对 CWD 读无关文件，绕过包含性闸门 | 越界一侧 size=-1、不做文本对比、无外部内容泄入样本 — `tst_assetops::compareVersionsRefusesManagedPathOutsideProject` | #476/#477 无此函数 |
| U6 | P3 细节 | workflow/assetops.cpp `compareVersions` 样本 | 40 条样本先全给 A 侧，A 差异 ≥40 行时 B 侧（新内容）零样本 | 60/60 → 20/20；60/3 → 37/3 — `tst_assetops::compareVersionsSamplesBothSides` | 无 |
| R-7 | 候选 | ui/pages/datagraphpage.cpp m_trace/m_related/m_impact/Entity 激活 | setCurrentIndex 未屏蔽信号 + 显式 request → 同事件内双发 request（合并窗吸收，代次 ++ 两次） | 待量化（低） | 无 |
| R-8 | 候选 | storagegovernancecontroller / recycle_bin | 物理清除后回收清单跨会话残留（10-13 余项 4） | 待复核 | 无 |
| R-9 | 候选 | workflow/projectopen.cpp `openPath` | 目录内多个 *.qgz 且无同名 qgz 时取字母序第一个，静默 | 待定（改默认口径需取舍） | 无 |

## 轮次记录

（R1 起逐轮追加。）
