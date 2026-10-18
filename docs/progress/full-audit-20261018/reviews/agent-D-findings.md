# Agent D 审查报告 — src/qgis + src/ui（生命周期/状态/交互/QGIS 边界/DESIGN 合规）

- 基线：366aef97（本 worktree 只读审查，未修改任何源码/测试/构建文件，未运行 cmake/编译/ctest）。
- 审查日期：2026-10-18。
- 排除依据：`01-conflict-matrix.md` + `99-external-issues-snapshot.txt`（44 个开放 issue 全文已通读）；
  PR #518 / PR #521 触碰文件清单经 `gh pr view --json files` 核实（见 §3）。
- 方法：先全目录高风险模式扫描（QTimer/singleShot/connect-lambda-无接收者/裸 new/deleteLater/QPointer），
  再按风险分级逐行深审主壳非排除 TU 与 src/qgis 核心，其余抽查。**src/ui 115k 行未逐行全读**，
  覆盖方式逐文件如实标注于 §1；未覆盖区域见 §4。

---

## 1. 覆盖清单

方式图例：逐行 = 全文通读；重点函数 = 指定区域逐行 + 其余接口扫；接口 = 头文件+调用面扫+模式 grep。

### 1.1 src/qgis（90 文件，19.5k 行）

| 文件 | 方式 | 风险级 | 关键类 | 调用链 | 测试对应 |
|---|---|---|---|---|---|
| qgisconstrainteditsession.cpp/.h | 逐行 | 高 | QgisConstraintEditSession | beginEdit→initialize→editCommandEnded→synchronize→persist/finish | tst_constraint_draw / tst_singlefactor_constraintediting |
| qgiseditingservice.cpp | 逐行 | 高 | QgisEditingService | 属性表/编辑工具条→begin/commit/rollbackEdit→PaleoProjectStore busy | tst_edittools / tst_constraintediting |
| qgiscanvascontroller.cpp | 逐行 | 高 | QgisCanvasController | 主壳画布创建/CRS 变换/setExtent/广播去抖 | tst_ui / tst_layerplatform |
| previewmapcanvas.cpp/.h | 逐行 | 高 | PreviewMapCanvas | datapreview 各 tab / mappingreferencecanvas | tst_datapreview* / tst_mappingpages |
| previewmaptools.cpp/.h | 逐行 | 高 | PreviewMeasureTool/Identify/Profile/ToolManager | previewmappage 工具条 | tst_datapreview_map（工具族） |
| qgislayerservice.cpp | 逐行 | 高 | QgisLayerService | declare/instantiate/releaseHorizon/setActiveHorizon | tst_layerplatform / tst_mapping |
| qgislayerorganizer.cpp | 逐行 | 中 | QgisLayerOrganizer | legendLayersAdded→placeLayer→树序/customLayerOrder | tst_layerorganizer |
| qgislayerprofile.cpp | 重点函数(前 120 行+applyPageProfile 族) | 中 | QgisLayerProfileService | showPage→applyPageProfile→QgsMapThemeCollection | tst_mappingpages / tst_layerplatform |
| qgisprojectservice_async.cpp | 逐行 | 高 | QgisProjectService(async) | openProjectAsync→worker→adoptBackgroundOpen 四拍 | tst_background_open（#483 P2 既有） |
| evidencemapadapter.cpp | 逐行 | 高 | EvidenceMapAdapter | 证据面板→bindLayer→showFeature/spatialCandidates | tst_evidenceselection / tst_evidencee2e |
| mapcanvaslink.cpp | 逐行 | 中 | MapCanvasLink | 主/参考画布联动 | tst_mappingpages（参考视图） |
| mappingreferencecanvas.cpp | 逐行 | 中 | MappingReferenceCanvas | 编图参考证据面板 | tst_mappingpages |
| draftautosave.cpp | 逐行 | 中 | DraftAutosave | 属性表编辑→editCommandEnded→flush/undo | tst_edittools（autosave 族） |
| wellpointnavigation.cpp | 逐行 | 中 | Navigation(filter) | 预览井点单击→pick→openWell | tst_datapreview_map |
| nativeattributetablehost.cpp | 逐行 | 中 | PaleoAttributeTableHost | layertreepanel→showAttributeTable | tst_ui（attribute table） |
| taskpanel 侧消费 + qgisstyleservice.cpp | 逐行 | 中 | QgisStyleService | apply*Style/wellFaciesRenderer | tst_services2（#487 分组既有） |
| layervocabulary.h / constraintcrs.h | 接口 | 低 | 词表/CRS helpers | — | tst_constraintcrs |
| faciesmaplegend / factorstylewriter / geopatterns / semanticlegend / qgislabelzorder / topologicalindex / manifestprojection / crossplotmaplink / previewidentify / previewrendercache / qgislayoutservice / seismicsectiontool / wellsectionmapband / mappingfeatureidentity / facieshierarchyrenderer / standardelements / previewrasteranalysis / composeattribution / wellattributestore / qgisprocessingservice / mappingartifactwriter / mapbooklayout / layoutexport | 接口+模式 grep（composeattribution 的 fromWkb 疑点已对 vendored 头核实为 void 成员、非 bug） | 中-低 | — | — | 各域 tst |
| dataexportwriter.*/geologicalmaplayout.*（主仓未提交新文件，本树不存在） | 未审 | — | — | EXCLUDED_ACTIVE_WORK | — |

