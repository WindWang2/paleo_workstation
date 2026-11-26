// 层：视图
#include "paleomainwindow.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../services/paleotaskservice.h"
#include "../workflow/mappingworkbench.h"
#include "datapreview/datapreviewtabs.h"
#include "edittools/editingtoolbar.h"
#include "pages/composepage.h"
#include "pages/constraintpage.h"
#include "pages/datapage.h"
#include "pages/mappingworkbenchpage.h"
#include "pages/mappingtaskguide.h"
#include "pages/wellpredictionpanel.h"
#include "workbench/mappingworkbenchcommandcontroller.h"
#include "workbench/mappingreferencecontroller.h"
#include <QStatusBar>

namespace {
void bindWorkbenchPageHorizon(MappingWorkbenchPage *page, SelectionContext *selection) {
  const auto apply = [page, selection] {
    const auto unit = selection->activeUnit();
    const auto key = paleo::strat::analysisLayerKey(unit);
    page->setProperty("paleo.unitDisplayName", key.isEmpty() ? QString() : unit.displayName);
    page->setHorizon(key.isEmpty() ? selection->activeHorizon() : key);
  };
  apply();
  QObject::connect(selection, &SelectionContext::activeUnitChanged, page, apply);
  QObject::connect(selection, &SelectionContext::activeHorizonChanged, page, apply);
}
}

void PaleoMainWindow::bindWorkbenchSelection(MappingWorkbench *workbench) {
  if (m_selection) {
    workbench->setAnalysisUnit(m_selection->activeUnit());
    connect(m_selection, &SelectionContext::activeUnitChanged,
            workbench, &MappingWorkbench::setAnalysisUnit);
    connect(workbench, &MappingWorkbench::catalogBound, this, [this, workbench] {
      workbench->setAnalysisUnit(m_selection->activeUnit());
    });
  }
  // 编图过程不必需预先填写解释说明，不再强制开启阻断模式。
}

void PaleoMainWindow::attachWorkbenchCommands(
    MappingWorkbench *workbench, const QList<MappingWorkbenchPage *> &pages,
    PaleoEditingToolbar *edit, ConstraintPage *constraint, const WorkbenchWells &wells,
    const WorkbenchLegendFn &legend, const WorkbenchShowFn &show, const WorkbenchShowFn &compare,
    const std::shared_ptr<QHash<QString, QString>> &shown) {
  using paleo::workbench::MappingWorkbenchCommandController;
  using paleo::workbench::MappingWorkbenchImpactFeedback;
  auto *impact = new MappingWorkbenchImpactFeedback(workbench, statusBar(), this);
  MappingWorkbenchCommandController::Context context;
  context.workbench = workbench;
  context.dialogParent = this;
  context.statusBar = statusBar();
  context.canvasCtl = m_canvasCtl;
  context.layerSvc = m_layerSvc;
  context.impact = impact;
  context.factorBusy = [this] { return m_factorTask && m_factorTask->running(); };
  context.taskService = [this] { return m_taskSvc; };
  context.edit = edit;
  context.constraint = constraint;
  context.wellPanel = wells.panel;
  context.wellDock = wells.dock;
  context.legend = legend;
  context.show = show;
  context.compare = compare;
  context.comparePrimary = [this, compare](const QString &id, QString *error) {
    auto *dock = prepareMappingReferenceDock();
    auto *controller = dock ? dock->findChild<paleo::workbench::MappingReferenceController *>() : nullptr;
    if (!controller) return compare(id, error);
    for (const auto &candidate : controller->comparisonSources(id)) {
      if (compare(candidate, error)) {
        if (error) error->clear();
        return true;
      }
    }
    return false;
  };
  context.openWells = wells.openWells;
  context.beginEdit = wells.beginEdit;
  context.shown = shown;
  context.navigation.showAttributeTable = [this](const QString &layer) { showAttributeTable(layer); };
  context.navigation.openGraph = [this](const QString &asset, const QString &version) {
    openDataGraph(asset, version);
  };
  context.navigation.focusCatalogVersion = [this](const QString &asset, const QString &version) {
    auto *data = findChild<DataPage *>();
    if (!data) return false;
    showPage(QStringLiteral("data"));
    if (m_previewTabs) m_previewTabs->openVersion(version);
    data->focusVersion(asset, version);
    return true;
  };
  context.navigation.exportMap = [this] {
    if (auto *compose = findChild<ComposePage *>())
      emit compose->exportPdfRequested();
  };
  // C-W2：宿主状态钩子——触发时刻现读（只读翻转/工程切换即时生效）。
  context.projectSession = [this] { return m_projectSvc ? m_projectSvc->sessionId() : 0; };
  context.projectReadOnly = [this] { return isProjectReadOnly(); };
  context.projectSwitching = [this] { return m_projectSvc && m_projectSvc->isOpening(); };
  for (auto *page : pages) {
    if (m_selection) bindWorkbenchPageHorizon(page, m_selection);
    impact->addPage(page);
    if (auto *guide = page->findChild<MappingTaskGuide *>()) guide->watchEditor(edit);
    context.page = page;
    auto *commands = new MappingWorkbenchCommandController(context, page);
    connect(page, &MappingWorkbenchPage::commandRequested, commands,
            &MappingWorkbenchCommandController::handleCommand);
  }
}
