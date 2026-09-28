// 层：视图
#pragma once
#include <SARibbon.h> // vendor/saribbon（MIT）——主窗是 SARibbonMainWindow
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVector>
#include <functional>

#include "../domain/importrows.h"   // FolderPreviewRow / FolderRowResult（T22 静态面，W2 下沉 domain）
#include "../services/seismictaskservice.h" // m_seismicTaskSvc unique_ptr 需完整类型

class QComboBox;
class QDialog;
class QLabel;
class QTableWidget;

class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class QgisLayerProfileService;
class ToolAvailabilityService;
class SelectionContext;
class QgsMapLayer;
class LayerTreePanel;
class LayerProfileBar;
class LayerPropertiesDialog;
class QgsLayoutItemMap;
#include <QDockWidget>

namespace seismic {
class SeismicSectionDockWidget;
class Seismic3DViewPanel;
class SeismicTaskService;
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

class QStackedWidget;
class QSplitter;
class DataPreviewTabs;
class PredictionWorkflow;
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
class FolderImportWorkflow;
class ProjectOpenWorkflow;
class RegistrationWorkflow;
class SeismicMapLink;
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
class QCloseEvent;
class QContextMenuEvent;
class QPoint;

// ui/ — PaleoMainWindow: the five-page workflow shell (§42), Ribbon 形态。
// 顶部 = SARibbon：「文件」应用按钮 + 五个页签（数据管理 | 预测编图 | 单因素图 |
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

    // Page ids: "data" | "predict" | "constraint" | "compose" | "validate" (+ "startup")
    void showPage(const QString &pageId);
    QString currentPage() const { return m_currentPage; }
    // 页 id 的 ribbon 页签（objectName "ribbonCategory.<pageId>"）；未知 id → nullptr。
    SARibbonCategory *categoryForPage(const QString &pageId) const;
    void showStartup();            // first-run: recent projects + new/open
    void onProjectOpened();        // called after project opens: swap startup->workspace
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
    void attachWorkbench(MappingWorkbench *workbench);
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
    void attachDataPage(DataPage *dataPage, WellCorrelationPanel *corrPanel,
                        DataImportService *importSvc, PaleoTaskService *taskSvc);
    void attachPredictPage(PredictPage *predictPage, PredictionWorkflow *pred);
    void attachConstraintPage(ConstraintPage *constraintPage, ConstraintWorkflow *constraint);
    void attachComposePage(ComposePage *composePage, CompositionWorkflow *compose,
                           QgisLayoutService *layoutSvc);
    void attachValidatePage(ValidatePage *validatePage, ValidationWorkflow *validate,
                            WellCorrelationPanel *corrPanel, DataImportService *importSvc);
    // m2(C) 接缝：版面地图项钉页面档案主题。m1 档案服务在场走
    // QgisLayerProfileService::setLayoutMapTheme（含记录修剪），缺席时直写
    // QGIS 原生 follow-visibility 预设（与 m1 兜底分支同语义）。
    void pinLayoutTheme(QgsLayoutItemMap *mapItem, const QString &pageId);
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

  protected:
    void closeEvent(QCloseEvent *event) override;
    // 面板管理右键（§42 壳规约）：只在停靠区标题栏/边距触发——命中件
    // 落在某个 QDockWidget 的「非内容」区域（标题栏）时弹 createPopupMenu；
    // dock 内容子树与画布各有自己的右键语义，不抢。
    void contextMenuEvent(QContextMenuEvent *event) override;

  private:
    void buildShell();
    // ribbon 骨架：五个页签、「文件」菜单、右侧按钮组（面板/Web 服务）。
    // 页签内的命令组依赖页面板与服务，由 buildRibbonPanels 在 attachWorkflows
    // 末尾填充。
    void buildRibbon();
    void buildRibbonPanels(DataPage *data, PredictPage *predict, ConstraintPage *constraint,
                           ComposePage *compose, ValidatePage *validate,
                           PaleoEditingToolbar *editTb, WellCorrelationPanel *corrPanel);
    // 三个编图页各有一组「要素编辑」：动作与编辑条共享同一批 QAction；图层
    // 下拉共享编辑条下拉的 model，选择经 setCurrentLayer 回写。
    void addEditingPanel(SARibbonCategory *category, PaleoEditingToolbar *editTb);
    // 面板显隐菜单（QMainWindow::createPopupMenu 列出全部 dock 的
    // toggleViewAction）。右键 dock 标题栏与右上「面板」钮共用此入口。
    void showPanelMenu(const QPoint &globalPos);
    // m2(D) 页面图层档案：四个编图页切到/层位 chip 切换后重应用当前页档案
    // （QgisLayerProfileService::applyPageProfile）。数据页无档案，no-op。
    void applyCurrentPageProfile();
    // 预览分栏（§4 预览壳）：数据页地图在上预览在下；预览空态收成一行
    // 次级文字，首个标签打开时展开到约三分之一高度。
    void applyPreviewSplit();
    // 「导入工区文件夹」：分类确认表（可改类型）→ 两阶段导入 → 计数汇总，
    // 确认后只打开井口标签（§3/autoplan-design）。
    void runFolderImport(DataImportService *svc);
    // 已知目录的入口变体（「从工区文件夹新建」复用同一确认框流程）。
    void runFolderImportAt(DataImportService *svc, const QString &dir);
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

