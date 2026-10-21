# Agent D 深审报告 — QGIS 封装 / 地图渲染 / UI/UX / DESIGN.md 符合度

- 基线：`093346e6`（#607 收口）→ `b301f2e8`，20 个 PR（#603/#608–#614/#616–#620/#621–#626）。
- 方法：Read 全文深读 + 调用链追踪（含跨层 io/workflow 消费端），基线版对照（`git show 093346e6:`）区分「本窗口引入」与「存量」。
- 排除核对：#590/#596/#585/#592 同类只登记并标 `EXCLUDED_EXISTING_ISSUE`；PR #627 文件（`src/qgis/onlinebasemap.*`、`src/qgis/previewmapcanvas.cpp`、`src/qgis/qgiscanvascontroller.cpp`、`tests/tst_onlinebasemap.cpp`、`DESIGN.md`）标 `EXCLUDED_ACTIVE_WORK`，仅经调用方契约面读取，不做深审。

---

## 1. 覆盖清单

### 1.1 深读（全文或逐行追踪）

| 文件 | 方式 | 风险级 | 窗口内变更 | 测试对应 |
|---|---|---|---|---|
| src/qgis/basemaplayer.{h,cpp} | 全文 | 高（#616 新增） | #616 新增 | tst_wellbasemap.cpp |
| src/qgis/previewrasteranalysis.cpp | 全文 | 高（#612/#625 重构） | 重构 | tst_previewrasteranalysis*（既有） |
| src/ui/wellcomposite/wellcompositecanvas.cpp | 全文 | 高（#615 修复点） | 邻接 | tst_wellcomposite* |
| src/ui/wellcomposite/compositebody.cpp | 全文 | 高（#619/#626 手势） | #619/#626 | tst_wellcomposite_visual |
| src/ui/wellcomposite/wellcompositetrack.h | 全文 | 中 | #626 | tst_welllog_defaultview |
| src/ui/wellcomposite/trackcolumns.cpp | 全文 | 高（#626 岩性粒度） | #626 | tst_welllog_defaultview + golden |
| src/ui/wellcomposite/defaultlayout.cpp | 全文 | 高（#626 新增） | #626 新增 | tst_welllog_defaultview |
| src/ui/wellcomposite/standardcurvetracks.cpp | 全文 | 高（#614/#626） | #614/#626 | tst_welllog_defaultview |
| src/ui/wellcomposite/paneltracks.cpp | 全文 | 高 | #614/#626 | tst_wellcomposite_framework |
| src/ui/wellcomposite/panelsetup.cpp | 全文 | 高（隐藏道接线） | #615/#626 | tst_wellcomposite_framework |
| src/ui/wellcomposite/paneldata.cpp | 全文 | 高 | #614/#626 | tst_wellcomposite* |
| src/ui/wellcomposite/hiddentrackbar.cpp | 全文 | 中 | #614/#626 | tst_wellcomposite_framework |
| src/ui/wellcomposite/trackregistry.cpp | 关键段 | 低 | #626 | — |
| src/ui/wellcomposite/trackconfigdialog.cpp | 全文 | 中（#626 粒度 UI） | #626 | tst_wellcomposite_framework |
| src/ui/wellcomposite/{curveconfigdialog,trackcurves,trackpatterns,trackcompound,wellcompositepanel}.cpp | diff 深读 | 中 | #614/#619/#626 | tst_welllog_defaultview |
| src/ui/datapreview/datapreviewtabwelllog.cpp | 全文 | 高（装配链） | #603/#614 | tst_welllog_defaultview |
| src/ui/datapreview/previewbasemapcontrol.cpp | 全文 | 高（#616 新增） | #616 | tst_wellbasemap.cpp |
| src/ui/datapreview/datapreviewtabs_basemap.cpp | 全文 | 中（#616 新增） | #616 | tst_wellbasemap.cpp |
| src/ui/datapreview/previewmappage.cpp | 全文 | 中 | #616（selectBasemap/addAtBottom） | tst_previewmap* |
| src/ui/datapreview/previewtocpanel.cpp | 关键段+diff | 高（#625） | #625 | tst_previewmap* |
| src/ui/datapreview/previewsectionpanel.h | diff 深读 | 高（#617 色标） | #617 | tst_seismic 色标用例 |
| src/ui/datapreview/datapreviewtabseismic.cpp | diff 深读 | 中 | #617 | — |
| src/ui/dialogs/coreimageviewer.cpp | 全文 | 高（#619 新增） | #619 新增 | tst_wellcomposite（照片道） |
| src/ui/dialogs/folderconfirm_internal.h / _preview.cpp / _results.cpp | 全文 | 高（#603 拆分） | #603/#621 | tst_folderimport* |
| src/ui/pages/constraintpage{.cpp,_internal.h} | diff 深读 | 高（#618 IPO） | #618 | tst_constraintpage* |
| src/ui/pages/constraintpage_generate.cpp | 全文 | 高（#618 一键成图） | #618 | tst_constraintpage* |
| src/ui/seismic3d/seismiccameracontroller.cpp / seismic3dviewportwidget.cpp | diff 深读 | 高（#617 手势） | #617 | tst_seismic3d |
| src/ui/paleomainwindow_{project,attach,ribbon,workbench_commands}.cpp | diff 深读 | 中 | #616/#618/#619 | tst_mainwindow* |
| src/ui/ribbonpanels.cpp | diff 深读 | 中 | #618 | — |
| src/app/main.cpp / officepreviewrenderer.cpp | diff 深读 | 中 | #622 | — |
| src/qgis/wellattributestore.cpp | diff 深读 | 中 | #619 | tst_wellattributes |
| src/ui/pages/{datalist*.cpp,dataopspanelops.h,mappingworkbenchpage.cpp}、src/ui/help/whatstiscatalog.cpp | diff 深读 | 低 | #621/#618 | — |
| tests/tst_wellbasemap.cpp / tst_welllog_defaultview.cpp | 抽查 | — | 新增 | 自身 |

