// 层：视图
#pragma once
#include <SARibbon.h> // vendor/saribbon（MIT）——主窗是 SARibbonMainWindow
#include <QString>
#include <QStringList>
#include <QHash> // #473 拆分段：shown（已上图图件缓存）经 shared_ptr 参数传递
#include <QMap>
#include <QVector>
#include <functional>
#include <QPointer>

#include <memory> // #558：run 状态 unique_ptr 成员（完整类型在 paleomainwindow_runners.h）

#include "../domain/importrows.h"   // FolderPreviewRow / FolderRowResult（T22 静态面，W2 下沉 domain）
#include "notifications/paleonotify.h" // #282 maybeSaveProject 询问三选一（SaveChoice）
#include "../services/seismictaskservice.h" // m_seismicTaskSvc unique_ptr 需完整类型

class PaleoDockManager;
class QComboBox;
class QDialog;
class QLabel;
class QTableWidget;
class QTimer;
class QToolButton;

class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class QgisLabelZOrder;
class QgisLayerProfileService;
class ToolAvailabilityService;
class ErrorHub;
class NotificationCenter;
class ErrorHistoryPanel;
class SelectionContext;
class QgsMapLayer;
class LayerTreePanel;
class LayerPropertiesDialog;
class QgsLayoutItemMap;
class QgsRubberBand;
#include <QDockWidget>

namespace seismic {
class SeismicSectionDockWidget;
class Seismic3DViewPanel;
class SeismicTaskService;
}

namespace paleo::petrophys {
// #558：m_petroPhysSvc unique_ptr 头面只留声明——完整类型（含
// algorithms/petrophys 链）在实现 TU（paleomainwindow.cpp / _attach.cpp）。
class PetroPhysTaskService;
}

namespace paleo::fault {
class FaultInterpretationController;
class FaultManagerPanel;
}

class PaleoDockWidget : public QDockWidget
{
  Q_OBJECT
public:
  using QDockWidget::QDockWidget;

  bool userWantsVisible() const { return m_userWantsVisible; }
  void setUserWantsVisible(bool v) { m_userWantsVisible = v; }

  void setProgrammaticVisible(bool v)
  {
    m_programmatic = true;
    setVisible(v);
    m_programmatic = false;
  }

  void setVisible(bool visible) override
  {
    if (!m_programmatic)
      m_userWantsVisible = visible;
    QDockWidget::setVisible(visible);
  }

private:
  bool m_programmatic = false;
  bool m_userWantsVisible = true;
};

class StratigraphicWebPage;
class QStackedWidget;
class QSplitter;
class DataPreviewTabs;
class PredictionWorkflow;
class RealizationWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
class MappingWorkflow;
class MappingWorkbench;
class MapVersionController;
class MapVersionStore;
class ProjectDataFacade;
class DataCatalog;
class DataImportService;
class PreviewDocService;
class DepthConversionWorkflow;
class PropertyModelWorkflow;
class PropertyModelPanel;
// #558 扇出根一：属性建模/相图在途作业状态（JobRunner + 交接体）的完整
// 类型只活在实现侧伴生头 paleomainwindow_runners.h——本头持前置声明 +
// unique_ptr，不再经 workflow 头把 algorithms 重头链扇出给全体 ui TU。
struct PropModelRunState;
struct FaciesMappingRunState;
class FaciesMappingWorkflow;
class FaciesMappingPanel;
class PaleoMapBookPanel;
class PaleoMapBookController;
class FolderImportWorkflow;
class ProjectOpenWorkflow;
class RegistrationWorkflow;
class SeismicMapLink;
class SeismicSectionTool; // R4 信号化（方向 49）：剖面捕获工具壳持有
class QgisProcessingService;
class QgisLayoutService;
class QgisEditingService;
class PaleoProjectStore;
class PaleoTaskService;
class PaleoDecorationManager;
class PaleoEditingToolbar;
class DataPage;
class PredictPage;
class ConstraintPage;
class ComposePage;
class ValidatePage;
class WellCorrelationPanel;
class WellSectionPanel;
class MappingWorkbenchPage; // #473：attachWorkbench 子域接线段的参数面
class WellPredictionPanel;  // #473：井预测修订域句柄（WorkbenchWells）
class EvidenceInteraction;  // #473：证据域接线段参数（原 attachWorkbench 内联声明）
namespace metadata {
class WellSectionStore;
}
class FaultSetStore;
class WellSectionWorkflow;
class SectionWorkbench;
class QCloseEvent;
class QContextMenuEvent;
class QPoint;