### 1.2 src/ui（400 文件，115k 行）— 主壳与核心页

| 文件 | 方式 | 风险级 | 关键类 | 调用链 | 测试对应 |
|---|---|---|---|---|---|
| paleomainwindow.cpp | 逐行 | 高 | PaleoMainWindow 本体 | 构造/析构/flashHorizon/closeEvent/maybeSaveProject/syncSeismicVolumeToDocks/resetProjectScopedState | tst_ui / tst_panels（⚠ 文件在 PR #518 触碰清单） |
| paleomainwindow_pages.cpp | 逐行 | 高 | showPage/syncBottomDockForTasks | 页切换/dock 策略/correlation 进出 | tst_panels（mainWindow_*） |
| paleomainwindow_docks.cpp | 逐行 | 高 | buildCenterArea/buildLayerTreeDock/buildPageDocks | 壳构建/启动页/web dock | tst_ui |
| paleomainwindow.h（PaleoDockWidget 意图模型） | 逐行 | 高 | PaleoDockWidget | setProgrammaticVisible/userWantsVisible | tst_panels |
| paleomainwindow_wellsection.cpp | 接口+模式 grep | 中 | — | — | tst_wellsection_ui |
| paleomainwindow_project/_status/_errorhub/_faults/_references/_shortcuts/_ribbon/_sections | 接口+模式 grep | 低-中 | — | — | tst_panels |
| paleomainwindow_attach_compose/_mapping/_models/_python/_validate | 接口+模式 grep | 中 | — | — | tst_mappingpages 等 |
| paleomainwindow_attach.cpp/_attach_constraint/_attach_data/_attach_predict/_attach_shell/_workbench*/ribbonpanels | 排除（PR #521/#518）| — | — | — | — |
| pages/validatepage.cpp | 逐行 | 高 | ValidatePage | run→validationFinished→populateResult/clearResults | tst_panels（validatePage_*） |
| pages/constraintpage_generate.cpp | 重点函数(前 120 行+busy 状态机) | 高 | ConstraintPage 生成域 | generate→workflow→busy 门控 | tst_mappingpages（constraintPage_*） |
| pages/predictpage.cpp | 重点函数(setRunBusy 族) | 高 | PredictPage | busy 门控/取消可见性 | tst_panels（predictPage_*） |
| pages/mappingworkbenchpage.cpp | 重点函数(ctor 1-680 逐段+模式 grep 全文) | 高 | MappingWorkbenchPage | issue()/refreshTimer/证据面板 | tst_mappingworkbench / tst_mappingpages |
| pages/datagraphpage.cpp / entitypanel.cpp / composepage.cpp / datapage.cpp / mappingreferencepanel.cpp / wellpredictionpanel.cpp | 模式 grep+关键区抽读 | 中 | — | — | tst_datagraphpage / tst_panels |
| pages/datalist*（含 _undo/_tree/_dnd 等） | 排除（PR #518）| — | — | — | — |
| horizonchipbar.cpp | 逐行 | 高 | HorizonChipBar | chip 点击→setActiveHorizon/availability | tst_panels / tst_ui |
| evolution/evolutionplayerpanel.cpp | 逐行 | 高 | EvolutionPlayerPanel | 播放/步进→switchHorizon | （无专测——见 FD-001） |
| notifications/toastcard.cpp / paleonotify.cpp / notificationcenter.cpp | 逐行 | 高 | Tier1/2 呈现栈 | ErrorHub→onRaised→卡/模态/状态栏 | tst_notifications 族 |
| notifications/errorhistorypanel.cpp | 接口 | 低 | Tier3 | — | tst_notifications |
| taskpanel.cpp | 逐行 | 中 | TaskPanel | tasksChanged 去抖+500ms 轮询 | tst_panels |
| paleodockmanager.cpp | 逐行 | 高 | PaleoDockManager | 拖放引导/布局保存恢复/意图钩子 | tst_panels（folderConfirm_* 邻域） |
| correlation/horizonmarkers.cpp | 逐行 | 高 | MarkerLineItem/HorizonMarkerSet | 拖拽→0-timer deferred commit→rebuild 保留 grabber | tst_correlation* |
| welltops/welltopseditordialog.cpp | 重点函数(buildUi/undo 族) | 中 | WellTopsEditorDialog | 井分层编辑/批量/版本回滚 | tst_welltops |
| dialogs/（gridding/depthanchor/folderconfirm/cataloghealth/storagegovernance/projectmapsettings） | 模式 grep+关键区抽读 | 中 | — | — | tst_import / tst_panels |
| layers/layertreepanel.cpp、correlationpanel.cpp、sequenceframeworkpanel*.cpp、layoutdesignershell.cpp、webviewpanel.cpp、attributetablepanel.cpp、batchjobpanel.cpp、paleotheme/paleoicons/paleoribbon/paleoemptystate、seismic3d/、seismicsection/、wellsection/、wellcomposite/、datapreview/、ai/、crossplot/、realization/、propertymodel/、python/、locator/、shortcuts/、symbols/、help/、faciesmapping/、faults/、decorations/、maptools/、edittools/、layout/ | 模式 grep 抽查（多数有既有 issue 或 PR 覆盖，见 §3/§4） | 低-中 | — | — | — |