### 1.2 排除面（EXCLUDED_ACTIVE_WORK，仅契约级读取）

- `src/qgis/onlinebasemap.{h,cpp}`：经 basemaplayer/previewbasemapcontrol/paleomainwindow_ribbon 调用面读（sources/createLayer/canOverlay/findLayer/destroyLayer/watchFailures/savedChoice/addPresetToProject）。同类问题未发现新实例（调用方挂 QPointer/context 的纪律良好）。
- `src/qgis/previewmapcanvas.cpp`：经 previewmappage 的 layers()/insertLayer 契约读；底图层父子关系（parent=m_page + destroyLayer 显式删除）与 #610 fault layer leak 修复同款，未发现同类泄漏。

### 1.3 未核查区（如实申报）

- `src/ui/datapreview/datapreviewtabs.cpp` 主文件全文（仅 diff 段）；`folderconfirm.cpp` 主体（经 internal/preview/results 三 TU 交叉覆盖）；`wellpositionlegendwidget.cpp`（本窗口未改）；tst_welllog_defaultview 断言细节（仅抽查索引式断言倾向，未见 #585 式裸索引断言，findText 按文本定位）。
- 双主题暗色实测未做（只读约束，无构建；以 token 出口 `applyThemedStyleSheet`/`PaleoTheme::tokens(Light)` 的纸面例外口径静态核对）。

---

## 2. Findings

### 新发现（立案）

#### D-01 · P3 · HIGH · wellcomposite / paneltracks.cpp:34-52 `syncSessionToTracks()`
- **Root Cause:** FormationTrack 三个数据源（分层/层序/砂层组）仅靠道标题字符串区分（`ft->title() == tr("层序")` / `!= tr("砂层组")`）。道配置对话框（`onTrackConfigRequested`，paneltracks.cpp:243）允许用户任意改标题，改名后分类判据失效。
- **Trigger:** 用户把「层序」道改名（如「层序 副本」或自定义名）→ 之后任何编辑会话操作（拖标志层/undo/redo 触发 `documentChanged`）。
- **Observed/Expected:** 改名后的层序道 `setIntervals(doc.formationIntervals)` 被分层区间覆盖（层序数据从图面消失、显示成分层）；同名双道内容重复。期望：按数据源身份（而非展示标题）路由同步。
- **Reproduction:** 打开含层序数据的综合柱状图 → 右键「道配置…」改「层序」道标题 → 进入 TOPs 编辑拖动任一标志层 → 层序道内容变分层。
- **Impact:** 地质文档图面静默错数据（低频路径但后果是数据混淆）；会话记忆按新标题存取反而自洽，用户无从察觉。
- **IssueMatch/PRMatch:** 本窗口 #626/#614 重构引入标题判据（基线版更糟：全部 FormationTrack 无差别覆盖——本次是部分修复残留洞）。
- **FixStrategy:** FormationTrack 增加 `dataSource` 枚举（Formation/Sequence/SandGroup），同步与会话匹配按类型标识而非标题；标题仅展示。
- **RegressionTest:** tst_wellcomposite_framework：改名层序道 → EditSession 改分层 → 断言层序道 intervals 仍为 sequenceIntervals。

