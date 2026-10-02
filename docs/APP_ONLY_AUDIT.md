# App-only 功能审计 — 本仓库对账（Phase 0 §39/E3）

状态：基于上游审计 `docs/phase0/et9-app-only-audit.md`（QGIS release-4_2
@53d73a8f）对**本仓库现状**逐能力对账。上游结论摘要：`src/app` 在 4.x 构建为
`libqgis_app`（APP_EXPORT 符号 178 头）但**头文件不安装、无 CMake export、
非支持链接目标**——按源码移植对待；4.x 里 Processing 执行 UI、图层属性对话框、
高级数字化 dock、map tips、DevTools 框架**已全部在 qgis_gui**。

判定词：**已用 gui**（直接消费 qgis_gui，无需自研）；**app 层自研·必要**
（QGIS 无可用对应，必须留在 Paleo 壳层）；**app 层自研·可再下沉**（目前自研，
但 qgis_gui 存在可替代设施，可评估改用）。

总原则（本仓库已实践）：`paleo_core` 只链 `qgis_core`/`qgis_gui`/
`qgis_analysis`（`CMakeLists.txt:33-38`），**不链 `libqgis_app`**；所有上游
app-only 能力以源码移植或自研等价物落地。

## 逐能力对账

| 能力 | QGIS 来源（ET9） | Paleo 现状 | 判定 |
|---|---|---|---|
| 主窗壳/五页布局/焦点环/a11y | QgisApp（app-only，5k 行级，32 处自引用） | `src/ui/paleomainwindow.cpp` + `src/ui/pages/{predictpage,constraintpage,composepage,validatepage,datapage}.cpp` 自研（W5b 后 `pagepanels.cpp` 已拆分为五页文件，仅剩 `pagepanels.h` 聚合头） | app 层自研·必要 |
| 图层树视图 | `QgsLayerTreeView`/DefaultActions/MapCanvasBridge 全 gui | mainwindow 直接用 gui 视图；坏层/过滤等指示器未用（上游 8 个 indicator provider 全 app-only） | 已用 gui |
| 定位器（搜索框） | widget `QgsLocatorWidget` gui；engine core；**11 个内置 filter 全 app-only** | gui widget + core engine 直用；业务过滤器自研 `src/ui/locator/paleolocatorfilters.cpp` | 已用 gui + app 层自研·必要（过滤器） |
| 布局设计器 | dialog `QgsLayoutDesignerDialog` app-only（5029 行/32 QgisApp 引用）；**全部构件在 gui**（view/tools/item widgets/registry + `QgsLayoutDesignerInterface` 官方嵌入接口） | `src/ui/layoutdesignershell.cpp` 实现 `QgsLayoutDesignerInterface` + gui 组件自组 designer shell；导出走 core `QgsLayoutExporter`（`layoutexportactions.cpp`） | app 层自研·必要（官方意图路线，ET9 结论 ①） |
| 高级数字化 dock | `QgsAdvancedDigitizingDockWidget` gui，0 QgisApp 引用 | `src/ui/edittools/*`/`paleoshapetools.cpp` 直用 | 已用 gui |
| 编辑工具（顶点/移动/环/部件…） | 基类 `QgsMapToolCapture` 系 gui；具体几何编辑工具 app-only（~3 QgisApp 引用/个） | `src/ui/edittools/{editingtools,vertexeditortools}.cpp` 基于 gui 基类自研；`vertexeditorshim.cpp` 等价顶点编辑 | app 层自研·必要 |
| Shape 数字化工具 | 框架 gui；24 个具体工具 app-only（薄，96–250 行/个） | `src/ui/maptools/paleoshapetools.cpp` 自研所需子集 | app 层自研·必要 |
| Undo 面板 | `QgsUndoWidget` app-only（trivial，0 引用） | `src/ui/edittools/editingundostack.cpp`（跨层栈）+ `src/ui/pages/`（W5b 拆分后的页文件集）的 `.paleo/undo_stack.json` vault | app 层自研·必要（含跨会话，超出上游能力） |
| 属性表 | 内件 `QgsAttributeTableView/Model/FilterModel/DualView` 全 gui；容器 dialog app-only（23 引用） | `src/ui/attributetablepanel.cpp` 用 gui 内件自组容器 | 已用 gui + 容器 app 层自研·必要 |
| Canvas 装饰（坐标/比例/指北针…） | 16 个 decoration app-only（painter overlay，薄） | `src/ui/decorations/paleodecorations.cpp` 自研 painter overlay | app 层自研·必要 |
| 状态栏坐标/比例 widget | `QgsStatusBar` gui；三个具体 widget app-only | mainwindow 自研 `statusCoords`/`statusScale`（mono 主题） | app 层自研·必要（与 DESIGN.md 主题绑定） |
| 消息日志查看 | `QgsMessageLogViewer` gui | mainwindow 直用 | 已用 gui |
| Message bar | `QgsMessageBar` gui | `qgisprocessingservice.cpp` 等直用 | 已用 gui |
| Processing 执行 UI | 4.x 全 gui（`QgsProcessingAlgorithmWidgetBase` 可嵌入 widget、batch/toolbox/wrappers） | `src/qgis/qgisprocessingservice.cpp` 消费 `qgis_processing` 算法（analysis/gui 注册面）+ 自研进度对话框 | 已用 gui/analysis（若未来要内嵌参数表单，gui 现成） |
| 图层属性对话框 | 4.x 已移入 gui（vector/raster/mesh/pointcloud） | 按需可用 | 已用 gui（未消费） |
| Map tips | `QgsMapTip` gui（4.x 起） | 未用 | 已用 gui（未消费，需要时零成本） |
| 标注/注记 UI | item widgets + map tools 全 gui | 未用 | 已用 gui（未消费） |
| 测量工具 | `QgsMeasureTool` 等薄 app-only；utils core | 未用（剖面/厚度链走 correlation panel） | 不在产品面 |
| 书签/几何校验/GPS/欢迎页 | app-only 各档 | 未用 | 不在产品面 |
| 地质域面板（对比面板/层位 chip/任务页/发布面板） | 无 QGIS 对应 | `src/ui/correlation*`、`horizonchipbar`、`taskpanel`、`releasepanel` | app 层自研·必要 |
| 嵌入 Web 面板 | 无 QGIS 对应（QGIS 内部用 WebEngine 处另有栈） | `src/ui/webviewpanel.cpp`（可降级外链浏览器） | app 层自研·必要 |

## 结论

1. **无 libqgis_app 依赖**：链接面 = core/gui/analysis（CMakeLists 零
   `qgis_app` 引用），与 ET9 建议（源码移植而非链接 app 库）一致。
2. **gui 直接消费面**（零自研成本）：图层树视图、定位器 widget/engine、
   高级数字化 dock、属性表内件、消息栏/日志、Processing 框架、map tips
   （备用）、图层属性对话框（备用）。
3. **必须自研的 app 层面**（上游 app-only 或无对应）：主窗壳、designer
   shell（走官方 `QgsLayoutDesignerInterface` 路线）、几何编辑/shape 工具、
   decorations、状态栏 widget、业务 locator filters、全部地质域面板。
   其中 designer shell 是最大单项（ET9 结论 ①，成本中高但路径是官方意图）。
4. **「可再下沉」候选 = 0 项**：本仓库没有「绕过 qgis_gui 现成设施自研了
   同等能力」的情况——自研项均为上游 app-only 或地质域专属。未来新增 UI
   能力时先查本表与 ET9，gui 已有的（如图层属性对话框、标注工具、map
   tips）不应再自研。
