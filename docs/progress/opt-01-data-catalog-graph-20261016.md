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

## R1 基线（干净 master）

- 构建 `flock … cmake --build build -j4` EXIT=0（2198 步，2506 s 冷构建）。
- `ctest -j2`：408 项 406 过 / 2 红（tst_singlefactor_fieldcontours、tst_horizontracksvc，本机 GDAL 3.10 环境项）；
  任务列出的其余 6 个既有红在新 master 上本次均通过（基线漂移，已记账本）。

## 轮次记录

### 轮 1：U1–U6（详见账本「轮 1」）

| 单元 | 提交 | 前 → 后 |
|---|---|---|
| U1 井口回查 | 197af242 | 同名井口表第二行开第一份资产预览 → 按 sourceUri 精确定位，歧义不猜 |
| U3 服务析构 | b5d7ad9a | 悬垂 m_svc（旧码 SIGSEGV）+ 未提交行报入库 → QPointer + 行如实报失败 |
| U2 损坏台账 | e99616e7 | 下一批覆盖抹掉全部历史、查看器 0 批无因 → 先备份原字节（失败拒写）+ 状态栏原因 |
| U4 图谱返回栈 | cb8e2105 | 追溯入口压入未请求中间态 → 一次直达 |
| U5 对比包含性 | f4125de7 | 越界受管路径回落原始 path 读工程外文件 → 视为不可读 |
| U6 对比样本 | 7c26fd69 | A 侧 ≥40 行时 B 侧零样本 → 两侧分摊 |

本域专项 Oracle 进度：「失败/中断时原始资产不受损」断言已有 3 处（U3 源文件字节、U2 拒写时原件字节、
U5 受管 v1 字节）；八类矩阵、工程 E2E 双跑、1k/10k/100k 性能 A/B 尚未做（下一批，P1/P2）。

### 轮 2：批次 2（详见账本「轮 2」）

| 单元 | 提交 | 前 → 后 |
|---|---|---|
| R-9 多工程文件目录 | f9b669c1 | 字母序静默打开首个 .qgz → 拒绝并列出候选（非致命提醒） |
| R-7 + P4 图谱双发 / 往返 | cd2344fa | 单击追溯/影响/展开关联发两次查询 → 一次；节点→版本→预览→追溯→返回全程真实 id 断言 |
| P2 台账基准 + 优化 | db876508、91aa5fd6 | 10 万行批次 record ≈1.9 s → ≈0.9 s、回读 30 万行 ≈1.95 s → ≈0.5 s；落盘 sha256 A=B |
| S5 Low 修复 | 7ee24b66、298416fd、a479c654、9bfc3e27、9aa9dd94、72ef6604、1f482a9d | U1-c/d、U3-d、U2-b/c/d/e、U4-c、U5-c |

### 独立 review（S5）

- 第 1 轮（dbb03269..0dd90e07，静态）：0 High / 0 Medium。
- 已修：U1-c、U1-d、U3-d、U2-b、U2-c、U2-d、U2-e、U4-c、U5-c。
- 未修（不在写入域）：U5-e（versiondialog.h）。
- 记入 PR 已知限制：U1-b、U4-b、U5-b、U5-d、U6-b，以及账本历史中出现的本地补丁标记字样。
- f9b669c1 及之后的提交待 S5 第 2 轮。

### 批次 2 新增：P1/P3/P5

| 单元 | 提交 | 内容 |
|---|---|---|
| P5 台账串账 | fe15e9a2 | 导入在飞时切换工程，作废批次被记进新工程台账 → 比对 session 工程与当前 catalog，不一致不入账（旧码红） |
| P1 E2E 双跑 | 6f736cd2 | tst_datagraphpage::projectDataEndToEndTwice（run1/run2 摘要一致：assets=2;versions=4;links=2;recoveredNo=3） |
| P3 换代/取消 | cd92a246 | tst_datagraphpage::rapidRefocusLandsOnLatestGeneration |

### 外部可见流程八类矩阵（本 PR 触及的流程；✓=有测试，—=不适用并说明）