---

## 2. Findings（新问题）

### FD-001 演化动览帧表不随工程层位词表重载，步进会把 activeHorizon 切到集合外层位

- **Severity**: P2（功能失效 + 全局选择上下文污染；触发面限于自定义层位词表工程）
- **Confidence**: High（代码路径完整核实，无任何重载/刷新通路）
- **Module**: src/ui
- **File:Line/Function**: `src/ui/evolution/evolutionplayerpanel.cpp:19`（ctor `m_frames( mappingHorizons() )`）；`evolutionplayerpanel.h:55`（`m_frames` 无 setter/reload）；对照 `src/ui/paleomainwindow.cpp:489-490`（onProjectOpened 只调 `HorizonChipBar::reloadHorizons()`）；`src/domain/mappinghorizons.h:18`（`mappingHorizons()` = `AreaRules::active().sequenceBoundaries`，随 `AreaRules::setProjectDir` 在工程打开时装载，`src/app/appcontext.cpp:707`）。
- **Root Cause**: 播放器帧表在主窗口构造时快照内置默认词表（C3/C6/D53…），工程打开后 AreaRules 换成 project_area.json 的 sequence_boundaries 时，chip 条有 reloadHorizons 重灌，演化动览被遗漏——同类「工程级参数驱动的层位 UI」收口漏了一个组件。
- **Trigger（用户操作序列）**: ① 打开一个 project_area.json 覆盖了 sequence_boundaries（与内置 8 层不同）的工程；② 看 chip 条已按新词表重建；③ 点演化动览「下一期▶」（或「播放」自动步进）。
- **Observed/Expected**: 观察——`stepBy` 按陈旧 m_frames 取下一层位 → `switchHorizon` 把 `SelectionContext::setActiveHorizon` 与 `QgisLayerService::setActiveHorizon` 设为**本工程不存在的层位**：`releaseHorizon(otherHorizons)` 清地图层、`instantiateHorizon(外来源)` 无声明白干、chip 条 `applyActive` 全灭；工程真实层位切换时 `applyFrame` 因 indexOf<0 不更新帧标签（标签停留在陈旧层位）。预期——帧表随工程词表重载，步进只在本工程层位集合内。
- **Impact**: 演化动览在自定义层位工程中完全错步；更广的影响是 activeHorizon 被 污染后，编图链三页（predict/constraint/compose）消费 `SelectionContext::activeHorizon` 的取数/门控带外层位值，直到用户手动点合法 chip 才恢复。
- **Existing Issue Match**: 无（#481/#482/#483 未提及；非 #490-4 地震缝同族）。
- **Active PR Match**: 无（evolutionplayerpanel 不在 PR #518/#521 清单）。
- **Fix Strategy（最小根因修复）**: 给 EvolutionPlayerPanel 加 `reloadFrames()`（`m_frames = mappingHorizons(); applyFrame(m_selection->activeHorizon());`），在 `PaleoMainWindow::onProjectOpened` 与 `bar->reloadHorizons()` 同一位置调用。
- **Regression Test 思路**: 构造 panel（内置词表）→ `AreaRules::setProjectDir`（fixture 用两层的自定义词表）→ 调 reload → 断言 `frameOrder()==新词表` 且 `currentHorizon()==activeHorizon`；旧代码下 `frameOrder()` 仍为内置 8 层（红）。