// ui/ — PaleoMainWindow: the six-page workflow shell (§42), Ribbon 形态。
// 顶部 = SARibbon：「文件」应用按钮 + 六个页签（数据管理 | 地层对比 | 智能预测 | 单因素图 |
// 智能编图 | 验证，页签 = 工作流页）+ 右侧全局按钮组（搜索/处理算法/面板/
// Web 服务）+ 快速访问栏（保存/撤销/重做）。
// 中央：数据管理页 = 「数据列表」上 +「数据预览」下的竖向分栏；其余四页 =
// 层位 chip 条 + QGIS 画布。左 = 图层树 dock；右 = 页面参数 dock（数据页是
// 「数据属性」）；底 = 日志/任务/连井/发布/属性表。
// UI never touches Qgs* beyond canvas/layer-tree widgets (§25).
class PaleoMainWindow : public SARibbonMainWindow
{
  Q_OBJECT
  public:
    PaleoMainWindow(QgisCanvasController *canvasCtl,
                    QgisProjectService *projectSvc,
                    QgisLayerService *layerSvc,
                    ToolAvailabilityService *tools,
                    SelectionContext *selection,
                    QWidget *parent = nullptr);
    ~PaleoMainWindow() override;
    void addDockWidget(Qt::DockWidgetArea area, QDockWidget *dock);

    // Page ids: "data" | "correlation" | "predict" | "constraint" | "compose" | "validate" (+ "startup")
    void showPage(const QString &pageId);
    QString currentPage() const { return m_currentPage; }
    // 页 id 的 ribbon 页签（objectName "ribbonCategory.<pageId>"）；未知 id → nullptr。
    SARibbonCategory *categoryForPage(const QString &pageId) const;
    void showAttributeTable(const QString &layerId);
    void openDataGraph(const QString &assetId = {}, const QString &versionId = {},
                       const QString &entityId = {}, bool lineage = false);
    void openTaskCenter();
    void showStartup();            // first-run: recent projects + new/open
    void onProjectOpened();        // called after project opens: swap startup->workspace
    // #153/#154/#156/#158：工程即将关闭/切换（QgisProjectService::
    // projectAboutToClose）——清掉所有工程作用域的视图状态：预览标签与
    // PreviewDoc 会话缓存、测井对比井集、3D/剖面地震体、属性建模在途作业。
    // 在途任务已由 AppContext 开新任务会话统一取消。
    void resetProjectScopedState();
    // #282（UIS-09）：关窗/切换工程前的 QgsProject 脏状态三选一——版面、
    // 图层样式、图层树顺序、地图主题只活在 .qgz 里，静默切换即丢。返回
    // false = 用户取消（调用方不得继续关闭/切换）。offscreen 且未注入询问
    // seam 时直接放行（保持无头自动化旧行为）。
    bool maybeSaveProject();
    // C1/#282：编辑中的矢量图层缓冲三选一（maybeSaveProject 的第一步）。
    bool resolveDirtyLayerEdits();
    // 询问 seam：生产不调即模态；壳级测试替身模态框（offscreen 无法真的弹）。
    using ProjectSaveAsk =
        std::function<PaleoNotify::SaveChoice(const QString &title,
                                              const QString &text)>;
    void setProjectSaveAskForTesting(const ProjectSaveAsk &ask)
    {
        m_projectSaveAsk = ask;
    }
    // #156：测井对比井集 = catalog 全部 well_log（打开工程与导入后都走这里）。
    // 面板不挂底栏页签，只刷井集（测井计算范围来源），不再逐井加载 LAS。
    void refreshCorrelationWells();
    void setProjectReadOnly(bool readOnly);
    bool isProjectReadOnly() const { return m_isProjectReadOnly; }
    // 打开工程文件（.paleo / .qgz）或工区目录（已有工程则打开，全新工区则建工程并唤起导入）。
    bool openPath(const QString &path);

    // §42 shell persistence — geometry/dock layout/last page in QSettings;
    // canvas extent is per-project and lives inside the .qgz (paleo props).
    void saveWindowState();
    void restoreWindowState();
    void saveCanvasExtent();       // write current canvas extent into the project
    void restoreCanvasExtent();    // apply stored extent after project opens

