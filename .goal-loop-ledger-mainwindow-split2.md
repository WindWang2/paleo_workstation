# 方向 83 ledger：paleomainwindow 壳层收尾（本体二次拆分 + 装配根审视）

- 基线：origin/master `f31ee461`（worktree `.worktrees/mainwindow-split2`，分支 `goal/mainwindow-split2-20261009`）
- 任务书行号口径为 `e3c8d31d`（本体 2,137）；本基线本体实测 **2,234 行**（后续 4 提交又涨 ~97）。
- 本 ledger 为工作文档，不随 PR 提交。

## R0 结构勘察

### 本体段位图（paleomainwindow.cpp 2,234 行）

| 段 | 行区间 | ~行 | 域 |
|---|---|---|---|
| includes | 1-152 | 152 | 共享 |
| 匿名 ns 助手 | 158-299 | 142 | pageLabels/pageDockTitles/ribbonWindowStyle/kEditingToolPages/readRecentProjects/writeRecentProjects/makeStartupPage |
| ctor + dtor | 301-404 | 104 | 生命周期 |
| applyCurrentPageProfile/addDockWidget | 406-417 | 12 | 杂项 |
| **buildShell** | 419-1042 | **624** | 单函数巨石：中央面(425-632)/左dock层树(634-716)/右dock占位(718-733)/底dock(735-751)/地震剖面dock(753-785)/连井dock(787-795)/错误历史dock(797-809)/3D dock(811-818)/Web dock(820-877)/buildRibbon调用(879-881)/状态栏族(883-958)/错误状态钮(960-975)/工程打开进度(976-1011)/ErrorHub同步(1013-1029)/主题chrome(1031-1041) |
| 主题三件 | 1044-1077 | 34 | reapplyThemeChrome/setCompactDensity/setDarkTheme |
| **buildRibbon** | 1079-1252 | 174 | 骨架+页签+**快捷键注册(1105-1131)**+文件菜单+右侧按钮组 |
| categoryForPage/showPanelMenu/contextMenuEvent/errorHistoryAction | 1254-1316 | 63 | ribbon/面板菜单 |
| restoreCorrelationDocks/showPage | 1318-1444 | 127 | 页切换 |
| 文件夹导入族 | 1446-1532 | 87 | runFolderImport(At)/folderImportWorkflow/stampSourceArea |
| flashHorizonLayer | 1534-1605 | 72 | T29 闪烁定位 |
| 工程打开族 | 1607-1743 | 137 | projectOpenWorkflow/openPath/showStartup/onProjectOpened |
| closeEvent | 1745-1758 | 14 | 生命周期 |
| updateWindowTitle/setProjectReadOnly | 1760-1797 | 38 | 标题/只读 |
| syncBottomDockForTasks | 1799-1833 | 35 | 底栏任务 |
| save/restoreWindowState | 1835-1855 | 21 | 窗口状态 |
| save/restoreCanvasExtent | 1857-1903 | 47 | 画布范围 |
| applyProvisionalRegistration | 1908-1947 | 40 | D11 临时配准 |
| resetProjectScopedState | 1949-2008 | 60 | 工程边界清理 |
| resolveDirtyLayerEdits/maybeSaveProject | 2010-2105 | 96 | C1/#282 保存流 |
| refreshCorrelationWells | 2107-2129 | 23 | 井集 |
| syncSeismicVolumeToDocks | 2131-2234 | 104 | 地震体装载 |

### 家族全景（16 文件 8,079 行 + ribbonpanels.cpp 同族）

| 文件 | 行 | 内容 | R0 裁决 |
|---|---|---|---|
| paleomainwindow.cpp | 2,234 | 本体 | **本次拆**（见切分线） |
| _workbench.cpp | 850 | attachWorkbench（编图工作台） | 留；≤900 达标 |
| _attach.cpp | 770 | attachWorkflows 编排 + reveal/realization/pin | 留 |
| _attach_constraint.cpp | 704 | 约束/单因素页 | 留 |
| _attach_shell.cpp | 668 | attachShellSurfaces(locator/保存/底栏/处理/编辑/图件) + attachAiAssistant + **attachErrorHub + showErrorHistory(648-668)** | attachErrorHub/showErrorHistory **移入新 _errorhub TU**（ErrorHub 域归拢，方向64 装配面独立） |
| _attach_models.cpp | 642 | 深度/相图/属性建模/3D 属性体 | 留 |
| _attach_mapping.cpp | 388 | 编图流水线/版本 | 留 |
| _wellsection.cpp | 385 | 连井剖面 | 留 |
| _attach_compose.cpp | 343 | 综合编图页 | 留 |
| _attach_data.cpp | 323 | 数据管理页 | 留 |
| _sections.cpp | 310 | 剖面 dock | 留 |
| _attach_validate.cpp | 206 | 验证/布井 | 留 |
| _attach_predict.cpp | 193 | 智能预测页 | 留 |
| _attach_python.cpp | 36 | Python 面 | 留（小 TU 先例） |
| _faults.cpp | 27 | 断层 | 留（小 TU 先例） |
| ribbonpanels.cpp | ~500 | buildRibbonPanels/addEditingPanel | 留（已独立） |

