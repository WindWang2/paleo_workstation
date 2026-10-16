# opt-05：QGIS 图层、综合编图、证据交互、地图版式与验证发布整体深化

- 分支：`goal/paleo-opt-05-qgis-compose-publish-20261016`
- worktree：`/home/kevin/projects/paleo_workstation-opt-05-qgis-compose-publish`
- BASE_SHA：`dbb03269ca1809145e1ee39b699c6fd08916a317`（= 启动时 origin/master）
- 盘点时间：2026-10-16；构建：`RelWithDebInfo`，vendored superbuild prefix 共享符号链接，
  跨 worktree 编译锁 `$GIT_COMMON_DIR/paleo-shared-build-j4.lock`，一律 `-j4`。

## R0 基线冻结

### 排除矩阵（快照于启动时 `gh issue list`/`gh pr list`）

**Open Issues #460–#485（26 项，全部不处理、不评论、不触碰）**，与本域交集指纹：

| Issue | 指纹（file:hunk） | 与本域关系 |
|---|---|---|
| #465 | `mappingworkbench_edit.cpp:288,372-390` | 文件不在本域白名单，邻近证据代码改动时不得顺带修复 |
| #464 | `mapversioncontroller_async.cpp:343-346` + `PaleoProjectStore::setProjectPaths` m_busy | 文件不在本域 |
| #472 | workflow/ui 112 文件 Qgs* 直连 + 无检查器 | 系统性检查器/门面收敛不做；本域新代码沿用既有模式 |
| #473 | `paleomainwindow_workbench.cpp` attachWorkbench 1060 行 | 文件不在本域（本域只有 `paleomainwindow_attach_compose.cpp`） |
| #476 | `releasestore.cpp:25`（qHash 连接名）、`releasestore.cpp:28-47`（readOnly 通道）、`mapversionstore.cpp` 90 行复制门禁 | releasestore 仅允许动非 issue 部分 |
| #480 | `mapversioncontroller*`、`mappingworkbench.cpp:870-876,907-913`、`mappingworkbench_predict.cpp:813-824`、`faultinterpretationcontroller.cpp:40-46` 等 | 全部不在本域白名单 |
| #481 | `paleodecorations.cpp:56` setDark 零调用/gDark、`paleomainwindow_attach_*`、`paleoicons.cpp` 等 | decorations 目录在本域但该 hunk 排除 |
| #482 | seismicsection/wellcomposite/datalist/datapreview | 不在本域 |
| #483 | `qgisprojectservice_async.cpp:159,192-193,224-262`、`previewmapcanvas.h:142-145` + `.cpp:260-290,493-514`、`mappingartifactwriter.cpp:38-58`、`projectmapreference.cpp:20-31,81-104`、`qgislayerservice.cpp:263-268` | src/qgis 在本域但这些 hunk 排除 |
| #484 | 构建/CI | 不在本域 |
| #485/#475 | 总追踪/CI 红项分诊 | 不做 |

**Open PRs（只读，零交集要求）：**

| PR | 分支 | 占用文件 |
|---|---|---|
| #486 | `codex/predict-fallback-gl-watchdog` | `src/domain/welllogfacies.cpp`、`src/ui/seismic3d/seismic3dviewpanel.{h,cpp}`、`tests/tst_wellfacies.cpp` |
| #487 | `codex/facies-legend-groups` | `src/qgis/qgisstyleservice.cpp`、`tests/tst_services2.cpp` |

**兄弟 worktree（同机运行中）**：`paleo_workstation-opt-02-well-seismic-correlation`、
`paleo_workstation-opt-03-singlefactor-constraint`——文件域不交叉（本域文件清单见下），
构建经共享 flock 串行。

### 本域可写集（任务书 ∩ 排除后）

