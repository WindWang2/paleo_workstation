#pragma once
#include <QMainWindow>
#include <QString>

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
class DataImportService;
class SeismicMapLink;

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

    // Swap right-dock placeholder panels for the real page panels (§42.2),
    // bound to the workflow orchestrators. Call after AppContext assembly.
    void attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                         CompositionWorkflow *compose, ValidationWorkflow *validate,
                         DataImportService *importSvc = nullptr,
                         SeismicMapLink *seismicLink = nullptr);

  private:
    void buildShell();

    QgisCanvasController *m_canvasCtl;
    QgisProjectService *m_projectSvc;
    QgisLayerService *m_layerSvc;
    ToolAvailabilityService *m_tools;
    SelectionContext *m_selection;

    QTabBar *m_workflowTabs = nullptr;
    QStackedWidget *m_centerStack = nullptr;   // page0=startup, page1=canvas
    QDockWidget *m_leftDock = nullptr;
    QDockWidget *m_rightDock = nullptr;
    QDockWidget *m_bottomDock = nullptr;
    QString m_currentPage;
};