裁决依据：attach 族方向 56 已按页域拆好，各 ≤770 且 ≤900；不再动（除 attachErrorHub/showErrorHistory 归域移动）。workbench 850 为家族最大，oracle「家族最大不超本体」要求本体 >850——定案本体目标 ~950-1,100。

### 切分线定案（新 7 TU + 本体收缩）

新内部契约头 `paleomainwindow_internal.h`（方向 65 先例）：跨 TU 共享的文件域助手
- `pageLabels()` / `pageDockTitles()`（docks/ribbon/pages 三 TU 用）
- `readRecentProjects()` / `writeRecentProjects()`（docks[makeStartupPage]/project[onProjectOpened] 用）
- 单 TU 用的助手留各 TU 匿名 ns（ribbonWindowStyle→本体；kEditingToolPages→_pages；makeStartupPage→_docks TU 局部）

头文件私有区新增成员函数声明（**公共区零变更**，私有 build 助手先例：buildRibbon/buildRibbonPanels/addEditingPanel 已在）：
- `buildCenterArea()` / `buildLayerTreeDock()` / `buildPageDocks()` / `buildStatusBar()` —— buildShell 六段按原文顺序分解，buildShell 变纯编排（初始化序逐行保持）
- `buildErrorHistoryDock()` / `wireErrorHubStatus()` —— ErrorHub 呈现域（任务书：ErrorHub 自成域）
- `registerPageShortcuts()` —— Ctrl+1..6/Ctrl+Tab 注册块（任务书：快捷键注册自成域，方向63 装配面）

| 新 TU | 内容（原行区间） | 预估行 |
|---|---|---|
| _ribbon.cpp | buildRibbon(去快捷键块) + categoryForPage + showPanelMenu + contextMenuEvent (1079-1104,1132-1252,1254-1311) | ~340 |
| _shortcuts.cpp | registerPageShortcuts (1105-1131) | ~60 |
| _docks.cpp | buildCenterArea + buildLayerTreeDock + buildPageDocks + makeStartupPage(TU 局部) (425-877 拆三函数) | ~640 |
| _status.cpp | buildStatusBar (883-958 + 976-1011，错误钮段让位 wireErrorHubStatus) | ~250 |
| _errorhub.cpp | buildErrorHistoryDock(797-809) + wireErrorHubStatus(960-975,1013-1029) + errorHistoryAction(1313-1316) + attachErrorHub/showErrorHubHistory(自 attach_shell 移入) | ~180 |
| _pages.cpp | restoreCorrelationDocks + showPage + syncBottomDockForTasks (1318-1444,1799-1833) | ~290 |
| _project.cpp | 文件夹导入族 + projectOpenWorkflow/openPath/onProjectOpened/refreshCorrelationWells (1446-1532,1607-1647,1661-1743,2107-2129) | ~400 |
| **本体保留** | includes(裁剪) + ctor/dtor/closeEvent + buildShell 编排 + 主题三件 + applyCurrentPageProfile/addDockWidget + updateWindowTitle/setProjectReadOnly + showStartup + save/restoreWindowState + save/restoreCanvasExtent + maybeSaveProject/resolveDirtyLayerEdits + resetProjectScopedState + flashHorizonLayer + applyProvisionalRegistration + syncSeismicVolumeToDocks | **~1,000** |

### 信号序/初始化序保持策略