#### D-02 · P3 · SUSPECTED · wellcomposite / hiddentrackbar.cpp:97-101 + panelsetup.cpp:396-402
- **Root Cause:** 「更多隐藏道」菜单 action 以 `Qt::QueuedConnection` 投递，恢复走 `trackRestoreIndexRequested(specIndex)` 索引通道；标题通道 `trackRestoreRequested(title)` 发了但消费端不用。排队投递与 `setTracks()` 重建之间无版本号护栏（仅有边界+可见性检查）。
- **Trigger:** 菜单点击 →（事件循环间隙内）隐藏道集合被重建（如另一恢复路径先行触发 `onTrackVisibilityChanged`）→ 陈旧 specIndex 恢复到错误的道上。
- **Observed/Expected:** 恢复显示的道与用户所点不一致；期望按道身份（title/指针）恢复。
- **Reproduction:** 窗口极窄（实操需事件循环精确交错），静态追踪可达；未实测复现。
- **Impact:** 低——错恢复一道显示，无崩溃（bounds 检查在）；主路径（前 3 个 chip 直连同步）不受影响。
- **IssueMatch/PRMatch:** #626（更多隐藏道菜单为 #614/#626 新增）。
- **FixStrategy:** 恢复槽改为按 `title` 定位（信号已存在），或给 HiddenTrackBar 加代次计数随信号携带。
- **RegressionTest:** tst_wellcomposite_framework：排队投递期间 setTracks 重建后恢复，断言命中原 title。

#### D-03 · P3 · HIGH · qgis 封装 / previewrasteranalysis.cpp:79-88/117-126/594-599 + previewtocpanel.cpp:499-509
- **Root Cause:** `requestSummary/requestHistogram/requestExtrema` 的 `finished` 回调仅在 `State::Succeeded`（且文件 mtime 未变）时调用 `ready`；任务失败、取消或文件中途被改时**回调永不发生**，API 无失败通道。
- **Trigger:** TOC 选中一个损坏/不可读栅格（GDAL open 失败）→ 快速样式面板 `rebuildQuickPanel` 里 min/max spin 保留**上一个图层**的数值，无任何失败提示。
- **Observed/Expected:** 数值面板描述 A 层却显示 B 层的统计；用户随后手动拉伸按错值渲染。期望：失败回调或面板侧失败态（DESIGN「诚实状态」条款）。
- **Reproduction:** 目录放一个伪装 .tif 的损坏文件进预览 → 选中 → min/max 与上一次选择相同、无报错。
- **Impact:** 静默陈旧数据驱动样式决策；三个 request* 消费面（tocpanel 已核实；tabhorizon/tabmapprocket 同构调用未逐一核实失败面）。
- **IssueMatch/PRMatch:** 契约为存量（基线版同样仅 Succeeded 回调）；本窗口 #625 重构本文件未补失败通道——登记为契约层缺口而非回归。
- **FixStrategy:** `ready` 签名加失败参数或增加 `failed` 回调；调用方在 spin/图例处显示「统计不可用」。
- **RegressionTest:** tst_previewrasteranalysis：坏 URI 任务失败后断言收到失败通知。

### EXCLUDED_EXISTING_ISSUE 登记（同类只登记，#590/#592 类）

#### D-04 · P3 · VERIFIED · #590 同类（同步重活阻塞 UI）· PR#616 新面
- src/qgis/basemaplayer.cpp:128-159（`createFromFile` 同步 `QgsRasterLayer`/`QgsVectorLayer` 构造）+ previewbasemapcontrol.cpp:134（`applyChoice` GUI 线程 build）+ previewbasemapcontrol.cpp:203-204（**每次井位类页构建自动重放 savedChoice**——保存过大 GeoTIFF/mbtiles 时每次开页同步重开栅格）。
- Impact: 大文件底图首帧卡顿秒级。FixStrategy: file 类底图经任务池异步 open 后再上画布；或至少 auto-apply 只挂层不阻塞。

#### D-05 · P3 · VERIFIED · #590 同类 · 存量（基线已有，#625 未动该路径）
- src/qgis/previewrasteranalysis.cpp:294（`applyPseudoColorRenderer` Quantile 分支在 GUI 线程同步 `histogram(layer, classes*4)` 全带直方图）+ previewtocpanel.cpp:583（HistEq 拉伸路径）。异步 `requestHistogram` 的结果被丢弃后同步重扫一遍。
- Impact: 大栅格 HistEq 拉伸 UI 冻结。FixStrategy: 把已取回的 Histogram 传入 Quantile 分桶（数据已在手，无需重扫）。

#### D-06 · P3 · VERIFIED · #590 同类 · 存量（#311 引入，本窗口 #603/#619 活跃面）
- src/ui/datapreview/datapreviewtabwelllog.cpp:217（GUI 线程逐照片 `imagelod::loadThumbnail` 同步解码；井照片多时开页卡顿）。FixStrategy: 缩略装载移入既有 requestLas 第二段任务。

#### D-07 · P3 · VERIFIED · #590 同类 · #603 拆分后保持
- src/ui/dialogs/folderconfirm_results.cpp:180-183（`retryRow` → `importFolderRow` 同步在 GUI 线程做 copy+SHA256，大文件重试冻结对话框）。基线版同构，登记不立案。