- `src/qgis/`：`qgislayerorganizer`、`qgislayerprofile`、`qgislayerservice`（除 :263-268）、
  `qgiseditingservice`、`qgislayoutservice`、`layoutexport`、`mapbooklayout`、`standardelements`、
  `factorcontour`、`factorstylewriter`、`geopatterns`、`semanticlegend`、`topologicalindex`、
  `layervocabulary.h`、`mapcanvaslink`、`crossplotmaplink`、`draftautosave`、`facieshierarchyrenderer`、
  `manifestprojection`、`previewidentify`、`previewmaptools`、`previewrendercache`、`qgiscanvascontroller`、
  `qgislabelzorder`、`qgisprocessingservice`、`seismicsectiontool`、`wellsectionmapband`、
  `wellattributestore`、`qgiserrors_internal.h` 等**非保护文件**；
  **只读**：`qgisstyleservice.cpp`（#487）、`qgisprojectservice*`（#483 hunks + #437 家族）、
  `mappingartifactwriter`（#483 hunk）、`projectmapreference`（#483 hunk）、
  `previewmapcanvas`（#483 hunk）、`evidencemapadapter`/`mappingfeatureidentity`/`qgisconstrainteditsession`（#437 家族）
- `src/ui/layers/`、`src/ui/layout/`、`src/ui/decorations/`（除 setDark/gDark hunk）、`src/ui/faciesmapping/`
- `src/ui/pages/{composepage,validatepage,mappingworkbenchpage}*`
- `src/ui/paleomainwindow_attach_compose.cpp`
- `src/ui/releasepanel.cpp`（发布列表 UI；任何 open issue/PR 均未占用此文件）
- `src/workflow/{compositionworkflow,mapexport,mapbook,mapbookqueue,validationworkflow,mappingworkbench_compose,mappingevidencereview}*`
- `src/metadata/releasestore.*`（除 :25 qHash、:28-47 readOnly 两个 #476 hunk）
- 对应 `tests/tst_*`

### 已交付能力（不得重新包装成交付）

- #448（方向 05 前轮）：page:* 主题重放守恒无声明临时图层（W1）、declared() 热点去放大
  （W2/W3）、翻译上下文+三角洲别名（W4/W9）、模板失败反馈 templateFailed（W5）、
  mapbooklayout 字体工厂+validateExportedFile 头尾校验（W6/W7）。
- #452（方向 07）：后台任务中心、验证发布、图件导出。
- #437：feature_uid/解释主张/约束审计/版本快照；#456：解释同步+gpkg 锁+窗口适配。
- W8（exportLayoutAsync 任务创建失败不 emit）前轮已判「PaleoTaskService::start 恒非空，
  契约自洽非缺陷」——不重报。

## R1 运行/测试基线与验证记录

- configure：`cmake -S . -B build`（RelWithDebInfo，vendored prefix 共享符号链接），共享锁排队后完成。
- 构建：`cmake --build build -j4 --target <11 个受影响测试目标>`，冷构建编译了 vendored
  依赖；期间修复 1 处编译错（`tst_layertreepanel` 误把 `QgsLayerTreeGroup` 当
  `QgsLayerTree` 用 `setHasCustomLayerOrder` 系列 API）。
- 终态定向测试（共享锁下 `ctest -j2 -R` 11 项）：
  `tst_release`、`tst_mapbook`、`tst_panels`、`tst_workflows`、`tst_mappingpages`、
  `tst_composeworkflow`、`tst_layerplatform`、`tst_layerproperties`、
  `tst_layertreepanel`、`tst_mapping`、`tst_layerorganizer` —— **11/11 通过**。
- 非编译门禁：`git diff --check` 干净；`check_layering.py` / `--strict` / `--selftest`
  通过（零残留 baseline）；`check_ui_invariants.py` / `--strict` clean（0 违规）；
  `check_i18n.py` / `--selftest` 通过；`check_ui_tokens.py` violations=0。