- buildShell 六段调用序 = 原文行序；段内零重排。
- mapEmpty 局部变量跨段（buildCenterArea 建、空态同步接在 buildLayerTreeDock 内）：按文件自身规约（头注释「cross-widget lookups via objectName」）——但为保序把空态同步块(704-716)留在 buildCenterArea 尾部不行（左 dock 建(634-702)在前）——定案：空态同步块随 buildLayerTreeDock 尾部（原行序 634→716 不变，仅函数边界落在 716 后）。mapEmpty 经 findChild("mapEmptyState") 取回（objectName 唯一，tst_ui 依赖同名）。
- startup 局部指针（579-630 按钮 connect）：留在 buildCenterArea 内（原行序）。
- buildRibbon 内快捷键块抽出后调用点在原位（1104 之后、1132 之前）。
- 状态栏错误钮(960-975)与 ErrorHub 同步(1013-1029)原为被 976-1011 工程进度段隔开的两段：wireErrorHubStatus 合并两段后，工程进度段在其后执行——**连接序变化**：错误钮 clicked connect 原在第 968 行（进度段前），ErrorHub historyChanged 在 1026（后）；合并后两 connect 相邻、进度段 connect 全部后移。涉及对象互不相同（QToolButton/ErrorHub vs QgisProjectService 信号），无同对象同信号序依赖，行为等价（记为 Low 入 PR body）。

### 公共 API 冻结证据

- 快照：`.split-audit/public-api-baseline.txt`（class 声明至 `protected:` 前，157 行）
- 消费方（rg 证据，22 文件）：src/app/{appcontext,main,crossplotcontroller}、src/ui/{datapreviewtabs.h,whatsthiscatalog,shortcutcatalog,paleotheme.h}、tests 13 个（tst_panels/tst_shortcuts_shell/tst_ui/tst_shell_projectlifecycle/tst_stratigraphicweb/tst_webviewpanel/tst_sectionlifecycle/tst_composeworkflow/tst_crossplot_controller/tst_derivationgraph/tst_jobrunner/tst_petrophys_assembly/tst_predict_assembly）——全部只走公共区，预期零改动。

### 装配根审视（src/app 2,720 行）

| 文件 | 行 | 职责 |
|---|---|---|
| appcontext.cpp | 950 | 服务装配 + 工程生命周期中枢（见下职责清单） |
| mkproject.cpp | 619 | 工程文件创建 CLI |
| main.cpp | 410 | 入口 + 主窗 attach 序（264-349：attachWorkflows/DepthConversion/PropertyModel/FaciesMapping/AiAssistant/PythonConsole/Faults/WellSiting/ErrorHub/Mapping/Workbench） |
| crossplotcontroller.cpp | 353 | 交会图控制器 |
| aiwiring.cpp | 74 | AI 远端预测装配（**好先例**：纯函数 + 结果结构体 + 独立可测） |

**AppContext 职责清单**（ctor 113-596 + 成员函数）：
A. QGIS 运行时生命周期 + 启动追踪（113-140）
B. 核心服务构建：store/taskSvc/projectSvc/manifest/layerSvc/layerOrganizer（142-161）
C. 导入服务 + layerDeclared 接线（163-170）
D. 画布/处理/编辑/样式/工具/ErrorHub/选区（172-184）
E. 断层解释控制器（186-188）
F. 地震/井地图联动（190-209）
G. 布局/预测/ONNX/AI 辅助（211-230）
H. AI 远端预测装配（aiwiring::installRemotePrediction）+ AI 对话/LLM 钥匙串 + 工具回路（bindChatToolRunner）（234-260）
I. Python 三件（261-266）
J. 编图/验证/井网辅助工作流（267-295）
K. 编图链门面/版本库/编辑服务回挂（297-318）
L. **工程生命周期中枢**：projectOpened 巨 lambda（320-589，约 250 行）——锁接管/只读四落盘面/journal 恢复/manifest rehydrate/AreaRules/配准/崩溃上下文/catalog 九连绑/onnx 模型注册/版本库重绑/测区井位井轨迹层/断层 store/布井 store/图层树规整
M. mapConfigurationChanged → 配准重挂 + 底图刷新（591-595）
N. 非 ctor：refreshSurveyLayer/refreshWellsLayer/refreshWellTrajectoriesLayer、acquireProjectLock、closeProject/releaseProjectSession、drainTasks、析构

**勘察事实（对任务书印象的修正）**：图片道（wellsection image track）装配
不在 appcontext——在 `PaleoMainWindow::attachWellSection`（paleomainwindow_
wellsection.cpp）自建 WellSectionWorkflow（壳侧唯一持有点）；appcontext 只
经 main.cpp 的 attachWorkflows 间接进入。底图（refreshBasemaps）与配准
（setGeoreference/clearGeoreference）确在 appcontext（L 段 + M 段）。