#### D-08 · P3 · VERIFIED · #592 同类（文案无 tr）· 登记清单
- compositebody.cpp:62（空态提示 QStringLiteral 非 tr）；trackcolumns.cpp:205/261/439（「顶深-底深 m」「岩性花纹」「深度等比」道头文案）；trackconfigdialog.cpp:30-39（kPalettes「经典测井/高对比/柔和」）。本窗口新增文案均已 tr()（#626 自查良好），以上为存量遗漏点补充登记。

### 排除验证（深读后裁定非问题，留档防复查）

- **folderconfirm「新建其他井…」取消路径**（folderconfirm_preview.cpp:47-49）：取消时 `setCurrentIndex(0)` 重入槽先清 `wellTargets` 再 `return`，外层不再回写 `__new__`——门控与状态正确（第一遍误判为 bug，逐行重追踪推翻）。
- **#615 横向拖拽跳动修复**（wellcompositecanvas.cpp:559-576）：夹紧偏移 + QSignalBlocker 静默回写滑块，根因与修复一致，边界（totalW<=bodyW 复位 0）正确。
- **#625 底图不做全图统计**（previewtocpanel.cpp:493/550 `paleoBasemap` 属性闸）：快速面板对底图收起栅格样式页 + 拉伸入口早退，DESIGN §161 口径落地。
- **#622**：`QSG_RHI_BACKEND=opengl` 提前到所有启动路径（main.cpp:112-116）+ officepreviewrenderer PostGate（close-under-mutex + 故意泄漏 static）线程收尾纪律正确。
- **#617 3D 手势**（seismic3dviewportwidget.cpp press/move/hover + camera Rotate 取反）：任意左键旋转、Alt 显式移切片、拾取模式 Ctrl 旋转/Shift 平移均与 DESIGN §181-184 一致；hover 光标只在 Alt 下提示移面。
- **#617 二维色标**（previewsectionpanel.h）：Indexed8 + 256 色表，0 保留缺测白；微弱负振幅强制负极性桶（`qMin(127, index)`）防量化染正；图例渐变与剖面同表；换线不重置选择（面板复用）。
- **#619 coreimageviewer**：非模态 WA_DeleteOnClose、两段装载（缩略→原图 requestDecode 唯一 key + QPointer 护栏）、加载/失败态在窗内、mono 读数、工具行 PaleoToolRow——DESIGN §2026-10-21 全项符合。
- **#618 一键成图状态机**（constraintpage_generate.cpp:799-880）：同步启动失败即停（`!runbusy` 即 fail）、outcome 优先、单发 singleShot 续步防重入；IPO 步骤条忙态冻结防误判回退。
- **#626 高度粒度/网格**：quantumCells 浮点容差正确（35/10→10/10/10/5）；trackcurves 横向网格线计数夹到像素高 + 10000 上限，无界循环不可达；纸面网格用 Light token border/surfaceAlt（DESIGN §单井测井图道口径）。
- **分层纪律**：抽查窗口内全部新 include 未见 ui→io/algorithms 违例；`services/*` 对视图开放符合层表；basemaplayer 以 QObject 门面（`build/onCanvasCrsChanged`）隔绝视图直连 qgs*，方向正确。

---

## 3. 模块健康结论

**总体判断：本窗口 UI 大增量面（wellcomposite 24 文件 + datapreview 底图链 + seismic 手势）质量高于上一审计周期。** 20 个 PR 中深读的修复型 PR（#615/#622/#625）根因定位准确、修复完整；功能型 PR（#616/#617/#619/#626）与 DESIGN.md 新增条款逐项对齐（纸面例外、禁用 reason、非模态检视、诚实状态、mono 数值、双主题 chrome）。未发现 P0/P1/P2 级问题。

- **QObject 生命周期**：窗口内新增代码普遍用 QPointer/连接 context/unique_ptr 纪律（coreimageviewer、watchFailures、PostGate、异步换源 seq 号），未发现新增悬垂捕获。#610 同类图层泄漏模式在底图链（build 失败各分支 unique_ptr/delete + destroyLayer 显式收旧层）未见复发。
- **最大残余风险面**是 #590 同类同步阻塞在**新底图链**上的扩散（D-04：每次开页同步重开本地底图文件是 #616 新引入的行为面，建议优先收敛），以及 previewrasteranalysis 失败契约缺口（D-03）。
- **wellcomposite 状态机**（默认模板↔会话记忆↔编辑会话三方同步）是长期复杂度热点：D-01 的标题判据与 D-02 的索引通道都是「以展示属性当身份」的债，建议引入道数据源身份后一并清偿。
- 建议 RegressionTest 三条（D-01/D-02/D-03 各一）随修复 PR 附带；D-04–D-08 登记不立案，收敛时按 #590/#592 台账合并处理。
