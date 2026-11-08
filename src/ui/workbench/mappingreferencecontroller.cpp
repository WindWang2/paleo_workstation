// 层：视图
#include "mappingreferencecontroller.h"

#include "../pages/mappingreferencepanel.h"
#include "domain/facieshierarchy.h"
#include "linkage/selectioncontext.h"
#include "qgis/mappingreferenceloader.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprojectservice.h"
#include "workflow/mappingworkbench.h"

#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>

namespace paleo::workbench {

MappingReferenceController::MappingReferenceController(
    QDockWidget &dock, MappingWorkbench &workbench, QgisCanvasController &canvas,
    QgisLayerService &layers, QgisProjectService *project, SelectionContext &selection)
    : QObject(&dock), m_workbench(&workbench), m_layers(&layers), m_project(project),
      m_selection(&selection),
      m_panel(new MappingEvidencePanel(canvas.canvas(), project ? project->project() : nullptr, &dock))
{
    dock.setWidget(m_panel);
    connect(&dock, &QDockWidget::visibilityChanged, m_panel, &MappingEvidencePanel::setViewActive);
    connect(m_panel, &MappingEvidencePanel::referenceRequested,
            this, &MappingReferenceController::loadReference);
    connect(&workbench, &MappingWorkbench::changed, this, &MappingReferenceController::refreshSources);
    connect(&selection, &SelectionContext::activeHorizonChanged,
            this, &MappingReferenceController::refreshSources);
    connect(&workbench, &MappingWorkbench::catalogBound, this, [this, &dock] {
        m_panel->setSources({}, {});
        m_panel->clearReferences(QCoreApplication::translate(
            "PaleoMainWindow", "工程已切换，请选择当前工程的参考证据。"));
        dock.hide();
    });
    refreshSources();
}

void MappingReferenceController::refreshSources()
{
    if (m_workbench && m_selection)
        m_panel->setSources(m_workbench->products(QString()), m_selection->activeHorizon());
}

void MappingReferenceController::loadReference(MappingReferencePanel *view, const QString &layerId)
{
    if (!view || !m_workbench || !m_layers)
        return;

    const auto declaration = m_workbench->declaration(layerId);
    auto *sourceLayer = m_layers->layer(layerId);
    const auto styleDirectory = m_project
        ? QFileInfo(m_project->projectPath()).dir().filePath("styles")
        : QString();
    auto *reference = MappingReferenceLoader::load(declaration, sourceLayer, styleDirectory);
    if (!reference) {
        view->clearReference(QCoreApplication::translate(
            "PaleoMainWindow", "参考图件无法读取，请检查文件是否存在，或选择其他图件。"));
        return;
    }

    auto metadata = m_workbench->versionForLayer(layerId).extra;
    if (sourceLayer)
        metadata.insert(QStringLiteral("pinned_levels"),
                        FaciesHierarchy::encodePins(m_workbench->pinnedLevels(layerId)));
    view->setReference(reference, metadata);
}

bool MappingReferenceController::showSource(const QString &layerId, QString *error)
{
    m_panel->setViewActive(true);
    if (layerId.isEmpty())
        return true;
    const auto *view = m_panel->showSource(layerId);
    if (view && view->hasReference())
        return true;
    if (error)
        *error = QCoreApplication::translate("PaleoMainWindow", "参考图件无法读取，请在参考面板中重新选择");
    return false;
}

} // namespace paleo::workbench