**「组装根瘦身」最小方案（定案）**：
- 最小落地：配准+底图刷新在 L/M 两段各出现一次（460-462/592-594 与
  536/594），抽 app 内自由函数 `applyGeoreference(QgisProjectService*,
  DataImportService*, QgisLayerService*)`（新 TU src/app/mapwiring.cpp，
  aiwiring 同构：纯函数、无类接口变更、独立可测），appcontext 两调用点
  改调它。行为等价（同一语句序列收敛为单一函数）。
- 如实递延（记 TODOS）：projectOpened 巨 lambda 分段抽 wiring（锁/会话段、
  catalog 九连绑段、onnx 模型注册段、断层/布井 store 段）——顺序语义敏感
  （锁先于读、journal 先于写、AreaRules 先于 catalog 装载），需专属方向
  承接，方向 83 不追。

## 验证证据（方向 83）

### 体量（wc，2026-10-08 拆分后）

```
913  paleomainwindow.cpp（本体，≤1200 ✔，> 家族最大 850 ✔）
850  paleomainwindow_workbench.cpp（家族最大，≤900 ✔，≤本体 ✔）
770  _attach.cpp / 704 _attach_constraint / 643 _attach_shell(668→643) / 642 _attach_models
597  paleomainwindow_docks.cpp（新 TU，≤700 ✔）
388 _attach_mapping / 385 _wellsection / 343 _attach_compose / 323 _attach_data / 310 _sections
239  paleomainwindow_ribbon.cpp（新）
206 _attach_validate / 193 _attach_predict
201  paleomainwindow_pages.cpp（新）
156  paleomainwindow_project.cpp（新）
144  paleomainwindow_status.cpp（新）
106  paleomainwindow_errorhub.cpp（新）
 59  paleomainwindow_shortcuts.cpp（新）
 + paleomainwindow_internal.h（共享助手契约头）
```

### 类零变更
- 公共区（class 声明至 `protected:` 前，157 行）diff 逐字节一致：`.split-audit/public-api-baseline.txt` vs `-after.txt` → `PUBLIC-API-IDENTICAL`
- 头文件 diff +10 行全在 private 区（build 段声明，先例 buildRibbon/buildRibbonPanels/addEditingPanel 同区）
- 消费方零改动：git status src/app tests/ → 空（22 消费文件清单见 R0 节）

### 行为等价
- 转录校验（`.split-audit/verify_transcription.py`）：旧本体+attach_shell 段全部非空行 multiset 被「新本体+7TU+internal.h」覆盖；唯二残差 = mapEmpty findChild 替换行 + internal 头 inline 前缀——均为拆分机制行
- 信号连接序（`.split-audit/audit_connect_order.py` → connect-order-evidence.txt）：构造期 connect 55=55 条；**唯一偏移** = ErrorHub historyChanged/historyCleared 两条前移至错误胶囊 connect 之后、工程打开进度 connect 之前（wireErrorHubStatus 合并原 960-975+1013-1029 两段；对象互不相同，无同信号序依赖）——Low，入 PR body
- 壳族测试 ×2（拆分后）：tst_notifications PASS×2、tst_shortcuts_shell PASS×2；tst_panels 131过/5红×2、tst_uxtheme 30过/1红×2——与**stash 回基线直跑**（同机同 commit 原始源码）逐函数一致（dataops_d2_savedFilterPresets/treeSortMemory/d6_vim/d7_column/d8_importPreset + themeSettingsRoundTripDefaultsLight）→ 预存 Windows QSettings 环境红，非拆分所致
- R0 红集合 27 项（`.split-audit/redset-baseline.txt`，基线全量 ctest 两分块）；终局对照见下

### 环境与构建
- 全量构建绿（build11/12/13/14 EXIT=0）；新 TU 零 MSVC 警告（与基线 paleomainwindow.cpp 零警告口径一致）
- layering 三档：loose ✔ / strict exit 0 ✔ / selftest 21+2+5+7+3+2 夹具 ✔
- include 纪律：定向修剪 6 处未用 include（本体 paleoviewport.h+QToolButton；_project QApplication/QLabel/QPushButton/QVBoxLayout）