- 验证期自查出并修复的 2 处回归（测试先红后绿）：
  1. `attach_compose` 跨层位门禁最初对空 `activeHorizon`（新建工程未选 chip）也拦截，
     导致 `faciesPolygonsReady` 不再经 `prepareFaciesForEditing` 铺工作副本——
     `saveFaciesAttributes` 走「只读派生件→重铺→重实例化」，既有测试手持的旧
     `QgsVectorLayer*` 悬垂 SIGSEGV。门禁改为仅拦「非空且不同」的层位。
  2. `moveSyncsActiveCustomLayerOrder` 用例对单例 `QgsProject::instance()` 假设
     空工程，前序用例遗留层污染树序断言——改为显式 `removeAllMapLayers()` +
     `layerTreeRoot()->addLayer()` 逐层定序（同时不假设 `addMapLayers` 插入顺序）。

## R2 已实施改善单元（11 个，均带回归 Oracle；编号沿用审计，organizer 批量化记 U9a、版式守护记 U9b）

| # | 单元 | 文件 | Oracle（测试） |
|---|------|------|----------------|
| U1 | 发布清单掉 `LayerDeclaration.title`——ManifestProjection 内嵌格式有 title，ReleaseStore 序列化缺字段：重开读回丢显示名、`diff()` 对纯改名失明。补齐 title 序列化/反序列化并纳入 manifestKey | `src/metadata/releasestore.cpp` | `tst_release` manifestAtRoundTripsTitle / diffFlagsTitleOnlyChange |
| U3 | `ReleaseStore::diff()` 对损坏 manifest_json 静默当空集→把「快照损坏」谎报成「对方全新增」。existence 查询连带校验载荷可解析，不可解析报具名错误 | 同上 | `tst_release` diffRefusesCorruptManifest |
| U2 | 地图册索引页/manifest 装配失败只进 `failures[]`——`complete()` 报喜、`summary()`「失败 0」。complete() 连装配失败一起算；summary 单列「装配失败 N」 | `src/workflow/mapbookqueue.{h,cpp}` | `tst_mapbook` batchQueueAssemblyFailuresAreCounted |
| U6 | `ValidatePage::clearResults()` 清行不调 `clearCellWidgets()`——setRowCount(0) 不删 cellWidget，切工程后胶囊控件成孤儿叠画（文件自身注释自证的风险） | `src/ui/pages/validatepage.cpp` | `tst_panels` validatePage_clearResultsResetsProjectArtifacts（扩） |
| U4 | `CompositionWorkflow::fuseFactors` 无层位门禁：空层位产 `composite.` 畸形声明、跨层位因子混进本层位产物。拒绝空层位 + 输入声明层位必须匹配 | `src/workflow/compositionworkflow.cpp` | `tst_workflows` fusionRejectsEmptyAndCrossHorizon |
| U8 | 综合页相属性/边界核查目标全局限定——`refreshFactors` 收集全部 `facies.*` 矢量不限层位，跨层位矢量化完成还会自动接管目标层并进编辑。页内新增 `setComposeHorizon`（壳喂 activeHorizon），因子表/相栅格下拉/相属性目标只收本层位；壳侧跨层位完成不接管不编辑只留状态行 | `src/ui/pages/composepage.{h,cpp}`、`src/ui/paleomainwindow_attach_compose.cpp` | `tst_mappingpages` faciesTargetAndListsRespectComposeHorizon、`tst_composeworkflow` crossHorizonReadyDoesNotStealFaciesTarget |
| U5 | `ensureSharedDataVisible()` 愈合后写回 `pageThemeName(m_currentPage)`——applyPageProfile 路径里它还是离开的那页，把本页树态灌进别页档案（跨页可见性串档、随 .qgz 持久化）。显式传「被应用的主题」名 | `src/qgis/qgislayerprofile.{h,cpp}` | `tst_layerplatform` sharedDataHealWritesAppliedThemeRecord |
| U7 | 图层树对无 paleoLayerId 的手工层按契约回退发射 `QgsMapLayer::id()`，但 `resolveLayer` 只查实例缓存+paleoLayerId 扫描——属性对话框/业务页对该路径恒失败。补原生 id 兜底 | `src/ui/layers/layerpropertiesdialog.cpp` | `tst_layerproperties` manualLayerResolvesByQgsMapLayerId |
| U9a | `legendLayersAdded` 批量归位逐层 `declared()`（sqlite 全表读×层数），`reorganize()` 建组/归位/剪枝三次复读——与 goal7 W3 已验证的取表约定相悖。批量路径共享一次快照 | `src/qgis/qgislayerorganizer.{h,cpp}` | `tst_layerorganizer` 既有摆放回归（行为等价） |
| U10 | `LayerTreePanel::moveCurrentNode` 挪层不同步 `customLayerOrder`——自定义绘制序生效时（「显示预测点位」置顶启用它）画布叠置停在旧位。被挪子树按新树序 `slotIntoCustomOrder` 回填（自下而上），特意置顶条目不打平；S5 复审补网：拖拽 InternalMove（mime 克隆不经 moveCurrentNode）经 `rowsInserted` 收集 + 排队回填 | `src/ui/layers/layertreepanel.cpp` | `tst_layertreepanel` moveSyncsActiveCustomLayerOrder / dragMoveSyncsActiveCustomLayerOrder |
| U9b | `buildHorizonMapLayout` 改写活工程图层（nodata 透明表/井名标注）且不还原——画布残留「出图专用」状态随 .qgz 持久化。`TransientLayerStyleGuard` 入场快照、挂为布局子对象，布局析构即还原（建不出也还原） | `src/qgis/layoutexport.cpp` | `tst_mapping` horizonMapLayoutRestoresLiveLayerStyle |