### FD-002 预览量测工具无 deactivate() 清理——切换工具后量测橡皮带残留画布

- **Severity**: P3（交互残留，读数误导）
- **Confidence**: High（同文件三个工具对照，行为差异明确）
- **Module**: src/qgis
- **File:Line/Function**: `src/qgis/previewmaptools.cpp:102-121`（PreviewMeasureTool ctor/dtor，无 deactivate override）；对照同文件 `:311-317`（PreviewIdentifyTool::deactivate 隐藏 m_rectBand）、`:403-408`（PreviewProfileTool::deactivate 隐藏 m_activeBand）；`previewmaptools.h:103/:134` 仅有后两者的声明。
- **Root Cause**: QgsMapTool 基类 deactivate 不回收橡皮带；identify/profile 都补了 deactivate 隐藏，measure 漏了（其析构注释只处理 scene 先亡场景）。
- **Trigger**: ① 数据预览打开地图资产；② 工具条激活「距离测量/面积测量」；③ 点两个及以上点（虚线带+读数出现）；④ 不按 Esc，直接点工具条「漫游」。
- **Observed/Expected**: 观察——量测虚线带与已落点残留在预览画布上，漫游/缩放中持续叠加显示；重新激活测量工具旧测量还在（m_finished/m_points 保留）。预期——与 identify/profile 一致：deactivate 时隐藏 band（Esc 已有 clear 路径，切工具至少应隐藏图形）。
- **Impact**: 陈旧测量图形被当成常驻标注；与同族工具的清理契约不一致。
- **Existing Issue Match**: 无（#482 是 seismicsection 画布；#483 是 qgis/app 清单未含此项）。
- **Active PR Match**: 修复落点 `previewmaptools.cpp` 不在任何 PR；消费方 `previewmappage.cpp` 在 PR #518 触碰清单（边界：修 qgis 侧不冲突，建议 PR #518 合并后再动消费方文案，若有的话）。
- **Fix Strategy**: `void PreviewMeasureTool::deactivate() override { m_band->hide(); QgsMapTool::deactivate(); }`（保留 m_points，重激活可继续测或再 Esc 清）。
- **Regression Test 思路**: tst 里 activate(measure)→canvasPressEvent 落两点→activate(pan)→断言量测 band `!isVisible()`（旧代码红）。