| 流程 | 正向成功 | 错误输入 | 取消/关闭 | 跨工程切换 | 重复执行 | 并发排队 | 重开后持久化/恢复 | 历史版本兼容 |
|---|---|---|---|---|---|---|---|---|
| 文件夹导入 + 台账 | ✓ rejectedRowsReachLedgerAndAcceptedTops | ✓ allInvalidRowsAreFailureWithReasons | ✓ folderImportSurvivesServiceTeardownHonestly（服务关闭） | ✓ folderImportAcrossProjectSwitchDoesNotCrossPostLedger | ✓ corruptLedgerPreservedBeforeOverwrite（第二批不再备份） | 既有 FIFO 用例（导入作业串行 + 冲突重做，tst_folderimport） | ✓ ledger 每次 load 读回；ledgerScaleRoundTrip 逐字段回读 | ✓ corruptShapesAreNotOverwritten / unreadableLedgerRefusesWrite（旧/坏形状不被覆盖） |
| 井口资产回查 | ✓ importedWellHeadAssetResolvesSameNameBySourcePath | ✓ 同名无路径命中 → 空 | — 同步只读查询 | — 依赖当前 catalog | ✓ 重复导入取最新 | — | — 只读 | — |
| 打开工程目录（R-9） | 既有 stampSourceAreaDirGate | ✓ openPathRefusesAmbiguousQgzDirectory | — 拒绝即不打开 | — | — | — | — | — |
| 资料图谱导航 | ✓ nodeVersionPreviewBackRoundTrip | — 无用户文本输入（检索框既有用例） | ✓ rapidRefocusLandsOnLatestGeneration（取消计算） | 既有 tst_datagraph 工程换代 | ✓ 单击只发一次查询（R-7） | ✓ 合并窗内连发只落最新代 | ✓ 返回栈逐级恢复 | — 图谱不落盘 |
| 版本对比 | ✓ compareVersionsSamplesBothSides | ✓ compareVersionsRefusesManagedPathOutsideProject | — 同步 | — | — | — | — | — 只读 |
| 工程数据 E2E | ✓ projectDataEndToEndTwice | 关联：未决→归位 | — | — | ✓ 双跑摘要一致 | — | ✓ 新服务实例重开读回 | 未覆盖：box 无历史真实工程，未做「历史真实数据回读」 |

「原始资产在失败/中断后完好」断言：U3（服务析构后源文件字节）、U2/U2-d（拒写时原台账字节）、U5（受管 v1 字节）、
P5（切工程后源文件字节）、P1（更新/回滚后 v1 历史字节与源文件）——≥3 处。

### 轮 3：S5 第 2 轮修复（详见账本「轮 3」）

M-1（b3d849d6）；Low 2（eb9aee8c）、4（25acad5a）、16（720e0534）、15（a425e229）、14（577dd9f9）、9（a3d5bc61）、7（f0947ac3）、12（de2521ff）。Low 8/11 留作已知限制。

## 收尾门禁（R10，2026-10-16 05:2x UTC+8）

- `git fetch --prune origin`：origin/master 仍为 BASE_SHA dbb03269（未移动，无需合并）；open PR #486/#487/#488 与本 PR 文件交集为空；open issues 仍为 #460–#485（26）。
- 全量构建：`flock -x <git-common-dir>/paleo-shared-build-j4.lock bash -lc 'cmake --build build -j4'` rc=0（de2521ff 代码态；a5e7044c 之后仅文档，`ninja: no work to do`）。
- 全量 `QT_QPA_PLATFORM=offscreen ctest --test-dir build -j2`（de2521ff 代码态，04:44–05:17，real 641 s，`/tmp/opt01-ctest-final.log`）：**409 项 407 过**，红 = {tst_singlefactor_fieldcontours, tst_horizontracksvc}，与干净 master 基线完全相同。
- tst_singlefactor_perf 定性：该二进制 02:32 由干净 master 源构建、此后未重链（只链 paleo_algorithms，本 PR 零触及；libpaleo_algorithms.a 01:55）。基线 164.5 s 通过 → 批次 2 全量与单跑两次超预算（L 档 15303 ms > 15000 ms，box load avg 最高 ≈9）→ 收尾全量 165.6 s 通过。同一 master 二进制时过时不过 ⇒ **主机负载导致的预算抖动，与本 PR 无关**。
- 定向 8 套（tst_folderimport / io_importledger / assetops / assetpaging / datagraphpage / datagraph / storagegovernance / panels）`ctest -j2 -R …` **连跑两遍 8/8**（`/tmp/opt01-ct-final-run{1,2}.log`）。
- 静态门禁：`git diff origin/master --check` 0；`python3 tools/check_layering.py --strict` 通过；`python3 tools/check_ui_invariants.py --strict` clean；`python3 tools/check_i18n.py` 通过。
- 本地补丁标记扫描：最终树 0；src/tests/cmake 在全部提交中 0；仅文档散文出现于 12847382/8456f9f1（ef79b2ee 已删除）。两份本地补丁文件仍为 skip-worktree，未提交。