    // Swap right-dock placeholder panels for the real page panels (§42.2),
    // bound to the workflow orchestrators. Call after AppContext assembly.
    void attachWorkbench(MappingWorkbench *workbench, class EvidenceInteraction *evidence = nullptr);
    // goal/time-depth-velocity：层树「转换为深度域…」意图 → DepthConversionWorkflow。
    // 独立于 attachWorkflows，避免动其签名（同 attachMapping 先例）。
    void attachDepthConversion(DepthConversionWorkflow *depth);
    // goal/property-modeling：属性建模面板。幂等（dock 已建则只更新指针）。
    void attachPropertyModel(PropertyModelWorkflow *wf,
                             paleo::fault::FaultInterpretationController *faults = nullptr);
    // goal/facies-automapping：证据合成 + QA 报告面板。幂等（dock 已建则只
    // 更新指针）。面板只发意图，链路在 FaciesMappingWorkflow。
    void attachFaciesMapping(FaciesMappingWorkflow *wf);
    // 方向51：AI 地质对话助手——底栏加「AI 助手」页签，挂 AppContext 持有的
    // AiChatController。编排与会话落盘全在 workflow 层，这里只装配面板 +
    // 把「配置…」意图翻成状态栏提示（配置对话框递延，见 TODOS.md）。
    void attachAiAssistant(class AiChatController *controller);
    // 方向68：Python 脚本面——底栏加「Python 脚本」+「Python REPL（实验性）」
    // 页签，挂 AppContext 持有的 PythonConsoleController。返回脚本面板指针，
    // 供组装根把 importRequested 接到 DataImportService（视图不碰 io）。幂等。
    class PythonConsolePanel *attachPythonConsole(
        class PythonConsoleController *controller);
    // goal/fault-interpretation：断层解释接线——剖面 dock 挂编排器 + 右栏
    // 断层管理面板 dock。幂等（m_faultPanelDock 已建则只重挂控制器）。
    void attachFaults(paleo::fault::FaultInterpretationController *controller);
    // 方向34：井网辅助——验证页收成「验证/布井辅助」双页签 + 地图布点
    // 工具 + 导出对话框。独立于 attachWorkflows（同 attachDepthConversion
    // 先例）；幂等（页签已建则只重挂指针）。
    void attachWellSiting(class WellSitingWorkflow *wf);
    // 方向64：错误呈现接线——NotificationCenter（通知卡/severe 模态/状态栏 info）
    // 挂本窗口 + 「错误历史」dock（布局与面板菜单唤出）。幂等（已接则忽略）。
    void attachErrorHub(ErrorHub *hub);
    void showErrorHistory();
    void attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                         CompositionWorkflow *compose, ValidationWorkflow *validate,
                         DataImportService *importSvc = nullptr,
                         SeismicMapLink *seismicLink = nullptr,
                         QgisProcessingService *procSvc = nullptr,
                         PaleoProjectStore *store = nullptr,
                         QgisEditingService *editSvc = nullptr,
                         QgisLayoutService *layoutSvc = nullptr,
                         PaleoTaskService *taskSvc = nullptr);

    // wave/mapping-pipeline 阶段C+E：编图链 / 层位图导出 / 版本状态机接到
    // ③编图页。独立于 attachWorkflows，避免动其签名。Call after attachWorkflows.
    // catalog 用于导出产物登记（OUTPUT 资产，发布门的前提）。
    void attachMapping(MappingWorkflow *mapping, MapVersionController *versions,
                       MapVersionStore *versionStore, ProjectDataFacade *projectData,
                       DataCatalog *catalog = nullptr);

    // ---- W4 拆分段（实现在 paleomainwindow_attach.cpp）----
    // attachWorkflows 入口只做幂等守卫 + 页创建 + 顺序编排；每页接线一段：
    void attachPaleomapLibrary(DataPage *page);
    void wireDataImportRequests(DataPage *page, DataImportService *service, DataPreviewTabs *preview);
    void attachProjectDashboard(DataPage *dataPage, DataPreviewTabs *preview);
    void attachDataPage(DataPage *dataPage, WellCorrelationPanel *corrPanel,
                        DataImportService *importSvc, PaleoTaskService *taskSvc);
    void attachPredictPage(PredictPage *predictPage, PredictionWorkflow *pred);
    void attachConstraintPage(ConstraintPage *constraintPage, ConstraintWorkflow *constraint);
    void attachComposePage(ComposePage *composePage, CompositionWorkflow *compose,
                           QgisLayoutService *layoutSvc);
    void attachValidatePage(ValidatePage *validatePage, ValidationWorkflow *validate,
                            DataImportService *importSvc);
    // m2(C) 接缝：版面地图项钉页面档案主题。m1 档案服务在场走
    // QgisLayerProfileService::setLayoutMapTheme（含记录修剪），缺席时直写
    // QGIS 原生 follow-visibility 预设（与 m1 兜底分支同语义）。
    void pinLayoutTheme(QgsLayoutItemMap *mapItem, const QString &pageId);
    // 把清单图层实例化并勾选到图层树。zoomTo 为真时再缩放到该层。
    // 单因素等值线、综合编图成果和验证定位都走这条，不另建显示路径。
    bool revealDeclaredLayer(const QString &layerId, bool zoomTo);
    // 绘制或保存约束后：勾选 constraints.<层位>，套约束线样式并刷新画布。
    // 主图层不是线时，再勾上已声明的线伴随视图。
    void showDrawnConstraintLines(const QString &horizon);
    // 方向 47：realization 面同画布上图 + 不确定性图签。成员/统计面各按
    // 同集合互斥显隐（成员换成员、统计换统计），差值面独立可叠。
    // badgeTitle 空 = 关图签。
    void showRealizationLayer(const QString &layerId, const QString &badgeTitle,
                              const QString &badgeSub);
    // 成员/统计 intent 的统一入口（面板与数据页树共用，图签文案同源）。
    void showRealizationMember(const QString &setId, int index);
    void showRealizationStat(const QString &setId, const QString &token);
    // 壳面（locator/保存/底栏面板/处理算法/编辑条/图件设计）；返回编辑条
    // 逻辑宿主供 buildRibbonPanels 镜像。
    PaleoEditingToolbar *attachShellSurfaces(PaleoProjectStore *store,
                                             QgisProcessingService *procSvc,
                                             QgisEditingService *editSvc,
                                             QgisLayoutService *layoutSvc,
                                             PaleoTaskService *taskSvc);
    // attachMapping 三段：发布门（m_refreshPublishGate 本体）→ 导出接线 →
    // 版本状态机。
    void attachMappingPublishGate(ComposePage *composePage, MapVersionController *versions,
                                  MapVersionStore *versionStore, ProjectDataFacade *projectData,
                                  DataCatalog *catalog);
    void attachMappingExport(ComposePage *composePage, MappingWorkflow *mapping,
                             MapVersionStore *versionStore, ProjectDataFacade *projectData,
                             DataCatalog *catalog);
    void attachMappingVersions(ComposePage *composePage, MapVersionController *versions);

    // ---- T22 文件夹确认表（静态面，tst_panels 直接驱动；runFolderImport 只
    // 负责选目录 + exec）----
    // 「导入工区文件夹」确认表的类型词表/行结果写回已下沉 ui/dialogs/
    // folderconfirm.{h,cpp}（PaleoFolderConfirm 命名空间，W2）。

    seismic::SeismicSectionDockWidget *seismicSectionDock() const { return m_seismicSectionDock; }
    QDockWidget *seismic3dDock() const { return m_seismic3dDock; }
    seismic::Seismic3DViewPanel *seismic3dPanel() const { return m_seismic3dPanel; }
    QAction *errorHistoryAction() const;

  protected:
    void closeEvent(QCloseEvent *event) override;
    // 面板管理右键（§42 壳规约）：只在停靠区标题栏/边距触发——命中件
    // 落在某个 QDockWidget 的「非内容」区域（标题栏）时弹 createPopupMenu；
    // dock 内容子树与画布各有自己的右键语义，不抢。
    void contextMenuEvent(QContextMenuEvent *event) override;

  private:
    PaleoDockManager *m_dockManager = nullptr;
    void buildShell();
    // ribbon 骨架：五个页签、「文件」菜单、右侧按钮组（面板/Web 服务）。
    // 页签内的命令组依赖页面板与服务，由 buildRibbonPanels 在 attachWorkflows
    // 末尾填充。
    void buildRibbon();
    // 方向 83 拆 TU：buildShell 按原文行序分解为下列构建段（初始化序/
    // 连接序逐行保持；实现分布 paleomainwindow_docks/_status/_errorhub/
    // _ribbon/_shortcuts.cpp）。
    void buildCenterArea();       // 中央面：启动页/画布面/数据面/地层对比页
    void buildLayerTreeDock();    // 左 dock 图层平台 + 空态同步
    void buildPageDocks();        // 右栏占位/底栏/地震剖面/连井/3D/Web dock 族
    void buildStatusBar();        // 状态栏族（层位/坐标/比例尺/CRS/打开进度）
    void wireErrorHubStatus();    // 状态栏错误胶囊 + ErrorHub 计数同步
    void registerPageShortcuts(); // Ctrl+1..6 / Ctrl+Tab（方向 63 注册表）
    void restoreCorrelationDocks();
    bool showMappingReference(const QString &layerId, QString *error);
    void buildRibbonPanels(DataPage *data, PredictPage *predict, ConstraintPage *constraint,
                           ComposePage *compose, ValidatePage *validate,
                           PaleoEditingToolbar *editTb);
    // 三个编图页各有一组「要素编辑」：动作与编辑条共享同一批 QAction；图层
    // 下拉共享编辑条下拉的 model，选择经 setCurrentLayer 回写。
    void addEditingPanel(SARibbonCategory *category, PaleoEditingToolbar *editTb);
    void addDataImportRibbon(SARibbonCategory *category, DataPage *data);
    void addFaciesDisplayPanel(SARibbonCategory *category);
    void attachFaciesDisplayControls(MappingWorkbench *workbench);
    // 面板显隐菜单（QMainWindow::createPopupMenu 列出全部 dock 的
    // toggleViewAction）。右键 dock 标题栏与右上「面板」钮共用此入口。
    // 菜单尾附「视图」节：浅色/深色主题切换（写 QSettings 仅发生在用户
    // 显式切换时；缺省浅色）。
    void showPanelMenu(const QPoint &globalPos);
    // 暗色翻案（决策日志 2026-09-28）：切主题 = 换 palette（ApplicationPaletteChange
    // 风暴自动重算所有活体样式）+ 重套 SARibbon 调色板与壳样式。dark 参数
    // 为目标态；写盘走 PaleoTheme::writeThemeToSettings（唯一写者）。
    void setDarkThemeEnabled(bool dark);
    void reapplyThemeChrome();
    // 界面密度（goal/ui-experience-polish）：切 comfort/compact = QSS 条目
    // padding 档 + 表缺省行高（PaleoTheme::applyDensity / applyDensityToViewTree）。
    // 写盘走 PaleoTheme::writeDensityToSettings（唯一写者 = 用户切换动作）。
    void setCompactDensityEnabled(bool compact);
    // 窗口标题 = 「<工程名> — Paleo Workbench [*]」（QGIS 惯例；[*] 配
    // setWindowModified，工程 isDirtyChanged/projectSaved 驱动）。无工程时
    // 只有产品名。工程打开/保存后由接线刷新。
    void updateWindowTitle();
    // W2 长任务可见性：有运行中任务时自动露出底栏任务页（程序化显隐，不动
    // userWantsVisible）；任务清空后恢复用户原可见态。
    void syncBottomDockForTasks();
    // m2(D) 页面图层档案：四个编图页切到/层位 chip 切换后重应用当前页档案
    // （QgisLayerProfileService::applyPageProfile）。数据页无档案，no-op。
    void applyCurrentPageProfile();
    // 「导入工区文件夹」：分类确认表（可改类型）→ 两阶段导入 → 计数汇总，
    // 确认后只打开井口标签（§3/autoplan-design）。
    void runFolderImport(DataImportService *svc);
    // 已知目录的入口变体（「从工区文件夹新建」复用同一确认框流程）。
    void runFolderImportAt(DataImportService *svc, const QString &dir);
    // 「多选文件导入」：与文件夹导入同一确认表/两阶段导入语义，数据源是
    // 显式文件清单（识别不了的类型按分类器落 document/unknown → 辅助资料）。
    void runFilesImportAt(DataImportService *svc, const QStringList &paths);
    // W2/W3 工作流懒建钩子（壳侧唯一持有点； nullptr 直至服务注入）。
    FolderImportWorkflow *folderImportWorkflow();
    ProjectOpenWorkflow *projectOpenWorkflow();
    // PROJECT_FILE_DESIGN：文件夹导入完成后把来源+统计回填 project.paleo
    // 的 sourceArea。只在「工程目录==导入目录」（从文件夹新建的工程）时写。
    void stampSourceArea(const QString &dir, const QVariantMap &stats);
    // D11 临时配准：手工仿射 → DERIVED GeoJSON 版本登记 → 「临时配准 · 名」
    // 矢量图层实例化 + 画布水印；失败走状态栏文案，不弹框。
    void applyProvisionalRegistration(DataImportService *svc,
                                      const QString &assetId,
                                      const QVariantMap &params);
    // T29 闪烁定位：图层范围上盖一条主色半透明橡皮带，100ms 一闪 ×4
    // （共 400ms，spec ~300–500ms）后自毁。只动橡皮带不动图层可见性——
    // 不与「在地图上显示」的双向同步打架。测试经 "horizonFlashActive"
    // 属性断言起止。
    void flashHorizonLayer(QgsMapLayer *layer);
    void syncSeismicVolumeToDocks();
    void attachSections(SeismicMapLink *link);
    // 连井剖面（well correlation section）：attachWorkflows 内
    // attachSections 之后、m_seismicTaskSvc 就位后调用。store 供剖面编辑
    // 产物落 project.sqlite（井序/连线改接版本化）。
    void attachWellSection(PaleoTaskService *taskSvc,
                           PaleoProjectStore *store = nullptr);
    SeismicMapLink *m_sectionLink = nullptr;
    SectionWorkbench *m_sectionWorkbench = nullptr; // attachSections 持有（this 父子）
    // 任意剖面捕获工具（R4 信号化，方向 49）：壳持有 SeismicSectionTool——
    // linkage 只发 sectionCaptureRequested 意图。析构在画布存活时先
    // unsetMapTool 再 delete；画布先死则由 destroyed 接线置空（坑序同
    // tst_sectionlifecycle 既有规避：活动工具不可直接 delete）。
    SeismicSectionTool *m_sectionCaptureTool = nullptr;
    PaleoDockWidget *m_wellSectionDock = nullptr;
    WellSectionPanel *m_wellSectionPanel = nullptr;
    WellSectionWorkflow *m_wellSectionWf = nullptr;
    metadata::WellSectionStore *m_wellSectionStore = nullptr;
    FaultSetStore *m_wellSectionFaultStore = nullptr;
    class WellSectionFenceWidget *m_wellSectionFence = nullptr;
    class WellSectionMapBand *m_wellSectionBand = nullptr;
    QPointer<WellCorrelationPanel> m_corrPanel; // 底栏测井对比（attachPages 创建）

    QgisCanvasController *m_canvasCtl;
    class WellSitingWorkflow *m_wellSitingWf = nullptr; // 方向34（attachWellSiting 建一次）
    class WellSitingPanel *m_wellSitingPanel = nullptr;
    // #411：在途布点工具——连续点「在地图上放置计划井」时旧工具被 setMapTool
    // 替换而无人删（canvas 不持有 map tool）。
    QPointer<class QgsMapTool> m_sitingPickTool;
    QgisProjectService *m_projectSvc;
    QgisLayerService *m_layerSvc;
    ToolAvailabilityService *m_tools;
    SelectionContext *m_selection;

    // 阶段E 发布门重算钩子：attachMapping 安装，attachWorkflows 的
    // validationDone 连接在验证跑完后调它（残差覆盖是门的一条腿）。
    std::function<void()> m_refreshPublishGate;

    StratigraphicWebPage *m_stratigraphicWebPage = nullptr;
    QList<QPointer<QDockWidget>> m_correlationHiddenDocks;
    QByteArray m_beforeCorrelationWindowState;
    QStackedWidget *m_centerStack = nullptr;   // page0=startup, page1=workspace
    QStackedWidget *m_workspaceStack = nullptr; // page0=画布面，page1=数据面，page2=地层对比
    PaleoDockWidget *m_dataListDock = nullptr;
    QWidget *m_dataListHost = nullptr;
    DataPreviewTabs *m_previewTabs = nullptr;
    bool m_previewMaximized = false;
    QByteArray m_preMaxWindowState;
    PaleoDockWidget *m_leftDock = nullptr;
    QDockWidget *m_rightDock = nullptr;
    PaleoDockWidget *m_bottomDock = nullptr;
    bool m_bottomDockAutoShown = false; // W2：任务驱动的自动露出（恢复用）
    // ---- wave/layer-platform：左 dock 图层平台（面板 + 档案工具条 + 服务） ----
    LayerTreePanel *m_layerPanel = nullptr;
    QgisLayerProfileService *m_profileSvc = nullptr;
    QPointer<MappingWorkbench> m_workbench; // 编图链三页的测井基础图层保证在场
    QgisLabelZOrder *m_labelZOrder = nullptr; // 标注随图层 z 序（labelsWithLayer 打标器）
    LayerPropertiesDialog *m_layerProps = nullptr;
    bool m_profileReplayQueued = false; // 层位切换后的页面档案重放去抖旗标
    seismic::SeismicSectionDockWidget *m_seismicSectionDock = nullptr;
    QDockWidget *m_seismic3dDock = nullptr;
    seismic::Seismic3DViewPanel *m_seismic3dPanel = nullptr;
    QToolButton *m_statusErrorBtn = nullptr;
    // goal/fault-interpretation：断层管理面板 dock（attachFaults 建一次）
    QDockWidget *m_faultPanelDock = nullptr;
    paleo::fault::FaultManagerPanel *m_faultPanel = nullptr;
    std::unique_ptr<seismic::SeismicTaskService> m_seismicTaskSvc;
    QPointer<class PaleoTask> m_seismicOpenTask;
    QString m_seismicOpenPath;
    quint64 m_seismicOpenGeneration = 0;
    // #481 P3-3：本会话已自动装载过的地震体路径——多体工程逐体推进
    //（每次同步装一个），同体不重复触发（防 A/B 轮换互踢）。
    QSet<QString> m_seismicAutoLoadedPaths;
    // goal/petrophysics-logs：测井计算批任务（面板只发意图，编排在此）。
    // #558：Qt 父子持有（构造点 parent=this，见 paleomainwindow_attach.cpp）——
    // 头面只留前置声明，algorithms/petrophys 链不再经本头扇出。
    paleo::petrophys::PetroPhysTaskService *m_petroPhysSvc = nullptr;
    QPointer<class PaleoTask> m_petroPhysTask;
    bool m_folderImportActive = false; // 文件夹导入期间抑制逐文件开预览标签
    PaleoTaskService *m_taskSvc = nullptr; // attachWorkflows 注入；空 → 导入走同步旧路径
    // #275：attach 期工程未开，projectDir/gpkg 必须调用时经此现取（与
    // rebindRealization 同口径）；按值捕获会得到永久空串/旧工程路径。
    PaleoProjectStore *m_projectStore = nullptr; // attachWorkflows 注入
    QgisEditingService *m_editSvc = nullptr; // attachShellSurfaces 注入；closeEvent 保存/放弃走它
    QVariantMap m_lastImpact; // G09：最近一次影响分析报告（受控重算入口用）
    // #282：脏工程「保存/放弃/取消」询问 seam（nullptr = 模态；测试替身）。
    ProjectSaveAsk m_projectSaveAsk;
    DataImportService *m_importSvc = nullptr; // attachWorkflows 注入；启动页「从工区文件夹新建」用
    // 壳唯一数据门面（W1）：dataPage 属性与 previewTabs 共用同一实例。
    PreviewDocService *m_previewDoc = nullptr;
    DepthConversionWorkflow *m_depthWf = nullptr;
    PropertyModelWorkflow *m_propModelWf = nullptr;
    paleo::fault::FaultInterpretationController *m_propModelFaults = nullptr;
    QDockWidget *m_propModelDock = nullptr;
    PropertyModelPanel *m_propModelPanel = nullptr;
    // #148：地图册批量导出（「智能编图 › 图件输出 › 地图册」），工程关闭时
    // resetProjectScopedState 里 resetProject。
    QDockWidget *m_mapBookDock = nullptr;
    // 方向64：错误呈现层 + 错误历史 dock（attachErrorHub 建）。
    NotificationCenter *m_notifications = nullptr;
    QDockWidget *m_errorHistoryPanelDock = nullptr;
    PaleoMapBookPanel *m_mapBookPanel = nullptr;
    PaleoMapBookController *m_mapBookCtl = nullptr;
    bool m_propModelRunning = false;
    bool m_propModelCancel = false;
    // #85：计算段在任务池 worker 上跑；交接体由 worker 写、finished 回包（GUI）
    // 读。task 是 PaleoTaskService 持有的对象，QPointer 防服务先析构。
    // V2（goal/prop-model-v2）：SGS 多实现 → list（每实现一项；呈现取首实现）。
    QPointer<PaleoTask> m_propModelTask;
    // 方向20：属性建模改由统一 JobRunner 编排（忙则拒绝/取消传播/commit 强制
    // owner 线程都由框架承担）。job 用 shared_ptr 与 commit 段共享同一份交接体
    // —— 共享所有权正是「commit 段回填的登记结果，UI 段能读到」的前提。
    // #558：runner/job/computed 打包进 PropModelRunState（窗口的 QObject 子
    // 对象，懒建；完整类型只在伴生头 paleomainwindow_runners.h 展开）。
    PropModelRunState *m_propModelRun = nullptr;
    // goal/facies-automapping：证据合成 + QA 报告面板（幂等 dock，同
    // attachPropertyModel 形态）。runner 承担忙则拒绝/取消传播/commit 亲和
    //（#558 同上）。
    FaciesMappingWorkflow *m_faciesMappingWf = nullptr;
    QDockWidget *m_faciesMappingDock = nullptr;
    FaciesMappingPanel *m_faciesMappingPanel = nullptr;
    FaciesMappingRunState *m_faciesMappingRun = nullptr;
    // #558 run 状态桥访问点（实现 paleomainwindow_attach_models.cpp）：
    // 懒建（首次使用时挂到本窗口）；cancelPropModelRun 未建时 no-op
    //（= 无在途作业，resetProjectScopedState 早于 attach 也安全）。
    PropModelRunState *propModelRun();
    FaciesMappingRunState *faciesMappingRun();
    void cancelPropModelRun();
    QPointer<PaleoTask> m_faciesMappingTask;
    bool m_faciesMappingRunning = false;
    void finishFaciesMappingRun();
    // 单因素本地方向：准备和发布在界面线程，插值在任务池。
    QPointer<PaleoTask> m_factorTask;
    void finishPropertyModelRun(double overlayAlpha);
    // goal/attr-volume — 属性体 3D 预览就绪 → 喂视口（服务线程已取数烘焙：
    // 三槽属性切片 + 堆叠层体渲染；ok=false 时状态栏报因）。
    void showAttributeVolumeIn3D(
        const seismic::SeismicTaskService::AttributeVolumePreview &preview,
        bool ok, const QString &message);
    FolderImportWorkflow *m_folderImportWf = nullptr;   // W2 文件夹/单文件导入编排
    ProjectOpenWorkflow *m_projectOpenWf = nullptr;     // W2 打开/新建工程编排
    RegistrationWorkflow *m_registrationWf = nullptr;   // W3 临时配准编排
    RealizationWorkflow *m_realizationWf = nullptr;     // 方向 47：集合编排（统计派生/差值）
    // attachWorkflows 幂等守卫：该函数每次执行都清栈重建右栏页面、给底栏/
    // 状态栏加面板并往服务对象上叠信号连接，二次执行会重复建 dock/按钮并
    // 遗留悬空引用（后续用例段错误）。测试套件会二次触达同一窗口——
    // 入口早退，见 attachWorkflows 注释。
    bool m_workflowsAttached = false;
    PaleoDecorationManager *m_decorMgr = nullptr; // D11 临时配准水印等画布装饰
    int m_provisionalLayers = 0;   // 已上图的临时配准图层数（>0 → 水印）
    QString m_currentPage;
    bool m_isProjectReadOnly = false;
    QPointer<QgsRubberBand> m_horizonFlashBand;
    QPointer<QTimer> m_horizonFlashTimer;

    // ---- #473 拆分段（attachWorkbench 子域接线段）----
    // 入口只留幂等守卫 + 页签构建 + 顺序编排；编排体按子域拆段，私有成员
    // 访问经本类成员函数（同 W4 attachMapping 拆分先例）。图例/上图/参考窗
    //（显示编排域，含参考画布 Qt::white 纸面色 token 例外）留在
    // paleomainwindow_workbench.cpp 的工厂；证据/图谱联动、井预测修订域、
    // 命令分发链在 paleomainwindow_workbench_{evidence,wells,commands}.cpp。
    using WorkbenchLegendFn = std::function<void(const QString &)>;
    using WorkbenchShowFn = std::function<bool(const QString &, QString *)>;
    using WorkbenchOpenWellsFn = std::function<void(const QString &)>;
    using WorkbenchBeginEditFn = std::function<bool(const QString &)>;
    // 证据面板/图谱联动接线（_evidence.cpp）。
    void attachMappingEditFlow(MappingWorkbench *workbench);
    bool runMappingEditFlow(MappingWorkbenchPage *page, const QString &action, const QVariantMap &parameters,
                            const WorkbenchShowFn &show);
    void attachWorkbenchEvidence(EvidenceInteraction *evidence,
                                 const QList<MappingWorkbenchPage *> &pages);
    // 井预测修订域（_wells.cpp）：面板 dock、修订副本自动保存、画布右键
    // 改相菜单；返回句柄沿用原 lambda 局部名，入口侧捕获列表零改动。
    struct WorkbenchWells
    {
        WellPredictionPanel *panel = nullptr;
        QDockWidget *dock = nullptr;
        WorkbenchOpenWellsFn openWells;
        WorkbenchBeginEditFn beginEdit;
    };
    WorkbenchWells attachWorkbenchWells(MappingWorkbench *workbench,
                                        PaleoEditingToolbar *edit,
                                        const QList<MappingWorkbenchPage *> &pages,
                                        const WorkbenchLegendFn &legend,
                                        const WorkbenchShowFn &show,
                                        const WorkbenchShowFn &compare);
    // 命令分发链（_commands.cpp）——commandRequested 的动作分支表；
    // 参数即原 lambda 捕获面（this 之外全部显式化）。
    void runWorkbenchCommand(MappingWorkbench *workbench, MappingWorkbenchPage *page,
                             PaleoEditingToolbar *edit, ConstraintPage *constraint,
                             WellPredictionPanel *wellPanel, QDockWidget *wellDock,
                             const WorkbenchLegendFn &legend,
                             const WorkbenchShowFn &show, const WorkbenchShowFn &compare,
                             const WorkbenchOpenWellsFn &openWells,
                             const WorkbenchBeginEditFn &beginEdit,
                             const std::shared_ptr<QHash<QString, QString>> &shown,
                             const QString &action, const QVariantMap &p);
    // 显示编排域工厂（workbench.cpp 本 TU）：图例 / 上图互斥定位 / 参考窗。
    WorkbenchLegendFn makeWorkbenchLegend(MappingWorkbench *workbench);
    WorkbenchShowFn makeWorkbenchShow(MappingWorkbench *workbench,
                                      const WorkbenchLegendFn &legend,
                                      const std::shared_ptr<QHash<QString, QString>> &shown);
    WorkbenchShowFn makeWorkbenchCompare(MappingWorkbench *workbench);
};