### FD-003 PreviewMapCanvas::setLayers 置换不清理三层记账——悬垂指针键永久留存，地址复用可继承陈旧可见性/透明度

- **Severity**: P3（低概率可见性错乱 + 无界小泄漏）
- **Confidence**: Medium-High（代码路径确证；地址复用触发概率依赖分配器行为）
- **Module**: src/qgis
- **File:Line/Function**: `src/qgis/previewmapcanvas.cpp:106-122`（setLayers：`m_layers = layers` 后只为新层补默认值，不清旧层条目）、`:178-191`（forgetLayer：`!m_layers.contains(dead)` 早退——被 setLayers 置换出去的层销毁时三层哈希不清）、`:114-119`（`if (!m_visible.contains(l))` 守卫使同地址新层继承陈旧值）。消费方：`src/ui/datapreview/previewmappage.cpp:106`（syncLayers）、`:376`（layerOrderChanged→setLayers(order)）、`:664`（clearMapLayers→setLayers({})）。
- **Root Cause**: removeLayer 有完整的记账摘除，setLayers 整表置换走的是另一条路径且只加不减；destroyed 钩子的防重入早退把「置换出去的活层」的后续销毁也挡在清理之外。
- **Trigger**: ① 数据预览反复切换/重排/清空地图资产层（setLayers 换表、setLayers({})）；② 某层曾被 `setLayerVisible(l,false)` 后被置换并销毁；③ 分配器把新 QgsMapLayer 复用到同一地址并再次进入 setLayers。
- **Observed/Expected**: 观察——m_visible/m_opacity/m_blend 以裸指针为键累积陈旧条目（长会话无界增长）；命中地址复用时新层 `m_visible.contains(l)` 为真、跳过默认 true，直接继承旧层的 false/旧透明度（TOC 显示可见、画布不画或透明度错）。预期——置换出的层记账随之清除，新层总是默认可见/1.0。
- **Impact**: 长预览会话哈希增长（每层 3 条目永不释放）；可见性/透明度偶发串层难排查。
- **Existing Issue Match**: 相邻——#483 P3「previewmapcanvas 裸 QgsMapLayer* 键持三层状态无悬垂防护」记录的是键无防护、靠消费方克隆层兜底；本条是 setLayers 置换路径的**独立缺口**（即便消费方全用长命克隆层，置换泄漏仍在），修复时一并收口更彻底，注明依赖边界。
- **Active PR Match**: previewmapcanvas.cpp 不在 PR 清单；previewmappage.cpp 在 PR #518（触发面重合，修复落点在 qgis 侧独立可隔离）。
- **Fix Strategy**: setLayers 内对「旧 m_layers − 新集合」逐个走 removeLayer 的记账摘除（复用现有语义）；或 forgetLayer 改为无条件摘三层记账（destroyed 后以 QObject* 值为键删除安全）。
- **Regression Test 思路**: setLayers({A})→setLayerVisible(A,false)→setLayers({B})→delete A→new C 复用同地址（测试里以同地址手工构造困难，可改为断言内部计数/或暴露 layerStateCount() 供 tst 断言 setLayers 后不残留旧键——旧代码红）。

### FD-004 属性表宿主的破坏性确认走裸 QMessageBox::question，绕过 PaleoNotify Tier-2 收敛契约

