#pragma once
#include <QMainWindow>
#include <QString>
#include <functional>

class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class ToolAvailabilityService;
class SelectionContext;
class QTabBar;
class QDockWidget;
class QStackedWidget;
class PredictionWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
class MappingWorkflow;
class MapVersionController;
class MapVersionStore;
class ProjectDataFacade;
class DataCatalog;
class DataImportService;
class SeismicMapLink;
class QgisProcessingService;
class QgisLayoutService;
class QgisEditingService;
class PaleoProjectStore;
class QCloseEvent;

// ui/ — PaleoMainWindow: the five-page workflow shell (§42).
// Anatomy: left = layer tree dock; center = canvas (+ startup page stacked under);
// right = page-specific dock; bottom = log/tasks tabs; top = workflow chain tab bar.
// UI never touches Qgs* beyond canvas/layer-tree widgets (§25).
class PaleoMainWindow : public QMainWindow
{
  Q_OBJECT
  public:
    PaleoMainWindow(QgisCanvasController *canvasCtl,
                    QgisProjectService *projectSvc,
                    QgisLayerService *layerSvc,
                    ToolAvailabilityService *tools,
                    SelectionContext *selection,
                    QWidget *parent = nullptr);

    // Page ids: "data" | "predict" | "constraint" | "compose" | "validate" (+ "startup")
    void showPage(const QString &pageId);
    QString currentPage() const { return m_currentPage; }
    void showStartup();            // first-run: recent projects + new/open
    void onProjectOpened();        // called after project opens: swap startup->workspace

    // §42 shell persistence — geometry/dock layout/last page in QSettings;
    // canvas extent is per-project and lives inside the .qgz (paleo props).
    void saveWindowState();
    void restoreWindowState();
    void saveCanvasExtent();       // write current canvas extent into the project
    void restoreCanvasExtent();    // apply stored extent after project opens

    // Swap right-dock placeholder panels for the real page panels (§42.2),
    // bound to the workflow orchestrators. Call after AppContext assembly.
    void attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                         CompositionWorkflow *compose, ValidationWorkflow *validate,
                         DataImportService *importSvc = nullptr,
                         SeismicMapLink *seismicLink = nullptr,
                         QgisProcessingService *procSvc = nullptr,
                         PaleoProjectStore *store = nullptr,
                         QgisEditingService *editSvc = nullptr,
                         QgisLayoutService *layoutSvc = nullptr);

    // wave/mapping-pipeline 阶段C+E：编图链 / 层位图导出 / 版本状态机接到
    // ③编图页。独立于 attachWorkflows，避免动其签名。Call after attachWorkflows.
    // catalog 用于导出产物登记（OUTPUT 资产，发布门的前提）。
    void attachMapping(MappingWorkflow *mapping, MapVersionController *versions,
                       MapVersionStore *versionStore, ProjectDataFacade *projectData,
                       DataCatalog *catalog = nullptr);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void buildShell();

    QgisCanvasController *m_canvasCtl;
    QgisProjectService *m_projectSvc;
    QgisLayerService *m_layerSvc;
    ToolAvailabilityService *m_tools;
    SelectionContext *m_selection;

    // 阶段E 发布门重算钩子：attachMapping 安装，attachWorkflows 的
    // validationDone 连接在验证跑完后调它（残差覆盖是门的一条腿）。
    std::function<void()> m_refreshPublishGate;

    QTabBar *m_workflowTabs = nullptr;
    QStackedWidget *m_centerStack = nullptr;   // page0=startup, page1=canvas
    QDockWidget *m_leftDock = nullptr;
    QDockWidget *m_rightDock = nullptr;
    QDockWidget *m_bottomDock = nullptr;
    QString m_currentPage;
};