### 装配根落地
- onnxwiring.{h,cpp}：ONNX 模型注册表装配自 projectOpened 巨 lambda 抽出（纯函数；无 ORT 空实现；调用位原位）；appcontext.cpp -28/+3；tr() 上下文 AppContext→QObject（中文源语言串，l10n_selftest 对照验证）
- 递延项记 TODOS.md「方向 83 装配根瘦身递延」

## 迭代记录

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|---|---|---|---|---|
| R0 | 勘察+切分线定案+API 快照+staged 9 文件草拟 | - | - | 基线构建 |
| E1 | 环境排障（详见「环境坑位记」） | build6 全量绿（75min） | ✔ | 基线 ctest |
| R1 | 基线 ctest 两分块（首跑在 126 被瞬时杀，-I 126,370 续跑） | **R0 红集合 27 项**（redset-baseline.txt） | ✔ | 应用拆分 |
| R2 | 应用拆分（staged 9 文件 + apply_edits.py）；4 轮 include 补漏（QStatusBar×2/SelectionContext/DataPreviewTabs×2/QStringLiteral 修复）后构建绿 | build11-14 EXIT=0；新 TU 零警告 | ✔ | 行为对拍 |
| R3 | 行为对拍：转录 multiset（唯二机制残差）；connect 序 55=55（唯一偏移=记录在案 wireErrorHubStatus 合并）；壳族 ×2（绿×2 + 基线同红×2 与 stash 基线直跑**逐函数一致**：panels 131过/5红同函数、uxtheme 30过/1红同函数）；layering 三档绿 | 证据文件齐 | ✔ | 装配根 |
| R4 | onnxwiring 落地（appcontext -28/+3）+ include 修剪 6 处 + TODOS 递延 | build13-15 绿；diff --check 净 | ✔ | 全量对照 |
| R5 | 全量 ctest 两分块：chunk A 126/126（117 绿 9 红，previewmap_perf 通过）；chunk B 244（225 绿 19 红）→ **首跑集合 diff 7 项全 perf/负载类**：+aiassist_perf（stash 基线直跑**同样红**·同断言 batch/single 比）+perf_catalog/previewmap_perf/pyramid_consume（拆分树复跑全绿；pyramid 3 连跑绿）−errorhub_storm/perf_regress/seismic_3dviz（基线负载红未触发，良性）→ **功能面红集合 diff 为空，零真实回归** | ctest-final-a/b.log + perf-recheck.log | ✔ | 收口 |
| R6 | review 轮 1+轮 2（五维：API 等价/信号序/装配语义/TU 边界/无死代码）全绿；原子提交 ×3（5c14188e 拆分 / 2747ded2 onnxwiring / 7ec813a8 TODOS）；no-op 构建确认提交树=实测树 | git log；ninja no work to do | ✔ | 完成 |

## Low 项（入 PR body）

1. wireErrorHubStatus 合并原状态钮/Hub 同步两段：ErrorHub historyChanged/historyCleared 连接前移至工程打开进度连接之前（对象互异，无同信号序依赖）。
2. onnxwiring 两句日志 tr() 上下文 AppContext→QObject（中文源语言串，l10n_selftest 绿）。
3. buildCenterArea→buildLayerTreeDock 的 mapEmpty 经 findChild\<QLabel*\>("mapEmptyState") 取回（PaleoEmptyStateLabel 无 Q_OBJECT，Qt 6.11 findChild static_assert 拒绝直接查派生类；该块只用 setVisible）。
4. 本机 Windows 预存环境红（基线 27 项红集合，QSettings 注册表/负载 perf 门类），见 redset-baseline.txt。

### 环境坑位记（后续方向复用）

1. `paleo-dev.ps1` 含中文注释（UTF-8 无 BOM）——**必须 pwsh.exe**（PS7 默认 UTF-8）；powershell.exe 5.1 按 ANSI 解析必炸解析错误。
2. 本会话 shell 的 PATH 无 `C:/Qt/Tools/CMake_64/bin`（cmake 3.29.3）与 Ninja——先前置，否则撞 cmake 3.27。
3. 新建 worktree 无 gitignored vendor；**先 junction `vendor/onnxruntime` 再首次 configure**。若先无 ORT 配过一次再补 ORT，CMake AUTOMOC 状态机脏（moc 清单不更新），唯一解是删 build 重配。
4. 本机 32GB/16 核但多会话并行构建常驻（空闲内存 ~4GB）——`PALEO_JOBS=4` 防 cl.exe 静默被杀（ninja 无 FAILED 行直接死即此症状）。