- **Severity**: P3（DESIGN.md 合规缺口 + 呈现契约旁路）
- **Confidence**: High
- **Module**: src/qgis（呈现缺口横跨 ui 契约）
- **File:Line/Function**: `src/qgis/nativeattributetablehost.cpp:94-110`（stopEditing：`QMessageBox::question(m_parent, tr("保存编辑"), … Save|Discard|Cancel)`）。
- **Root Cause**: DESIGN.md「错误呈现」Tier-2 明文「全局统一走 `ui/notifications/paleonotify.h` 的 `ask`/`askSaveDiscard` 收口，消除散弹式裸 QMessageBox」；#481 P2 已记 `paleomainwindow_attach_shell.cpp:719` 同族一处，此处是独立的第二处。qgis 封装层按分层不能 include `ui/`，导致该收口在此天然缺口。
- **Trigger**: ① 图层树右键打开 QGIS 原生属性表；② 编辑若干单元格；③ 切换编辑状态/关闭属性表触发 stopEditing。
- **Observed/Expected**: 观察——裸模态三选一直接弹出，不经 PaleoNotify（无统一文案/按钮语义、无 ErrorHub 入账路径，与主壳 maybeSaveProject 的 askSaveDiscard 呈现不一致）。预期——破坏性确认统一 Tier-2 收敛。
- **Impact**: 散弹式模态回归面（收敛契约的既有承诺被穿透）；两处「保存编辑？」交互外观/语义不统一。
- **Existing Issue Match**: 同族不同实例——#481 P2 只点名 attach_shell.cpp:719；本处独立可报。
- **Active PR Match**: 无（nativeattributetablehost 不在 PR 清单）。
- **Fix Strategy**: 仿 `PaleoMainWindow::m_projectSaveAsk` 范式给 host 注入 ask seam（`std::function<SaveChoice(...)>`，缺省回落现有裸框），组装根/视图层接 `PaleoNotify::askSaveDiscard`；offscreen 分支保持现有直存语义。
- **Regression Test 思路**: 注入 seam 的无头用例断言走 seam 且 Save/Discard/Cancel 三分支行为与旧路径一致；未注入时行为逐字不变（降级契约，同 paleonotify 无 hub 回落）。

### FD-005（记录级，不建独立工单的观察）多地震体自动装载链在「启动一体返回」后无自驱续扫

- **Severity**: P3（观察；主问题已被 #481 P3-3 记录）
- **Confidence**: High（代码事实）/ 触发依赖用户后续操作
- 位置：`src/ui/paleomainwindow.cpp:1013`（syncSeismicVolumeToDocks 启动一次装载后 `return`）。装载完成回调不重触发 sync；余体要等下一次 catalog changed/ribbon 动作才扫。#481 P3-3 已把该函数的两处 return 吞体记为既有问题（EXCLUDED_EXISTING_ISSUE），此处仅补充「完成回调不续扫」使弱化变体仍存在——修复 #481 时在完成回调尾部补一次 `syncSeismicVolumeToDocks()` 即一并闭合。paleomainwindow.cpp 在 PR #518 清单（EXCLUDED_ACTIVE_WORK 边界）。

---

## 3. 排除项汇总

核实方式：`gh pr view 518/521 --json files`（2026-10-18）+ 00-baseline.md 未提交簇清单。