    QgisCanvasController *m_canvasCtl;
    QgisProjectService *m_projectSvc;
    QgisLayerService *m_layerSvc;
    ToolAvailabilityService *m_tools;
    SelectionContext *m_selection;

    // 阶段E 发布门重算钩子：attachMapping 安装，attachWorkflows 的
    // validationDone 连接在验证跑完后调它（残差覆盖是门的一条腿）。
    std::function<void()> m_refreshPublishGate;

    QStackedWidget *m_centerStack = nullptr;   // page0=startup, page1=workspace
    QStackedWidget *m_workspaceStack = nullptr; // page0=画布面（chip 条+画布），page1=数据面
    QSplitter *m_centerSplit = nullptr;        // 数据面：数据列表 / 数据预览 竖向分栏（§4）
    QWidget *m_dataListHost = nullptr;         // 分栏上格——DataPage 由 attachWorkflows 挂入
    DataPreviewTabs *m_previewTabs = nullptr;  // 分栏下格——只在数据管理页可见
    int m_userListWidth = -1;                  // 用户拖动分栏记忆宽度（绝不因双击数据项重设）
    bool m_previewExpanded = false;            // 首个标签打开后已给过 60%（D7 预算）
    bool m_previewMaximized = false;           // D7：预览最大化态（列表留一行壳）
    QList<int> m_preMaxSplitSizes;             // 最大化前的分栏尺寸（还原用）
    PaleoDockWidget *m_leftDock = nullptr;
    QDockWidget *m_rightDock = nullptr;
    PaleoDockWidget *m_bottomDock = nullptr;
    // ---- wave/layer-platform：左 dock 图层平台（面板 + 档案工具条 + 服务） ----
    LayerTreePanel *m_layerPanel = nullptr;
    LayerProfileBar *m_profileBar = nullptr;
    QgisLayerProfileService *m_profileSvc = nullptr;
    LayerPropertiesDialog *m_layerProps = nullptr;
    bool m_profileReplayQueued = false; // 层位切换后的页面档案重放去抖旗标
    seismic::SeismicSectionDockWidget *m_seismicSectionDock = nullptr;
    QDockWidget *m_seismic3dDock = nullptr;
    seismic::Seismic3DViewPanel *m_seismic3dPanel = nullptr;
    std::unique_ptr<seismic::SeismicTaskService> m_seismicTaskSvc;
    bool m_folderImportActive = false; // 文件夹导入期间抑制逐文件开预览标签
    PaleoTaskService *m_taskSvc = nullptr; // attachWorkflows 注入；空 → 导入走同步旧路径
    DataImportService *m_importSvc = nullptr; // attachWorkflows 注入；启动页「从工区文件夹新建」用
    // 壳唯一数据门面（W1）：dataPage 属性与 previewTabs 共用同一实例。
    PreviewDocService *m_previewDoc = nullptr;
    FolderImportWorkflow *m_folderImportWf = nullptr;   // W2 文件夹/单文件导入编排
    ProjectOpenWorkflow *m_projectOpenWf = nullptr;     // W2 打开/新建工程编排
    RegistrationWorkflow *m_registrationWf = nullptr;   // W3 临时配准编排
    // attachWorkflows 幂等守卫：该函数每次执行都清栈重建右栏页面、给底栏/
    // 状态栏加面板并往服务对象上叠信号连接，二次执行会重复建 dock/按钮并
    // 遗留悬空引用（后续用例段错误）。测试套件会二次触达同一窗口——
    // 入口早退，见 attachWorkflows 注释。
    bool m_workflowsAttached = false;
    PaleoDecorationManager *m_decorMgr = nullptr; // D11 临时配准水印等画布装饰
    int m_provisionalLayers = 0;   // 已上图的临时配准图层数（>0 → 水印）
    QString m_currentPage;
};
