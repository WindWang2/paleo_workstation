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
#include <QMainWindow>
#include <QMap>

namespace paleo::workbench {

MappingReferenceController::MappingReferenceController(
    QDockWidget &dock, MappingWorkbench &workbench, QgisCanvasController &canvas,
    QgisLayerService &layers, QgisProjectService *project, SelectionContext &selection)
    : QObject(&dock), m_workbench(&workbench), m_layers(&layers), m_project(project),
      m_selection(&selection),
      m_panel(new MappingEvidencePanel(canvas.canvas(), project ? project->project() : nullptr, &dock))
{
    dock.setWidget(m_panel);
    const auto sizeDock = [&dock, panel = m_panel] {
        auto *main = qobject_cast<QMainWindow *>(dock.parentWidget());
        if (!main) return;
        const auto area = main->dockWidgetArea(&dock);
        const bool horizontal = !dock.isFloating() && (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea);
        const auto previous = dock.property("referenceFolded");
        const bool fold = panel->compact();
        dock.setProperty("referenceFolded", fold);
        if (!horizontal || !dock.isVisible()) return;
        if (fold) {
            if (previous.isValid() && !previous.toBool()) dock.setProperty("referenceExpandedHeight", dock.height());
            // A custom layout may put the log or a different dock in this row.
            // Its height remains under the user's control.
            const auto peers = main->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
            for (auto *peer : peers) {
                if (peer != &dock && !peer->isHidden() && !peer->isFloating() && main->dockWidgetArea(peer) == area &&
                    peer->geometry().bottom() >= dock.geometry().top() && peer->geometry().top() <= dock.geometry().bottom())
                    return;
            }
            main->resizeDocks({&dock}, {dock.minimumSizeHint().height()}, Qt::Vertical);
        } else if (previous.toBool()) {
            const int saved = dock.property("referenceExpandedHeight").toInt();
            int initial = panel->sizeHint().height();
            if (auto *central = main->centralWidget())
                initial = qMin(initial, (central->height() + dock.height()) / 2);
            main->resizeDocks({&dock}, {saved > 0 ? saved : initial}, Qt::Vertical);
        }
    };
    connect(m_panel, &MappingEvidencePanel::compactChanged, &dock, sizeDock);
    connect(&dock, &QDockWidget::topLevelChanged, &dock, sizeDock);
    connect(&dock, &QDockWidget::dockLocationChanged, &dock, sizeDock);
    connect(&dock, &QDockWidget::visibilityChanged, &dock, [sizeDock](bool visible) { if (visible) sizeDock(); });
    connect(m_panel, &MappingEvidencePanel::referenceRequested,
            this, &MappingReferenceController::loadReference);
    connect(&workbench, &MappingWorkbench::changed, this, &MappingReferenceController::refreshSources);
    connect(&selection, &SelectionContext::activeHorizonChanged,
            this, &MappingReferenceController::refreshSources);
    connect(&workbench, &MappingWorkbench::catalogBound, this, [this] {
        m_primaryId.clear();
        m_userSource.clear();
        m_panel->reloadProjectLayout();
        m_panel->clearReferences(QCoreApplication::translate(
            "PaleoMainWindow", "工程已切换，请选择当前工程的参考证据。"));
    });
    m_panel->enableCompactDock();
    connect(m_panel, &MappingEvidencePanel::sourceChosen, this, [this](const QString &source) {
        if (!m_primaryId.isEmpty()) m_userSource.insert(m_primaryId, source);
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

QStringList MappingReferenceController::comparisonSources(const QString &primaryId)
{
    m_primaryId = primaryId;
    const auto current = m_workbench->versionForLayer(primaryId);
    QStringList candidates;
    const auto chosen = m_userSource.value(primaryId);
    for (const auto *view : m_panel->views())
        if (view->hasReference() && view->sourceId() == chosen) candidates << chosen;
    QStringList versions{current.extra.value("cluster_patches_version").toString(),
                         current.extra.value("cluster_raster_version").toString()};
    versions << current.extra.value("computational_parent_versions").toStringList() << current.parentVersionIds;
    // Resolve only declared spatial versions. Opening a reference never materializes a main-map layer.
    QHash<QString, QString> registered;
    QMap<int, QString> earlier;
    const auto products = m_workbench->products(QString());
    for (const auto &item : products) {
        const auto row = item.toMap();
        if (row.value("draft").toBool() || (row.value("type") != "vector" && row.value("type") != "raster")) continue;
        const QString id = row.value("id").toString(), version = row.value("version_id").toString();
        if (version.isEmpty() || version == current.id) continue;
        if (!registered.contains(version) || id == QStringLiteral("product.") + version)
            registered.insert(version, id);
        const int revision = row.value("version").toInt();
        if (!current.assetId.isEmpty() && row.value("asset_id") == current.assetId &&
            revision > 0 && revision < current.versionNumber) earlier.insert(revision, version);
    }
    for (auto it = earlier.cend(); it != earlier.cbegin();) versions << (--it).value();
    for (const auto &version : versions)
        if (registered.contains(version)) candidates << registered.value(version);
    candidates << primaryId;
    candidates.removeDuplicates();
    return candidates;
}

} // namespace paleo::workbench