- **PR #521（codex/facies-topology-editing）触碰、未深审**：src/qgis/facieshierarchyrenderer.cpp、faciespartitioneditor.*、faciespartitiontool.*、mappingfeatureidentity.cpp；src/ui/edittools/**（editingtoolbar*、vertexeditortools、editingtools、editingundostack）；paleomainwindow_attach_shell.cpp、paleomainwindow_workbench.cpp、ribbonpanels.cpp。→ edittools 全目录与主壳 workbench/attach_shell 域 = EXCLUDED_ACTIVE_WORK（仅模式 grep 级抽查）。
- **PR #518（goal/issue-sweep-20261017）触碰、未深审**：src/ui/datapreview/previewmappage.cpp；pages/datalist.*、datalist_undo.cpp、dataops/dataopsmodel.h；paleomainwindow_attach.cpp、_attach_constraint.cpp、_attach_data.cpp、paleomainwindow.cpp/.h（⚠ 本体也在清单——FD 涉及 paleomainwindow.cpp 者已注明边界：FD-001 修复落点在 evolutionplayerpanel+onProjectOpened 一行，FD-005 记录级）、_workbench_commands/_evidence/_wells.cpp；seismic3d/seismic3dviewpanel.*；wellcomposite/compositebody.cpp、wellcompositestore.cpp。
- **主仓未提交导出簇（EXCLUDED_ACTIVE_WORK）**：src/ui/layout/**（layoutexportactions/layoutitem*/layouttemplates/layoutundostack/mapbookcontroller/mapbookpanel）、src/ui/dialogs/dataexportdialog.*、geologicalexportdialog.*、src/qgis/layoutexport.cpp、mapbooklayout.cpp、（dataexportwriter/geologicalmaplayout 在本树不存在——主仓未提交新文件）。仅模式 grep 级。
- **既有 issue 命中（EXCLUDED_EXISTING_ISSUE，本报告不重复展开）**：#463（seismic3d 书签）、#464（busy-token）、#465（saveEditingVersion）、#473（attachWorkbench 巨函数）、#481（主壳 P2×5 P3×6：removeRecursively/裸 QMessageBox@attach_shell/paleodecorations setDark 死接线/导入 tooltip 互踢/wellsection store 视图直建/seismic 双 return·进度条连接叠代）、#482（wiggle 纵向无裁剪/hover 全幅 update/死三元/datalist static_cast/鹰眼中键/compositebody 旗宽）、#483（async 工程 worker 单例并存/previewmapcanvas 裸键/restoreRasterCrs O(层数) GDALOpen 等）、#487（图例分组）、#488/#496（singlefactor 约束族）、#502（ui 规模失衡）。

## 4. 未覆盖区域（如实）

- **src/ui 逐行未覆盖**（115k 行总量限制；已做模式 grep + 关键区抽读，但**不称逐行**）：
  - seismic3d/（seismic3dviewpanel 1836 行——PR #518 排除；seismic3dviewportwidget/seismicslicerenderer 仅 grep）；
  - seismicsection/（dock 族 ~8 文件；#482/#490 已覆盖其渲染/交互主缺陷面，本轮仅 grep 未逐行核对 dock_track/curtain 全部状态机）；
  - wellsection/（wellsectionpanel 1541 行——#481 P2-5 已记其视图直建 store；fencewidget/wellsectiondialogs/scene 仅 grep）；
  - wellcomposite/（PR #518 排除：compositebody/wellcompositestore；curveconfigdialog/wellcompositecanvas 等仅 grep）；
  - correlation/（correlationpanel 994 行仅 grep——horizonmarkers 除外已逐行）、crossplot/、realization/、propertymodel/、ai/、python/、locator/、shortcuts/、symbols/、help/、faciesmapping/、faults/、decorations/、maptools/、datapreview/ 其余 tab 文件（datapreviewtabseismic 890 行仅 grep）；
  - entitypanel.cpp（1620 行）读了 CRUD/对话框/拓扑三段（~250 行），其余 grep；
  - mappingworkbenchpage.cpp（1638 行）逐段读 ctor+predict 段约 680 行，compose/evidence 段以 grep 为主；
  - layoutdesignershell.cpp、webviewpanel.cpp、attributetablepanel.cpp、batchjobpanel.cpp、correlationpanel.cpp、sequenceframeworkpanel*.cpp 仅 grep。
- **src/qgis 逐行未覆盖**：composeattribution.cpp（899）、wellattributestore.cpp（895）、qgisprocessingservice.cpp（784）、mappingartifactwriter.cpp（753）、previewrasteranalysis.cpp（540）、standardelements.cpp（520）、factorcontour.cpp（567）、geopatterns.cpp（424）、qgisprojectservice.cpp（407，读 async 半）等大文件为接口+模式级（其已知问题 #483 已在排除清单）。
- 未运行任何构建/测试（只读约束）；所有 Regression Test 思路均为设计建议，未验证红绿。