## 未做/不做（如实登记）

- Open Issues #460–#485 全数未触碰（含本域内被指纹占用的 hunks：
  `releasestore.cpp:25/:28-47`、`qgisprojectservice_async`、`mappingartifactwriter`、
  `previewmapcanvas`、`paleodecorations.cpp:56` setDark/gDark 等）。
- Open PR #486/#487 文件零交集：`git diff` name-only ∩ 两 PR 文件 = ∅。
- S4 审计的 `attach_mapping.cpp`/`appcontext.cpp`/`attach_shell.cpp` 线索在
  只读域，未动；S2 的证据面板候选（review-combo 同步/陈旧表单字段/多选移除/
  lineage 隐藏）超出本轮容量，登记不实施。
- S5 复审两个 MINOR 留作已知限制（如实登记，本轮不修）：
  1. `ensureSharedDataVisible` 愈合写回时用 `createThemeFromCurrentState` 定格
     整树——未声明的临时图层会被一并写进 `page:*` 主题记录（记录膨胀 + 其可见
     性随主题固化）。applyPrunedTheme 的重放路径仍守恒其活体状态，影响有界；
     彻底修法（按声明集剪枝记录）触碰主题记录语义，留给独立改动。
  2. `MapBookQueue` 出册失败时 manifest 的逐页状态仍写 `"ok"`（页确已渲染，
     仅目录发布失败），与 `result.failed=total` 的顶栏计数口径并存读感违和；
     字段语义本身如实，不改。
- 不等待在线 CI；不合并 PR。

## 轮次日志

- R0（2026-10-16）：基线 `dbb03269` 冻结；排除矩阵成文；worktree + 独立 build 建立；
  vendored prefix 符号链接接入。
- R1：configure/定向构建经共享锁排队完成；冷构建大量 vendored 重编译。
- R2–R3：S1–S4 四路审计（layer/compose+layout/workflow+release/facies+evidence）
  汇合 10 个可验证单元全部落地（见上表），test-first 实施。
- R4：定向 11 测试全绿（11/11），非编译门禁全过；验证期修复上记 2 处自引入回归。
- R5：S5 独立 review 全 diff——1 MAJOR + 4 MINOR 修复（U10 补拖拽/挪组同步、
  TransientLayerStyleGuard 标注快照泄漏、因子清单与工作流门禁同口径、
  首建主题先愈合共享数据层、releases() 标 manifestCorrupt + 面板显示「损坏」）、
  2 MINOR（主题记录含临时层、册 manifest 逐页 ok 口径）、1 NIT 如实登记于上节；
  复审后定向 11 测试双跑全绿（11/11 ×2），门禁复跑全过。
