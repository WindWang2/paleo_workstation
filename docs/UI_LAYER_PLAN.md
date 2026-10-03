## Implementation plan
# UI 分层收口计划：数据 / 视图 / 功能

日期：2026-09-27。分支：master。

## 0. 目标

`docs/PALEO_QGIS_PLAN.md` 已定总架构（QGIS 管 GIS 底座，Paleo 管地质语义）。本计划只管一件事：**在 `src/` 内部把「数据 / 视图 / 功能」三层边界钉死**，让后续界面迭代和功能开发不用在大文件里翻来翻去。

验收标尺：一个新人打开 `src/ui/` 下任何文件，能一眼看出它只干「渲染 + 发意图信号」；任何文件解析、工程编排、业务状态机都在 `src/` 非 UI 目录里；双向依赖检查进 ctest，方向反了立刻红。

## 1. 三层契约

| 层 | 目录 | 职责 | 允许依赖 |
|---|---|---|---|
| 数据 | `src/domain` `src/catalog` `src/io` `src/metadata` `src/services` `src/algorithms` | 资产/实体/关联的存取与查询；文件解析（LAS、SEG-Y、井分层/井头文本、GeoJSON）；工程文件读写；预览文档模型；算法核心 | QtCore、QtGui 基础类型；**禁止** QtWidgets、**禁止** include `ui/` |
| 功能 | `src/workflow` `src/linkage` `src/ai` | 工作流编排、命令执行、状态机（保存/发布门）、导入编排、打开工程编排 | 数据层 + QGIS 封装 + QtCore/QtGui；**禁止** QtWidgets、**禁止** include `ui/`、不 new 应用对话框/页面类（交互经信号回到壳） |
| QGIS 封装 | `src/qgis` | Qgs* 原生 widget/服务的封装（QgsMapCanvas、QgsLayoutExporter、处理对话框等）——QGIS 控件本身是 QWidget，此目录豁免 QtWidgets 禁令 | 数据层 + QtWidgets + qgis/* 头；**禁止** include `ui/` |
| 视图 | `src/ui/**` | 渲染、布局、收集输入、发意图信号；**只读**数据层门面（`DataCatalog`、`EntityView`、metadata 只读类型） | QtWidgets + 数据层读门面 + QGIS 封装 + 功能层信号；**禁止**调解析器入口、写目录/工程文件、跑业务编排 |
| 组装根 | `src/app` | 依赖注入与装配（`AppContext` 组合各层；`main.cpp` 入口） | 任意层，**唯一**允许 include `ui/` 的非视图目录；反向扫描豁免（见 W6.1，by design） |
| 测试壳 | `src/selfcheck` | 自诊断入口 | 同组装根规则 |

规则一句话：**视图只发信号不干活，功能只编排不画像素，数据只问答不管谁来问。**

### 1.1 现状合规部分（保留）

- `src/{domain,catalog,io,services,workflow,algorithms}` 无 QtWidgets 依赖（已验证）。`linkage` 例外：`threewaylocator.h` 持有 `QTabWidget*`/`WellCorrelationPanel*`/`ValidatePage*` 成员——语义上依赖 QtWidgets，由 W3b 消除，不计入「现状合规」。
- 页面板 → workflow 走信号的方向是对的（`PredictPage::runRequested` 等），保留。
- 类型只读豁免：视图可 include 数据层的纯类型头（`domain/types.h`、catalog DTO）与 **metadata 只读门面白名单**：`metadata/layermanifest.h`、`metadata/paleoprojectstore.h`、`metadata/mapversionstore.h`、`metadata/releasestore.h`（当前 ui 合法引用：`paleomainwindow.cpp:21,23,24`、`pagepanels.cpp:13`、`taskpanel.cpp:3`、`releasepanel.{h,cpp}:6,3`）。禁止的是解析入口与写路径（`metadata/paleoprojectfile.h` 上黑名单）。

## 2. 现状违规清单（实测，二轮补齐）

| 位置 | 混入的别的层 | 证据 |
|---|---|---|
| `src/ui/paleomainwindow.cpp`（3237 行） | 功能：工程打开编排 `openPath`；文件夹导入编排 `runFolderImport*` + `stampSourceArea`；临时配准 `applyProvisionalRegistration`（写 DERIVED 资产 + 建图层）；装配接线 `attachWorkflows`/`attachMapping` 合计 ~1100 行 | 27 处 `#include "../` 跨层引用 |
| `src/ui/datapreview/datapreviewtabs.cpp`（2547 行） | 数据：直接调 `LasParser::parse`、`SegyReader`、`SegySectionGrid`、`parseWellTopsText`、`parseWellHeadText`、`GeojsonAffine`；`m_segyReaders` 缓存住在视图 | **8** 个 `io/*` include（行 7–14） |
| `src/ui/correlationpanel.cpp` + `src/ui/correlation/curvebrowser.h` | 数据：`io/lasparser.h`（correlationpanel.cpp:4、curvebrowser.h:7）——一轮漏网，W1 覆盖范围扩到这里 | grep 实测 |
| `src/ui/pages/pagepanels.cpp`（2875 行） | 一个文件塞五个页面类；`io/arearules.h`/`io/dataimportservice.h` 不止取枚举——9+ 处 `AreaRules::active()` 运行时读取 + 5 处 `qobject_cast<DataImportService*>`（仅为拿 `catalog()`） | `DataPage::*` 343–1984 行 |
| `src/ui/paleomainwindow.cpp:22` | 数据：`metadata/paleoprojectfile.h`（写路径头；用点在 :1442、:1553，均位于 `stampSourceArea`/`openPath` 编排体内，随 W2 迁出而消除） | — |
| `src/ui/paleomainwindow.h:9` | 头文件 include `../io/dataimportservice.h`（`FolderPreviewRow`/`FolderRowResult` 静态签名用；二者是 `DataImportService` 嵌套 struct） | — |
| `src/workflow/mapexport.cpp:7` | **反向**：功能层 include `../ui/layout/layoutexportactions.h` | — |
| `src/linkage/threewaylocator.cpp:4-5` | **反向**：linkage include `../ui/correlationpanel.h`、`../ui/pages/pagepanels.h` | — |

## 3. 目标结构

```
src/
  domain/
    types.h                   # 不动（持久值类型 QVariantMap 契约保持纯净）
    arearules.h               # 整头平移自 io/arearules.h：Rules/ClassifierRules/DatPathRule/SegyIndexing/OnnxGrid 值类型 + active()/setProjectDir 访问器原样（工程规则是数据，不是 io 实现细节；视图经此读，不再 include io/）
    importrows.h              # NEW：FolderPreviewRow / FolderRowResult(+Outcome) 瞬态 DTO（从 dataimportservice.h 解嵌套；不进 types.h——其契约要求 QVariantMap 往返）
  services/
    previewdoc.{h,cpp}        # NEW 数据层：assetId → 渲染就绪预览模型（LAS/SEGY/tops/heads/geojson/timedep）
  io/
    lasdoc.h                  # NEW：LasCurve 纯类型 + well-info DTO（从 lasparser.h 拆出，视图可用）
  workflow/
    folderimport.{h,cpp}      # NEW 功能层：文件夹导入编排（预览→确认回调→两阶段导入→回填）
    projectopen.{h,cpp}       # NEW 功能层：openPath + 从工区文件夹新建
    registration.{h,cpp}      # NEW 功能层：临时配准（仿射→DERIVED 登记→图层实例化）
  ui/
    paleomainwindow.{h,cpp}   # 壳装配 + 页面切换 + dock 策略（目标 ≤ ~2200 行，实测落点 ~2128）
    paleoribbon.{h,cpp}       # ribbon 类目/命令组构建 + 镜像绑定（buildRibbonPanels 迁入）
    dialogs/folderconfirm.{h,cpp}  # NEW 视图：T22 确认表（PaleoFolderConfirm 命名空间）
    pages/datalist.{h,cpp}    # NEW：数据列表（导入行 + 树/表 + 搜索/筛选）
    pages/entitypanel.{h,cpp} # NEW：数据属性（实体角色槽/派生品/缺失源）
    datapreview/*             # 只渲染 PreviewDoc 模型，不再 include 解析器头
```

### 3.1 新组件契约

**`services/previewdoc`**（数据层）：输入 assetId，输出渲染就绪模型。内部走 `PaleoTaskService` 异步（沿用现有 worker 模式）；SEG-Y reader 缓存、LAS 曲线、井分层/井头行、GeoJSON 折线、时深表都从这里出。视图只拿结果结构体，不知道文件格式。连井剖面（correlationpanel/curvebrowser）的 LAS 曲线也走同一门面。最初方案保留 `lasAt(path)` 同步消费（旧 correlationpanel.cpp:530,542 为当时快照）；wave/deepen-perf B1 已把 correlation 的生产调用改为 quiet 异步任务、每井世代号与取消，worker 仍复用 `PreviewDocService::lasAt`。无任务服务时保留同步兜底；预览 tab 则走 `requestLas` / `onLasReady`。

**SEG-Y 是两阶段惰性协议，子 API 逐一钉死**（工程评审补钉，拆分前快照 datapreviewtabs.cpp:1990–2108；现行在 datapreviewtabseismic.cpp::buildSeismicContent 与 services/previewdoc）：① `segyOpen(assetId)` → 打开+索引一次，返回 geometry + inline/crossline 测线列表（缓存的 reader 持有文件句柄）；② `segyDecode(assetId, lineNo, isInline)` → 按用户选线按需 `readInline`/`readCrossline` 出道集——旧 decode 在途时新一代接管、陈旧结果按 `m_decodeSeq` 代际计数丢弃（语义原样）；③ `segyRelease(assetId)` → tab 关闭释放句柄（`m_decodeTask` 取消语义随行）；④ sha 校验谓词原样：**仅 sha 失配**（外链源被动过）才发下游过时意图，解码失败/取消不产标；⑤ `taskSvc == nullptr` 时同步降级路径保留（tst_datapreview 两模式都跑）。句柄归属、worker 线程纪律与「finished 后 GUI 只读快照」交接不变。

**在途态契约（设计评审补入）**：LAS/tops/heads/geojson 从 GUI 线程同步解析改为 service 异步产出后——预览 tab 立即打开，先放 loading 占位（复用现有「读取失败 + 重试」盒的容器样式），失败落回既有「读取失败」+原因+「重试」态、源缺失落「找不到源文件」态；不出现"tab 等数据就绪才打开"。大 LAS 的 GUI 同步解析延迟项已在 wave/deepen-perf B1 关闭，见 TODOS 已完成项及 docs/progress/deepen-perf.md。

**`workflow/folderimport`**（功能层）：`preview(dir)`、`run(dir, confirmCb, retryCb)`、`retry(row)`；`stampSourceArea` 落到这里。确认机制钉死为注入回调：`run` 接收 `std::function<bool(QVector<FolderPreviewRow>&)>`（确认）+ `std::function<FolderRowResult(int row, QString forceType)>`（行级重试回写，对应现状对话框跨导入存活渲染 `writeFolderRowResult` 的协议）。对话框另有三个壳侧副作用，也走注入回调：`onShowUnresolved()`（查看未决 → 跳数据页 + `setUnresolvedFilter(true)`）、`onPreviewAsset(assetId)`（自动打开刚导入的井头资产预览）、`importActiveChanged(bool)`（`m_folderImportActive` 归 workflow 持有，经此通知壳）。**批量结果回表接缝（工程评审补钉）**：现状是确认点击在对话框事件循环内发起导入、对话框保持打开、`applyResults` 按行回写仍开着的表（paleomainwindow.cpp:1257–1394）。为此 workflow 另暴露导入侧入口 `import(dir, overrides, progressCb) → FolderImportOutcome{QVector<FolderRowResult> rows; QString error; QString wellHeadAssetId;}`，由**对话框侧确认闭包**回调（壳持有 workflow 引用）；「任务池在场→worker 线程、不在场→同步」的分支保留在 workflow 内。结果回行**按 `path` 键对账不按表索引**（D5 两阶段排序会打乱行序，现状 1276–1290 已如此——新接缝不得退化为索引键）；「确认后取消键改『关闭』、有未决行才露查看未决、整体失败可重试、对话框中途关闭结果弃置（QPointer guard）」各态原样保留。`stampSourceArea` 以 `projectDir == importDir` 门控（:1429–1438 语义）、井头 assetId 按文件名回查 catalog 的查找（:1294–1305）一并迁入 workflow 经 `FolderImportOutcome` 返回。`preview(dir)` 的扫描期视图态：忙碌光标 + 状态栏文案一行；`preview` 为空时维持现状行为（exec 空表 + 全行跳过语义），明确记为本轮契约。`FolderPreviewRow` 增 `displayType`/`typeEditable`/`typeVocab` 字段——`typeVocab` = 全局 `projectClassifierTypes()` ∪ 该行分类器词表外类型（preview 阶段由 io 谓词算好回填），对话框纯渲染不回查 io。壳侧 `PaleoFolderConfirm` 命名空间函数把行渲染进 `QDialog`、exec、回写 overrides——对话框只画表，编排器不 new 对话框。

**`workflow/projectopen`**（功能层）：`openPath(path)` 三分支（.paleo / .qgz / 工区目录：已有工程→打开，新工区→建工程并唤起导入）。`PaleoMainWindow::openPath` 只留 `QFileDialog` 选择 + 委托 + 错误弹窗。

**`pages/datalist` + `pages/entitypanel`**（视图层）：`DataPage` 三段正式分家——导入行 + 列表段成 `DataListPanel`（中央 `dataListHost`），属性段成 `EntityPanel`（右 dock 第 0 页「数据属性」）。保留全部 objectName（`assetTable`/`assetTree`/`entityRoleTable` 等）与信号（`assetActivated` `assetWellActivated` `wellSelected`…）。`DataPage` 降级为兼容薄壳类留在 `pages/datapage.{h,cpp}`，测试零改动。

## 4. 工作项（按依赖序）

### W1 — 预览数据下沉（最大违规，先做）
1. 新建 `src/services/previewdoc.{h,cpp}`：`PreviewDoc` 变体（las / segy / tops / heads / geojson / timedep）+ `PreviewDocService`（注入 `PaleoTaskService`，异步产出，SEG-Y reader 缓迁入此）。
2. `DataPreviewTabs` 改为只消费 `PreviewDoc`：删除全部 8 个 `io/*` include 与解析调用；`datapreviewtabs.h` 的 `SegyReader` 成员/前置声明同步移除。
3. `src/io/lasdoc.h`：`LasCurve` 类型（lasparser.h 内唯一纯类型）+ well-info DTO 从 `lasparser.h` 拆出；`correlationpanel.cpp`、`curvebrowser.h` 改用 `lasdoc.h` + `PreviewDocService::lasAt(path)`（同步，见 §3.1），删掉 `lasparser.h` include。
4. 预览的标签打开/测线定位/井预选信号路径不变（`openAsset`/`openAssetForWell`/`openSeismicLine` 签名不动）。
5. `m_svc`（`DataImportService*`）非解析面改排：`datapreviewtabs.cpp` 里除解析外还有 catalog 只读（:1188-1362）、`absolutePathForVersion`（:1362,1370,1859,1905）、`documentPdfState`（:2175-2183，含 `documentPdfReady/Failed` 信号转发供 `tst_datapreview` spy）、`setImportService`/`m_catalogForTitles` 绑定（:1067-1084）——读面走 `PreviewDocService` facade 透传；三个**写**操作 `relocateVersionSource`（:1161）、`markDownstreamStale`（:2080，sha 失配谓词见 §3.1）、`ensureDocumentPdf`（spawn soffice 落 DERIVED 版本，写路径）违反视图只读契约，改发意图信号由壳/功能层接（`documentPdfRequested(assetId)` 出，`documentPdfReady/Failed` 回，spy 断言面不变）。`tst_datapreview` 的 `setImportService`/`documentPdfReady` 调用点同步改，行为断言不动。

### W2 — 文件夹导入与工程打开下沉
1. `src/ui/dialogs/folderconfirm.{h,cpp}`：`populateFolderConfirmTable`、`collectFolderTypeOverrides`、`writeFolderRowResult`、`folderImportSummaryText`、`buildFolderConfirmDialog`、`folderTypeLabel`、`folderRowDisplayType` 迁入 `PaleoFolderConfirm` 命名空间。`tst_panels` 改 include 与调用点（行为断言不动）。
2. `src/workflow/folderimport.{h,cpp}`：`runFolderImport`/`runFolderImportAt` 编排迁入（对话框仍由壳 exec）。
3. `src/workflow/projectopen.{h,cpp}`：`openPath` 主体迁入；`stampSourceArea` 跟随。
4. `paleomainwindow.h` 摘掉 `../io/dataimportservice.h` include。
5. `pagepanels.cpp`/`datapreviewtabs.cpp` 里的 `arearules`/`dataimportservice` 类型下沉：`arearules.h` **整头平移** `io/` → `domain/`（工程评审定案：Rules/ClassifierRules/DatPathRule/SegyIndexing/OnnxGrid 值类型 + `active()`/`setProjectDir`/`activeProjectDir()` 访问器原样——工程规则是数据层工件；`active()` 的「视图可读 accessor」歧义就此消除，不再走 domain 门面重导出或 catalog 挂接两条备选）；视图不再 include `io/arearules.h`。**结构体整体下沉**：`FolderPreviewRow`、`FolderRowResult`（含嵌套 `Outcome`）从 `DataImportService` 解嵌套移到新建 `domain/importrows.h`（瞬态类型不进 types.h 的 QVariantMap 契约）。`io/` 内部同步改引用——波及 `io/ingestplan.h:93`、`ingestplan.cpp:616+`、`dataimportservice.{h,cpp}`（`importFolder`/`folderRowFor`/`executePlannedItem` 签名）、`paleomainwindow.cpp`（19 处用点）与测试 `tst_import.cpp`（22 处）/`tst_panels.cpp:1479-1480`/`tst_perfbudget.cpp:194`/`tst_datapreview.cpp`。**兼容裁决（本计划内拍板）**：`DataImportService` 内保留 `using FolderRowResult = ::FolderRowResult;` 等源码兼容别名一期，测试零改动；删别名列为 TODOS 递延项。这样 `ui/dialogs/folderconfirm.h` 只依赖 `domain/importrows.h`，不引入 ui→`io/dataimportservice.h` include（该头同时进 W6.1 黑名单兜底）。
6. `pagepanels.cpp` 的非枚举使用也随拆分清除（§2 该行描述已修正）：9+ 处 `AreaRules::active()` 运行时读取改 include `domain/arearules.h`（整头平移后 API 不变，调用点零改动只换 include 路径）；5 处 `qobject_cast<DataImportService*>` 仅为取 `catalog()`——`paleo.page.importsvc` 属性改携带 `DataCatalog*`（或预览/实体门面），cast 全删。

### W3 — 临时配准下沉
`applyProvisionalRegistration`（手工仿射 → DERIVED GeoJSON 版本登记 → 图层实例化 + 水印）迁入新 `src/workflow/registration.{h,cpp}`；壳只留「画布水印 + 状态栏文案」两个视图动作。水印生命周期归属钉死：`m_provisionalLayers` 计数归 workflow，经 `provisionalLayerCountChanged(n)` 信号通知壳切换水印（现状是只增的单向锁存，语义原样保留）。

### W3b — 反向依赖回正（spec 评审补入）
1. `mapexport.cpp → ui/layout/layoutexportactions.h`：mapexport 实际只用导出核心（`PaleoLayoutExportActions::exportLayout`/`Format`/`PageRange`/`ExportOutcome`，mapexport.cpp:292–295）；把核心抽到 `src/qgis/layoutexport.{h,cpp}`（依赖 QgsLayoutExporter，落 QGIS 封装层），`ui/layout/layoutexportactions.h` 保留对话框/QAction 薄壳复用该核心。
2. `threewaylocator.cpp → ui/correlationpanel.h`、`ui/pages/pagepanels.h`：改为发意图信号——实际动作是切底部页签 + `scrollToWellTop`，信号定为 `correlationFocusRequested(wellId, horizon)` 与 `bottomTabFocusRequested(tabId)`，订阅方在壳里接线。ctor 不再收面板指针、`attach(ValidatePage*)` 移除；`tst_threeway.cpp:208–246` 同步改为订阅新信号（断言逻辑不变：验 `currentWidget`/`lastScrollWell` 改为验信号参数 + 槽侧行为）。
3. 完成标准：`grep -rn 'include "../ui/' src/{workflow,linkage,services,catalog,domain,io,metadata,algorithms,ai,qgis}` 为零（`src/app`、`src/selfcheck` 为豁免的组装根/测试壳，不入扫描）。

### W4 — 壳瘦身：ribbon 构建与接线分解
1. `buildRibbonPanels`、`addEditingPanel` 从 `paleomainwindow.cpp` 迁入 `paleoribbon.cpp`（或新 `src/ui/ribbonpanels.cpp`）。
2. `attachWorkflows`（~810 行）按页拆成私有成员函数 `attachDataPage`/`attachPredictPage`/`attachConstraintPage`/`attachComposePage`/`attachValidatePage`，入口只排顺序 + 幂等守卫；不新增抽象层。
3. `attachMapping` 按段拆（导出接线 / 版本状态机 / 发布门）。

### W5 — DataPage 分家
`DataListPanel` + `EntityPanel` 独立成件；`DataPage` 保留为兼容薄壳（`pages/datapage.{h,cpp}`），objectName、信号签名、**对外公共方法（`setUnresolvedFilter` 等被壳/文件夹确认对话框调用的入口，薄壳转发到子面板）**不变，测试零改动。

### W5b — 页面文件拆分
`pagepanels.cpp` 拆成每页一文件：`pages/predictpage.{h,cpp}`、`pages/constraintpage.{h,cpp}`、`pages/composepage.{h,cpp}`、`pages/validatepage.{h,cpp}` + W5 的 `datalist`/`entitypanel`/`datapage`。`pagepanels.h` 保留为聚合头，测试 include 不变。

### W5c — 层标记头注释
每个 `src/` 文件头注释加一行 `层：数据 | 功能 | QGIS 封装 | 视图 | 组装根 | 测试壳`（沿用现有中文注释风格；`src/app` 标「组装根」、`src/selfcheck` 标「测试壳」）。

### W6 — 边界护栏 + 文档
1. `tools/check_layering.py`：
   - include 路径先**规范化成仓库相对路径**再匹配（`../../io/ingestplan.h`、`../io/x.h`、`io/x.h` 归一为 `io/x.h`），防 `../` 前缀绕过。
   - 正向：`src/ui/**` include 黑名单 = `io/lasparser.h` `io/segyreader.h` `io/segysectiongrid.h` `io/wellfileparsers.h` `io/timedeptool.h` `io/geojsonaffine.h` `io/projectclassifier.h` `io/arearules.h` `io/dataimportservice.h` `algorithms/` `metadata/paleoprojectfile.h`。metadata 白名单（`layermanifest.h`/`paleoprojectstore.h`/`mapversionstore.h`/`releasestore.h`）见 §1.1，白名单外的 `metadata/*` 一律失败。
   - 反向：`src/{domain,catalog,io,metadata,services,workflow,linkage,algorithms,ai,qgis}/**` 出现 `../ui/` include 即失败；以上全部目录（含 `workflow`/`linkage`/`ai`/`algorithms`）出现 QtWidgets 痕迹即失败——**词表制**覆盖 `#include <QtWidgets…>`、`#include <QWidget>` 等单类头、`class Q…;` 前向声明三类写法（否则 threewaylocator.h 式 `class QTabWidget;` 漏检）；仅 `qgis` 豁免 QtWidgets，不豁免 `ui/`。`src/app`、`src/selfcheck` 不扫描——组装根/测试壳按契约允许 ui/ 依赖，此豁免在检查器中以注释注明 by design。
   - `io/` 正向规则改**白名单制**：`src/ui/**` 只允许 include `io/lasdoc.h`（及后续逐头显式放行的门面头），其余 `io/*` 一律失败——枚举黑名单挡不住 `ingestplan.h`/`horizonbinner.h`/`constraintstore.h` 这类漏网头。metadata 同理白名单（§1.1 四头）。
   - 层标记检查：每个 `src/` 文件头三行内必须有 `// 层：<六值词表之一>`，缺失即失败（W5c 的硬执行面，防注释腐烂）。**原子落地**：层标记添加与检查器启用必须同一 commit，否则 ctest 中途红。
   - 既有合法残留走 `tools/layering-baseline.txt`（带日期、逐文件列明、只许缩不许涨）；失败信息写明「如何正确收敛 baseline」一行，防橡皮图章。
   - **自检**：`check_layering.py --selftest` 跑内置正/反夹具（含 `class QTabWidget;` 与 `../../io/` 前缀样例），进 ctest——扫描器自身坏掉不能静默变绿。
   - 结构性上限写进 AGENTS.md：各层编进同一 `paleo_core` 静态库，护栏只能挡 include 层，挡不住「不带 include 直接 new」——接受为本轮已知边界（不引入接口抽象层的代价）。
2. 挂进 `ctest`；新增源文件按现有 CMakeLists.txt 显式列表登记（主源码列表 `CMakeLists.txt:82–146`，另有 `target_sources` 块 :155、:160、:163、:173、:261–269、:274–276、:283–286、:309+，无 glob）。
3. `AGENTS.md` 写三层契约；`docs/PALEO_QGIS_PLAN.md` 加边界一节；`DESIGN.md` 不动。`TODOS.md` 新增递延项：clang-tidy include-order CI、`DataImportService` 兼容别名删除（一期后）、大 LAS 同步 `lasAt` 的 UI 线程延迟悬崖、文件夹导入扫描期的进度 UX（本轮只定忙碌光标契约）。

## 4b. 实施时序推演（0I）

按动手顺序推演实施者在每个时段需要什么/会撞到什么：

**HOUR 1（地基）— 动手前必须钉死的三条接缝**（来自 Reviewer Concerns，先做 30 分钟书面定稿，再动代码）：
- **C-2 确认协议**：`folderimport.run(dir, confirmCb, retryCb)`；`confirmCb: std::function<bool(QVector<FolderPreviewRow>&)>`，`retryCb: std::function<FolderRowResult(int row, QString forceType)>`；**另钉 `import(dir, overrides, progressCb) → FolderImportOutcome`**——确认闭包在对话框内回调此入口，结果按 `path` 键回行（工程评审补钉，§3.1）。`FolderPreviewRow` 增加 `displayType`/`typeEditable`/`typeVocab` 字段（preview 阶段由 `io/projectclassifier.h` 谓词算好回填），对话框纯渲染不回查 io。
- **C-3 m_svc 改排清单**：`PreviewDocService` 需承载——catalog 只读透传、`absolutePathForVersion`、`ensureDocumentPdf`/`documentPdfState`（含 `documentPdfReady/Failed` 信号转发给 tst_datapreview spy）；两个写操作 `relocateVersionSource`/`markDownstreamStale` 改发意图信号由壳接。
- **C-4 AreaRules accessor（已定案）**：`io/arearules.h` 整头平移 `domain/arearules.h`（值类型 + `active()`/`setProjectDir` 原样），调用点只换 include 路径；`paleo.page.importsvc` 属性改携带 `DataCatalog*`，消除 5 处 `qobject_cast`。

**HOUR 2-3（核心逻辑）— 会撞到的歧义**：
- C-1 解嵌套：在 `DataImportService` 内保留 `using FolderRowResult = ::FolderRowResult;` 源码兼容别名，否则 `tst_import`（22 处）/`tst_panels`/`tst_perfbudget`/`tst_datapreview` 全断；别名是否保留要拍板（建议保留一期，下个迭代再删）。
- C-5 落点：`FolderPreviewRow`/`FolderRowResult` 是瞬态类型，**不进** `domain/types.h`（其自声明要求 QVariantMap 往返），落新建 `domain/importrows.h`。
- SEG-Y reader 缓存迁移：`m_segyReaders` 的 assetId→reader 映射与线程归属语义原样搬进 `PreviewDocService`。

**HOUR 4-5（集成）— 会让人意外的点**：
- `threewaylocator` 信号化后，`tst_threeway.cpp:208-246` 的 `attach(&page)`/直断面板断言改为信号订阅（先加信号再删 include，两步走）。
- `tst_layoutexport`（~15 处 `exportLayout` + `exportFinished` spy）靠 ui 壳类薄委托 `src/qgis/layoutexport` 核心保持绿（C-6）。
- `attachWorkflows` 幂等测试是 W4 拆函数的保险丝，先跑绿再拆。

**HOUR 6+（打磨/测试）— 事后才想到的事**：
- `tools/check_layering.py` 的 `io/` 正向规则改白名单制（仅放行 `lasdoc.h` 等显式门面头），避免 `ingestplan.h`/`horizonbinner.h`/`constraintstore.h` 漏网（C-7）。
- `layering-baseline.txt` 首轮会有既有残留，逐文件列明并标日期。
- 新增源文件登记点：CMakeLists 主列表 :82–146 外还有 target_sources 块（:155, :160, :163, :173, :261–269, :274–276, :283–286, :309+）。
- 五页截图人工 QA + `paleomainwindow.cpp` 前后行数对比写回 Review record。

**工时口径**：CC+gstack 集中实现约 2–4 天（W1/W2 占大头）；人工介入点 ~0.5 天（HOUR 1 三条接缝确认 + 五页截图 QA + 放行拍板）。

## 4c. 用户可见面清单（冻结面，即 W7 人工 QA 检查单）

本计划的隐含契约是「像素不变、接线不同」。以下为被冻结的用户可见面，逐一核对即完成行为回归：

| 冻结面 | 现状行为（不可回归） |
|---|---|
| 五页 Ribbon 页签 | 数据管理/预测编图/单因素图/智能编图/验证；objectName 与类目名不变 |
| 数据页三栏 | 中央 `dataListHost`（导入行+列表+搜索/筛选）/ `dataListPreviewSplit` / 右 dock 第 0 页「数据属性」 |
| `folderImportDialog`（T22 确认表） | 提示语→CRS 句→4 列表（路径/类型/实体/结果）→汇总行→确认/取消/查看未决；锁定行、跳过行、行级重试按钮、`folderShowUnresolvedButton` 显隐、`folderCrsNote` |
| 导入完成后副作用 | 「查看未决」跳数据页 + `setUnresolvedFilter(true)`；刚导入的井头资产自动开预览；`m_folderImportActive` 期间的重试语义 |
| 预览 tab 状态机 | 在途→loading 占位（新增，见 §3.1）；失败→「读取失败」+原因+「重试」；源缺失→「找不到源文件」+路径；成功→各类型视图 |
| 临时配准水印 | 画布水印 + 状态栏文案；`m_provisionalLayers` 单向计数语义不变 |
| Ribbon 命令镜像 | 页面按钮 enabled/tooltip/checked/text 镜像到 ribbon action；触发回 `button->click()` |
| 页面间联动 | `threewaylocator` 的切底栏页签 + `scrollToWellTop` 行为（改为信号驱动后不变） |

### W7 — 收口验证
1. `cmake --build` 全量；`QT_QPA_PLATFORM=offscreen ctest` 全绿（重点 `tst_ui` `tst_panels` `tst_datapreview` `tst_webviewpanel` `layering`）。
2. `paleomainwindow.cpp` 行数与跨层 include 数的前后对比写进本计划 Review record。
3. 五个页签截图人工过一眼（DESIGN.md 对照），数据页三栏布局确认；逐条核对 §4c 冻结面清单（导入对话框各态、预览在途/失败态、水印、未决跳转为重点）。
4. 新接缝的测试补齐（工程评审补入）：
   - `tst_folderimport`（新或并入既有）：`workflow/folderimport` 编排——confirmCb/retryCb 接线、空 preview 空表契约、`stampSourceArea` 的 projectDir==importDir 门控、`importActiveChanged` 切换、wellHeadAssetId 文件名回查、`import()` 的 taskSvc 在场/缺席双路径、路径键对账。
   - `tst_previewdoc`（新或并入 `tst_datapreview`）：在途占位→成功/失败/源缺失三态、`segyDecode` 代际丢弃、`segyRelease` 句柄释放、sha 失配→下游过时意图（解码失败/取消不产标）、无 taskSvc 同步降级。
   - `tools/check_layering.py --selftest` 入 ctest。

## 5. 明确不做（NOT in scope）

- 不引入 MVVM/MVC 框架或接口抽象层——靠文件归属 + include 护栏，不加虚基类。
- 不改 `DataCatalog`/`EntityView`/metadata 只读门面的查询 API（视图直接读是契约允许的）。
- 不动算法/预测/编图业务逻辑本身；不动 `src/qgis` 内部实现（只加 `ui/` include 禁令）。
- 不做 catalog.sqlite 索引（TODOS 已递延）；不做 clang-tidy include-order（本计划 TODOS 新增递延项）。
- 不改 SARibbon 第三方源码；不碰 `vendor/`。

## 6. 风险

- **W1 预览下沉**是最大改动：SEG-Y 异步 reader 生命周期跨 worker 线程，缓存迁移要保证句柄归属不换语义——`m_segyReaders` 的 assetId→reader 映射语义原样搬进 service。
- **W2 静态函数迁出**破坏 `tst_panels` 对 `PaleoMainWindow::populateFolderConfirmTable` 的直接驱动——测试同步改调用点，行为断言不动。
- **W3b 反向依赖**可能牵着信号接线顺序——`threewaylocator` 的调用方先改成信号再删 include，分两步走。
- **W4 拆函数**容易顺手改信号连接顺序——`attachWorkflows` 幂等测试是保险丝，必须绿。
- 分层检查初期误报走 `layering-baseline.txt` 记录基线，baseline 只缩不涨。

## 7. 完成定义

1. `src/ui/**` 零黑名单 include（`lasparser`/`segy*`/`wellfileparsers`/`timedeptool`/`geojsonaffine`/`projectclassifier`/`arearules`/`dataimportservice`/`algorithms`/`paleoprojectfile`）；非 ui 扫描目录零 `ui/` include 且零 QtWidgets（`qgis`、`app`、`selfcheck` 豁免按契约）；`paleomainwindow.cpp` ≤ ~2200 行（W1–W4 抽出 ≈1109 行，预测落点 ~2128；实测前后对比写进 Review record）。
2. `tools/check_layering.py` 接入 ctest 且绿（含 baseline 收敛）；`pagepanels.cpp` 拆分落地。
3. 既有测试全绿；五页功能行为不变（逐页人工核对）。
4. `AGENTS.md` + `PALEO_QGIS_PLAN.md` 写明三层契约；`TODOS.md` 收到 clang-tidy 递延项。

## 8. 架构依赖图（工程评审产出）

```
┌─────────────────────────── 组装根 src/app ───────────────────────────┐
│  AppContext：注入 catalog / taskSvc / workflows → PaleoMainWindow    │
│                    │（唯一允许 include ui/ 的非视图目录）             │
└────────────────────┼─────────────────────────────────────────────────┘
                     ▼
┌──────────────────── 视图 src/ui/** ──────────────────────────────────┐
│  PaleoMainWindow(壳:页面切换/dock/对话框exec/意图信号接线)            │
│  ├─ paleoribbon        ribbon 类目/面板/命令镜像（buildRibbonPanels） │
│  ├─ pages/{datapage,datalist,entitypanel,predict,constraint,         │
│  │   compose,validate}  页面渲染 + 意图信号（runRequested 等）        │
│  ├─ datapreview/*      只渲染 PreviewDoc 模型（零 io/ include）       │
│  ├─ dialogs/folderconfirm  T22 确认表（纯渲染，不 new 编排器）        │
│  └─ layout/layoutexportactions  对话框薄壳 → qgis/layoutexport 核心   │
└──┬───────────────┬──────────────────────┬───────────────────────────┘
   │意图信号        │读门面                 │读门面
   ▼               ▼                      ▼
┌───────────── 功能层 ────────────┐  ┌──────────── 数据层 ─────────────┐
│ workflow/folderimport           │  │ domain/types,arearules,importrows│
│   preview→import(回调编排)      │  │ catalog/DataCatalog(读写)        │
│ workflow/projectopen            │  │ io/*(解析器实现,ui 不可达)        │
│ workflow/registration           │  │ metadata/*(白名单头对 ui 只读)   │
│ linkage/threewaylocator         │  │ services/previewdoc(NEW)         │
│   →correlationFocusRequested…   │  │ services/DataImportService       │
│ workflow/mapexport ──┐          │  │ algorithms/*(算法核心)           │
└──────────────────────┼──────────┘  └──────────────▲─────────────────┘
                       ▼                            │
┌─────────── QGIS 封装 src/qgis ───────┐            │
│ layoutexport 核心(QgsLayoutExporter) │            │
│ mapcanvas / 图元封装                 │────────────┘
└─────────────────────────────────────┘
  豁免：qgis 可用 QtWidgets；app/selfcheck 是 ui/ 依赖的唯二例外
  禁向：ui→io/*(除 lasdoc.h 白名单)；非ui→ui/；非qgis→QtWidgets
```

## 9. 代码路径 → 测试覆盖图（工程评审产出）

| 新代码路径 | 既有覆盖 | 新增/改造测试 | 缺口裁决 |
|---|---|---|---|
| `PreviewDocService` LAS/tops/heads/geojson 异步产出 + 在途占位 | `tst_datapreview`（tab 断言） | `tst_previewdoc`：三态机 + 占位 | 新增（F5） |
| `segyOpen/segyDecode/segyRelease` 两阶段协议 | `tst_datapreview`（:795-816 双模式） | 代际丢弃、release 句柄、sha 谓词分支 | 新增 |
| `PreviewDocService::lasAt(path)` 同步 | `tst_datapreview`、correlation 用点 | 路径键同步语义断言 | 并入 previewdoc 测试 |
| `folderimport.preview/import` + 回调五件套 | `tst_panels`（表辅助）、`tst_import` | `tst_folderimport`：回调接线、空表契约、stampSourceArea 门控、路径对账、双路径 | 新增（F5） |
| `FolderPreviewRow/FolderRowResult` 解嵌套 + using 别名 | `tst_import` 22 处、`tst_perfbudget` | 别名源码兼容 = 零改动即回归证据 | 既有测试即覆盖 |
| `workflow/projectopen` openPath 三分支 | `tst_ui` 壳路径 | 委托三分支 + 错误弹窗契约 | 并入 folderimport 或 ui 测试 |
| `workflow/registration` + `provisionalLayerCountChanged` | 现有水印行为断言 | 信号驱动水印 latch 断言 | 并入既有用例 |
| `qgis/layoutexport` 核心 + ui 薄壳 | `tst_layoutexport`（15 处）、`tst_layoutdesigner_full` | 壳委托核心、spy 不变 | 既有测试即覆盖 |
| `threewaylocator` 信号化 | `tst_threeway.cpp:208-246` | 改订阅 `correlationFocusRequested`/`bottomTabFocusRequested` | 改造（断言逻辑不变） |
| `DataListPanel/EntityPanel` + `datapage` 薄壳 | `tst_panels`/`tst_ui` objectName 断言 | 零改动即兼容证据 | 既有测试即覆盖 |
| `attachWorkflows` 按页拆分 | `tst_ui.cpp:601` 幂等测试 | 幂等保险丝先绿后拆 | 既有测试即覆盖 |
| `check_layering.py` | — | `--selftest` 正负夹具 + `layering` ctest 项 | 新增（F5） |

## 10. 失效模式登记（工程评审产出）

| 失效模式 | 触发面 | 后果 | 防线 | 级别 |
|---|---|---|---|---|
| 批量导入结果回不去仍开着的对话框 | folderimport 接缝漏设 | 确认表各态静默回归（§4c 冻结面破） | `import()` 入口已钉 + `tst_folderimport` | **高**（已钉） |
| SEG-Y reader 句柄跨线程泄漏/复用错 | segyRelease 漏调、缓存迁移丢归属 | 文件锁/陈旧数据 | 两阶段 API 契约 + release 测试 | 中 |
| sha 失配谓词丢失 | markDownstreamStale 意图信号化时退化 | 解码失败也误标下游过时 | §3.1 谓词钉死 + 测试断言「仅 sha 失配产标」 | 中 |
| `confirmCb` 签名不含回表通道（照搬既有钉法会丢） | 首轮设计回调集过窄 | 同第 1 行 | import 入口分离钉法 | **高**（已钉） |
| 层标记与检查器分两次提交 | W5c/W6 非原子 | ctest 中途红 | W6 钉同 commit | 低 |
| 检查器漏检 `class QTabWidget;` 前向声明 | 词表只扫 `#include <QtWidgets` | threewaylocator 式回归漏网 | QtWidgets 词表制 + selftest 夹具 | 中 |
| `using` 别名永久残留 | 二期不删 | dataimportservice.h 继续当 DTO 容器 | TODOS 递延项登记 | 低 |
| 大 LAS 经 `lasAt` 阻塞 UI 线程 | correlation 同步接口 | UI 卡死（现状已存在，非新增） | TODOS 递延（不恶化现状） | 低 |
| `qobject_cast` 清除后 `paleo.page.importsvc` 属性断 | 属性改携带 DataCatalog* | 页面拿不到 catalog | 属性契约钉 + `tst_panels` | 中 |

## 11. 已存在资产（What already exists）

- **分层纪律骨架**：`src/{domain,catalog,io,services,workflow,algorithms}` 已无 QtWidgets——本计划是收口不是从零分层（§1.1）。
- **页面→workflow 信号方向**已正确（`PredictPage::runRequested` 等），保留。
- **io 解析器已返回干净结构体**——`PreviewDocService` 是门面不是新解析。
- **任务池基础设施**（`PaleoTaskService`/`PaleoTask`、worker 线程 marshal 纪律、无 taskSvc 同步降级）已就位，预览/导入共用。
- **对话框跨导入存活渲染**协议（`writeFolderRowResult`、路径键对账、锁定/跳过/重试行、QPointer 弃置守卫）已实现于壳，迁视图命名空间即可。
- **测试面**：`tst_datapreview` 双模式、`tst_threeway`、`tst_layoutexport`、`tst_import`（22 处 FolderRowResult 用点）、`attachWorkflows` 幂等测试——全部作为零改动回归证据复用。
- **ctest `add_test` 模式**与 CMake 显式源文件列表就位，检查器直接挂。


<!-- autoplan-accepted:ceo -->
- 契约表六行：数据（含 algorithms）/ 功能（workflow+linkage+ai，禁 QtWidgets）/ QGIS 封装（豁免 QtWidgets 不豁免 ui/）/ 视图 / 组装根 src/app（唯一允许 ui/ 依赖）/ 测试壳 src/selfcheck；视图与功能允许依赖均含 QGIS 封装。
- W1 预览数据下沉：`src/services/previewdoc.{h,cpp}` + `src/io/lasdoc.h`（LasCurve + well-info DTO）；`DataPreviewTabs` 移除全部 8 个 `io/*` include 与 `m_segyReaders` 缓存；correlationpanel/curvebrowser 走 `PreviewDocService::lasAt(path)` 同步接口；`openAsset`/`openAssetForWell`/`openSeismicLine` 签名不变。
- W2 导入/开工程下沉：`PaleoFolderConfirm` 命名空间承接确认表静态辅助；`FolderPreviewRow`/`FolderRowResult(+Outcome)`/`AreaRule` 词表下沉 `domain/types.h`（波及 ingestplan/dataimportservice 签名）；`runFolderImport*`（注入 confirm 回调）/`openPath`/`stampSourceArea` 迁入 `src/workflow/folderimport`+`projectopen`。
- W3 临时配准迁入 `src/workflow/registration.{h,cpp}`。
- W3b 反向依赖回正：导出核心抽 `src/qgis/layoutexport.{h,cpp}` 消除 mapexport→ui include；threewaylocator 改发 `correlationFocusRequested`/`bottomTabFocusRequested` 信号，ctor 不收面板指针；tst_threeway 同步改订阅信号。
- W4 `buildRibbonPanels`/`addEditingPanel` 迁出主窗；`attachWorkflows`/`attachMapping` 按页拆私有成员函数；不新增抽象层。
- W5 `DataPage` 分家为 `DataListPanel` + `EntityPanel` + `datapage.{h,cpp}` 兼容薄壳；objectName 与信号签名不变，测试零改动。
- W5b `pagepanels.cpp` 按页拆文件，`pagepanels.h` 留聚合头。
- W5c 每个 src/ 文件头加 `层：` 标记注释（六值词表：数据/功能/QGIS 封装/视图/组装根/测试壳）。
- W6 `tools/check_layering.py`：正向黑名单含 `io/dataimportservice.h` + metadata 白名单（layermanifest/paleoprojectstore/mapversionstore/releasestore）；反向 `../ui/` 扫描覆盖全部非视图层目录、QtWidgets 禁令覆盖 workflow/linkage/ai/algorithms（仅 qgis 豁免）；`app`/`selfcheck` 豁免并注明；`layering-baseline.txt` 只缩不涨；ctest 强制；AGENTS.md 与 PALEO_QGIS_PLAN.md 写边界契约；TODOS.md 补 clang-tidy 递延项。
- W7 全量构建 + `QT_QPA_PLATFORM=offscreen ctest` 全绿为完成条件；paleomainwindow.cpp 落点 ~2128 行（≤~2200）并记录前后对比。
- 保留需求：五页 ribbon 结构、QGIS 画布为主、数据页三栏、测试 objectName/信号兼容（tst_panels/tst_threeway 仅改调用点）、attachWorkflows 幂等。
<!-- /autoplan-accepted:ceo -->


<!-- autoplan-accepted:design -->
- §4c 用户可见面清单为冻结面契约：W7 逐条核对；任何行为差异即回归。
- §3.1 预览在途态：tab 立即打开 + loading 占位（复用失败盒样式）；失败/缺失态沿用现实现。
- folderimport 接缝回调全量钉死：confirmCb/retryCb/onShowUnresolved/onPreviewAsset/importActiveChanged；preview 空结果维持现状空表行为；扫描期忙碌光标契约。
- DataPage 兼容面 = objectName + 信号 + 对外公共方法（含 setUnresolvedFilter 转发）。
- 水印生命周期：workflow 持有计数，`provisionalLayerCountChanged` 通知壳。
- 结构修正落点：`domain/importrows.h`（FolderPreviewRow/FolderRowResult）、`domain/types.h`（AreaRule）、`DataImportService` 内 using 别名一期。
- W5c 层标记由 check_layering.py 硬检查（头三行 `// 层：`）。
- TODOS 递延：lasAt 大文件 UI 线程悬崖、别名删除、导入扫描进度 UX。
- Design outside voice：Codex 600s 超时 unavailable；单模型评审 [single-model]。
- 修订 ceo 条目（实现计划已改，accepted 块原文保留此处声明替换）：W2 中 `FolderPreviewRow`/`FolderRowResult(+Outcome)` 落点由 `domain/types.h` 改为 `domain/importrows.h`，`DataImportService` 保留 using 别名一期；folderimport 接缝回调扩为 confirmCb+retryCb+onShowUnresolved/onPreviewAsset/importActiveChanged；W6 正向规则由黑名单改为 io/metadata 白名单制并增层标记硬检查；W2 增加 AreaRules::active()/qobject_cast 清除项。
<!-- /autoplan-accepted:design -->

<!-- autoplan-accepted:eng -->
- folderimport 批量回表接缝：新增 `import(dir, overrides, progressCb) → FolderImportOutcome{rows, error, wellHeadAssetId}`，由对话框确认闭包回调（对话框保持打开、按 `path` 键对账、confirm→取消变关闭、QPointer 弃置守卫保留）；`stampSourceArea` 门控与井头回查迁入 workflow。
- PreviewDocService SEG-Y 钉死两阶段子 API：`segyOpen`（索引+句柄）/`segyDecode`（按需选线、代际计数丢弃、可取消）/`segyRelease`（tab 关释放）；sha 谓词——仅 sha 失配才发下游过时意图；无 taskSvc 同步降级保留。
- `ensureDocumentPdf` 属写路径：改 `documentPdfRequested` 意图信号，`documentPdfReady/Failed` 回执不变。
- `arearules.h` 整头平移 `io/` → `domain/`（含 `active()`/`setProjectDir`；types.h 契约保持纯净）；`paleo.page.importsvc` 属性改携带 `DataCatalog*`。
- check_layering.py 加固：include 路径规范化（防 `../../io/` 绕过）、QtWidgets 词表制（含单类头与 `class Q…;` 前向声明）、`--selftest` 夹具、baseline 收敛提示、W5c+检查器同 commit 原子落地；单一静态库的 include 级护栏上限写入 AGENTS.md。
- 新增测试：`tst_folderimport`（编排/回调/门控/双路径）、`tst_previewdoc`（三态/代际/释放/sha 谓词/同步降级）、check_layering selftest。
- 工程评审产出落盘：§8 架构图、§9 路径→测试图、§10 失效模式登记、§11 已存在资产、测试计划 artifact（~/.gstack/projects/paleo_workstation/*test-plan*.md）。
- 修订 design 条目（实现计划已改，块原文保留此处声明替换）：folderimport 回调集在 confirmCb/retryCb/onShowUnresolved/onPreviewAsset/importActiveChanged 之外补 `import()` 批量入口；`domain/types.h`（AreaRule）改为 `domain/arearules.h` 整头平移。
<!-- /autoplan-accepted:eng -->
## Review record

### Decision ledger (CEO Step 0)

| ID/owner | Contract & evidence | Current | Proposed | Status | Approval & scope |
|---|---|---|---|---|---|
| R0/mode | 审阅模式 | — | SELECTIVE EXPANSION | approved | autoplan 覆盖规则：/autoplan 固定 SELECTIVE EXPANSION |
| R1/scope | 计划内 7 个工作项 W1–W7 | W1–W7 in scope | — | approved | 用户原指令即工作边界 |
| R2/expand | pagepanels.cpp 2875 行 5 类同文件 | 不拆 | 每页一文件 + 聚合头 | approved | autoplan P1/P2：同爆炸半径、机械拆分 <1d |
| R3/expand | 文件头层标记 | 无 | `层：数据/功能/视图` 头注释 | approved | autoplan P5：零成本显式标记 |
| R4/expand | clang-tidy include-order | 无 | CI 检查 | deferred | 超爆炸半径（CI 配置），TODOS |
| R5/expand | catalog.sqlite | 已递延 | — | deferred | TODOS 既有 P3 项，触发条件未到 |
| R6/expand | MVVM/接口抽象层 | 无 | — | rejected | 计划明示不做；P5 显式优于抽象 |

<!-- AUTONOMOUS DECISION LOG -->
### Decision Audit Trail

| # | Phase | Decision | Classification | Principle | Rationale | Rejected |
|---|-------|----------|-----------|-----------|-----------|----------|
| 1 | CEO | Mode=SELECTIVE EXPANSION | Mechanical | autoplan override | 管线固定模式 | — |
| 2 | CEO | 接受 R2 pagepanels 拆分 | Mechanical | P1+P2 | 同文件拆分在爆炸半径内 | — |
| 3 | CEO | 接受 R3 层标记头注释 | Mechanical | P5 | 显式标记零成本 | — |
| 4 | CEO | R4 clang-tidy 递延至 TODOS | Mechanical | P3 | 超出爆炸半径 | — |
| 5 | CEO | R5/R6 维持递延/拒绝 | Mechanical | P4/P5 | 重复既有决议/避免过度抽象 | — |
| 6 | Design | 跳过 Step 0.5 mockup 生成 | Mechanical | P5 | 零新增视觉面，mockup 无信息量；W7 已有截图 QA | — |
| 7 | Design | 0D 全维度评审（偏交互态/系统对齐） | Mechanical | P1 | autoplan 全维度规则 | — |
| 8 | Design | 接受 1.1 冻结面清单（§4c） | Mechanical | P1+P5 | 「行为不变」从断言变清单可核对 | — |
| 9 | Design | 接受 2.1 在途态契约（loading 占位复用失败盒） | Mechanical | P1 | 唯一实际改变感知 UX 的接缝，必须钉死 | — |
| 10 | Design | 接受 3.1 对话框回调扩展（onShowUnresolved/onPreviewAsset/importActiveChanged） | Mechanical | P2 | 爆炸半径内，防壳指针泄漏 | — |
| 11 | Design | 修正 §3/W2 落点为 importrows.h（C-5） | Mechanical | P5 | 内部矛盾修正 | types.h 方案 |
| 12 | Design | DataPage 兼容契约扩展到公共方法（3.2） | Mechanical | P5 | setUnresolvedFilter 等跨页调用面 | — |
| 13 | Design | W5c 层标记纳入 W6 硬检查 | Mechanical | P1+P5 | 无检查的标记必腐烂 | — |
| 14 | Design | UX 递延项入 TODOS（lasAt 悬崖/别名删除/导入进度） | Mechanical | P3 | 超本轮爆炸半径 | — |
| 15 | Design | Codex 外部设计评审超时 → unavailable，单模型评审 | Mechanical | 环境失败 | 网络重连失败 600s 超时；已记 review-log | — |

### Spec review loop（3 iterations, capped）

| 轮次 | 分数 | 结果 |
|---|---|---|
| R1 | 6/10 | FAIL：13 项遗漏（correlation LAS、arearules、反向依赖、DataPage 三段、qgis 例外等） |
| R2 | 6.5/10 | FAIL：14/16 项已修；新增 C1–C8（契约表系统性遗漏、FolderPreviewRow 解嵌套、metadata 白名单等） |
| R3 | 6/10 | FAIL：7/8 修正确认；新增 C-1..C-8（folder-confirm 协议欠钉、m_svc 非解析面、§2 描述失真等） |

三轮达到上限停循环。未解决项记入 CEO summary `## Reviewer Concerns`（`~/.gstack/projects/paleo_workstation/ceo-plans/2026-09-27-ui-layer-separation.md`）；动手前必须钉死的接缝：C-2（确认回调协议）、C-3（m_svc 改排）、C-4（AreaRules accessor）。

### 文档批准

- 2026-09-27 用户批准两份输入文档（CEO summary + 本计划），进入 0I 时序推演（结果见 §4b）与后续 design/DX/eng 阶段。C-1..C-8 作为已记录的实现期接缝随工作项落实。

### Design phase review（单模型，Codex 超时 unavailable）

Step 0: 7/10（架构重构类计划，设计面冻结契约为主）。Litmus scorecard：本计划不产生新视觉面，litmus 七项按「现有 UI 保持不变」处理——Codex 缺席 → 全部 NOT SPEC'D，记 outside_status=unavailable。

Passes（auto-decide 全数应用，详见 audit trail #6–#15）：

| Pass | 初评 | 修后 | 要点 |
|---|---|---|---|
| 1 信息架构 | 6 | 9 | 缺「用户可见面清单」→ §4c 冻结面清单落地 |
| 2 交互态 | 5 | 9 | 在途态/扫描期/空表契约补入 §3.1 |
| 3 旅程弧线 | 6 | 9 | 对话框三回调 + DataPage 公共方法契约 |
| 4 AI Slop | 8 | 9 | 无新视觉；修 §3 importrows.h 矛盾 + typeVocab 语义 |
| 5 设计系统 | 9 | 9 | DESIGN.md 不动，无新组件——无 issue |
| 6 响应式/无障碍 | 7 | 7 | 桌面应用现状保留；无新增需求 |
| 7 未决决策 | — | — | 水印计数信号、别名裁决、层标记硬检查全部拍板 |

Overall: 5/10 → 9/10（六评分项取最低）。未决：无——剩余风险均为 eng 面（线程、签名兼容），留给 Phase 3。

### Engineering phase review

Native 评审全部事实核对通过（3237/2547/2875 行、27 处跨层 include、8 个 io/*、5 处 qobject_cast、9+ 处 AreaRules::active()、反向 include 位置均精确）。9 项发现全部 auto-fix 落计划：

| # | 级别 | 发现 | 落点 |
|---|---|---|---|
| F1 | **高** | folderimport 回调集缺批量结果回表通道（对话框确认后仍开着等 applyResults） | §3.1 钉 `import()→FolderImportOutcome` + path 键对账 |
| F2 | 中 | `ensureDocumentPdf` 是写路径混进读门面；`markDownstreamStale` 仅 sha 失配谓词 | W1.5 三写操作走意图信号；§3.1 谓词钉死 |
| F3 | 中 | SEG-Y 是两阶段惰性协议非「assetId→模型」 | §3.1 钉 segyOpen/segyDecode/segyRelease 三子 API + 降级路径 |
| F4 | 中 | 检查器漏 `class QTabWidget;` 前向声明与 `../../io/` 前缀 | W6 词表制 + 路径规范化 + 单库上限写 AGENTS.md |
| F5 | 中 | 新接缝无测试：folderimport 编排、previewdoc 异步态、检查器自检 | W7.4 三项新增 |
| F6 | 低 | W5c 层标记与检查器须同 commit | W6.1 原子落地条款 |
| F7 | 低 | §1.1 metadata 引用行号不全 | 已修正 |
| F8 | 低 | retryCb 表索引脆弱性 | §3.1 钉 path 键对账 |
| F9 | 低 | AreaRule 落点自相矛盾（types.h QVariantMap 契约） | 定案 `domain/arearules.h` 整头平移 |

安全面：无新增信任边界；`lasAt(path)` 路径仍来自 catalog/对话框；检查器仅 dev 时。性能面：SEG-Y 缓存迁移不改变句柄/线程语义（§6 风险已载）；`lasAt` 大文件同步阻塞是现状非新增（TODOS 递延）。

**Eng outside voice：Codex 600s 超时 unavailable**（TLS handshake eof / reconnect 循环，同 design 阶段的网络故障）；单模型评审 [single-model]，eng consensus 表各行记 N/A。

<!-- autoplan-accepted:ceo -->
- 契约表六行：数据（含 algorithms）/ 功能（workflow+linkage+ai，禁 QtWidgets）/ QGIS 封装（豁免 QtWidgets 不豁免 ui/）/ 视图 / 组装根 src/app（唯一允许 ui/ 依赖）/ 测试壳 src/selfcheck；视图与功能允许依赖均含 QGIS 封装。
- W1 预览数据下沉：`src/services/previewdoc.{h,cpp}` + `src/io/lasdoc.h`（LasCurve + well-info DTO）；`DataPreviewTabs` 移除全部 8 个 `io/*` include 与 `m_segyReaders` 缓存；correlationpanel/curvebrowser 走 `PreviewDocService::lasAt(path)` 同步接口；`openAsset`/`openAssetForWell`/`openSeismicLine` 签名不变。
- W2 导入/开工程下沉：`PaleoFolderConfirm` 命名空间承接确认表静态辅助；`FolderPreviewRow`/`FolderRowResult(+Outcome)`/`AreaRule` 词表下沉 `domain/types.h`（波及 ingestplan/dataimportservice 签名）；`runFolderImport*`（注入 confirm 回调）/`openPath`/`stampSourceArea` 迁入 `src/workflow/folderimport`+`projectopen`。
- W3 临时配准迁入 `src/workflow/registration.{h,cpp}`。
- W3b 反向依赖回正：导出核心抽 `src/qgis/layoutexport.{h,cpp}` 消除 mapexport→ui include；threewaylocator 改发 `correlationFocusRequested`/`bottomTabFocusRequested` 信号，ctor 不收面板指针；tst_threeway 同步改订阅信号。
- W4 `buildRibbonPanels`/`addEditingPanel` 迁出主窗；`attachWorkflows`/`attachMapping` 按页拆私有成员函数；不新增抽象层。
- W5 `DataPage` 分家为 `DataListPanel` + `EntityPanel` + `datapage.{h,cpp}` 兼容薄壳；objectName 与信号签名不变，测试零改动。
- W5b `pagepanels.cpp` 按页拆文件，`pagepanels.h` 留聚合头。
- W5c 每个 src/ 文件头加 `层：` 标记注释（六值词表：数据/功能/QGIS 封装/视图/组装根/测试壳）。
- W6 `tools/check_layering.py`：正向黑名单含 `io/dataimportservice.h` + metadata 白名单（layermanifest/paleoprojectstore/mapversionstore/releasestore）；反向 `../ui/` 扫描覆盖全部非视图层目录、QtWidgets 禁令覆盖 workflow/linkage/ai/algorithms（仅 qgis 豁免）；`app`/`selfcheck` 豁免并注明；`layering-baseline.txt` 只缩不涨；ctest 强制；AGENTS.md 与 PALEO_QGIS_PLAN.md 写边界契约；TODOS.md 补 clang-tidy 递延项。
- W7 全量构建 + `QT_QPA_PLATFORM=offscreen ctest` 全绿为完成条件；paleomainwindow.cpp 落点 ~2128 行（≤~2200）并记录前后对比。
- 保留需求：五页 ribbon 结构、QGIS 画布为主、数据页三栏、测试 objectName/信号兼容（tst_panels/tst_threeway 仅改调用点）、attachWorkflows 幂等。
<!-- /autoplan-accepted:ceo -->
<!-- autoplan-accepted:design -->
- §4c 用户可见面清单为冻结面契约：W7 逐条核对；任何行为差异即回归。
- §3.1 预览在途态：tab 立即打开 + loading 占位（复用失败盒样式）；失败/缺失态沿用现实现。
- folderimport 接缝回调全量钉死：confirmCb/retryCb/onShowUnresolved/onPreviewAsset/importActiveChanged；preview 空结果维持现状空表行为；扫描期忙碌光标契约。
- DataPage 兼容面 = objectName + 信号 + 对外公共方法（含 setUnresolvedFilter 转发）。
- 水印生命周期：workflow 持有计数，`provisionalLayerCountChanged` 通知壳。
- 结构修正落点：`domain/importrows.h`（FolderPreviewRow/FolderRowResult）、`domain/types.h`（AreaRule）、`DataImportService` 内 using 别名一期。
- W5c 层标记由 check_layering.py 硬检查（头三行 `// 层：`）。
- TODOS 递延：lasAt 大文件 UI 线程悬崖、别名删除、导入扫描进度 UX。
- Design outside voice：Codex 600s 超时 unavailable；单模型评审 [single-model]。
- 修订 ceo 条目（实现计划已改，accepted 块原文保留此处声明替换）：W2 中 `FolderPreviewRow`/`FolderRowResult(+Outcome)` 落点由 `domain/types.h` 改为 `domain/importrows.h`，`DataImportService` 保留 using 别名一期；folderimport 接缝回调扩为 confirmCb+retryCb+onShowUnresolved/onPreviewAsset/importActiveChanged；W6 正向规则由黑名单改为 io/metadata 白名单制并增层标记硬检查；W2 增加 AreaRules::active()/qobject_cast 清除项。
<!-- /autoplan-accepted:design -->

<!-- autoplan-accepted:eng -->
- folderimport 批量回表接缝：新增 `import(dir, overrides, progressCb) → FolderImportOutcome{rows, error, wellHeadAssetId}`，由对话框确认闭包回调（对话框保持打开、按 `path` 键对账、confirm→取消变关闭、QPointer 弃置守卫保留）；`stampSourceArea` 门控与井头回查迁入 workflow。
- PreviewDocService SEG-Y 钉死两阶段子 API：`segyOpen`（索引+句柄）/`segyDecode`（按需选线、代际计数丢弃、可取消）/`segyRelease`（tab 关释放）；sha 谓词——仅 sha 失配才发下游过时意图；无 taskSvc 同步降级保留。
- `ensureDocumentPdf` 属写路径：改 `documentPdfRequested` 意图信号，`documentPdfReady/Failed` 回执不变。
- `arearules.h` 整头平移 `io/` → `domain/`（含 `active()`/`setProjectDir`；types.h 契约保持纯净）；`paleo.page.importsvc` 属性改携带 `DataCatalog*`。
- check_layering.py 加固：include 路径规范化（防 `../../io/` 绕过）、QtWidgets 词表制（含单类头与 `class Q…;` 前向声明）、`--selftest` 夹具、baseline 收敛提示、W5c+检查器同 commit 原子落地；单一静态库的 include 级护栏上限写入 AGENTS.md。
- 新增测试：`tst_folderimport`（编排/回调/门控/双路径）、`tst_previewdoc`（三态/代际/释放/sha 谓词/同步降级）、check_layering selftest。
- 工程评审产出落盘：§8 架构图、§9 路径→测试图、§10 失效模式登记、§11 已存在资产、测试计划 artifact（~/.gstack/projects/paleo_workstation/*test-plan*.md）。
- 修订 design 条目（实现计划已改，块原文保留此处声明替换）：folderimport 回调集在 confirmCb/retryCb/onShowUnresolved/onPreviewAsset/importActiveChanged 之外补 `import()` 批量入口；`domain/types.h`（AreaRule）改为 `domain/arearules.h` 整头平移。
<!-- /autoplan-accepted:eng -->

## Review record（W7.2 收口，wave/ui-layer-separation 落点）

| 度量 | 前（c5532cf 基线） | 后 | 说明 |
|------|--------------------|----|----|
| `paleomainwindow.cpp` 行数 | 3386 | 1349 | ≤~2200 达标；attachWorkflows 按页拆入 `paleomainwindow_attach.cpp`（1270），ribbon 命令组入 `ribbonpanels.cpp`（445） |
| 主窗 `../` 跨层 include | 27 | 0 黑名单命中 | paleomainwindow.cpp 内 28 处 `../` 全指向白名单目录（qgis/services/workflow/domain/linkage/metadata 门面头），io/* 清零 |
| `pagepanels.cpp` | 3319 单文件 | 8 文件 | datalist/entitypanel/datapage(薄壳)+predict/constraint/compose/validate+pagepanels.h 聚合 |
| `datapreviewtabs.cpp` `io/*` include | 8 | 0 | 全部经 `PreviewDocService` 门面 |
| ui→`io/*` | — | 仅白名单 `io/lasdoc.h` 经门面头 | check_layering.py 机械执行 |
| 非视图层→`ui/` | mapexport/threewaylocator/wellcompositexml/seismicmaplink | 0 | 信号化/类型下沉（entityview.h→catalog、wellcompositemodel→domain） |
| ctest | 73 项 | 77 项全绿 | 新增 layering、layering_selftest、tst_folderimport、tst_previewdoc |

验收命令：`cmake --build build` 全量通过；`QT_QPA_PLATFORM=offscreen ctest --test-dir build` 77/77；
`./paleo-dev selfcheck` PASS；`tools/check_layering.py`/`--selftest` 绿。

**落点更新（2026-09-28）**：`wave/ui-layer-separation` 已并入 `master`（merge `84fc13b`）；
worktree `pw-uilayer` 与本地分支已删。合并接缝：`datapreviewtabs.cpp` 一处冲突
（保留 previewdoc 门面 include，并入侧 WIP 工区图改经 `m_doc->catalog()`）；
mapping-pages 合并时其三页走 m2 实装版（io/arearules.h → domain/arearules.h），
m2 兜底 qgislayerprofile API 调用点收口为 `PaleoMainWindow::pinLayoutTheme()`。
