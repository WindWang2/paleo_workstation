// 层：视图
#include "paleomainwindow.h"
#include "workbench/mappingreferencecontroller.h"
#include "qgis/projectscope.h"
#include "qgis/qgisprojectservice.h"
#include <QSettings>
#include <QTimer>
#include "workflow/mappingworkbench.h"

PaleoDockWidget *PaleoMainWindow::prepareMappingReferenceDock()
{
    if (!m_workbench || !m_canvasCtl || !m_layerSvc || !m_selection)
        return nullptr;

    using paleo::workbench::MappingReferenceController;
    auto *dock = findChild<PaleoDockWidget *>(QStringLiteral("mappingReferenceDock"));
    if (!dock) {
        dock = new PaleoDockWidget(tr("参考证据"), this);
        dock->setObjectName(QStringLiteral("mappingReferenceDock"));
        dock->setProperty("paleo.mappingReference", true);
        new MappingReferenceController(
            *dock, *m_workbench, *m_canvasCtl, *m_layerSvc, m_projectSvc, *m_selection);
        const bool restored = restoreDockWidget(dock);
        if (!restored) addDockWidget(Qt::BottomDockWidgetArea, dock);
        dock->setUserWantsVisible(!restored || !dock->isHidden());
        connect(dock, &PaleoDockWidget::userVisibilityChanged, dock, [dock](bool visible) {
            const auto key = dock->property("referenceVisibilityKey").toString();
            if (!key.isEmpty()) QSettings("paleo", "paleo").setValue(key, visible);
        });
        connect(m_workbench, &MappingWorkbench::catalogBound, dock, [this, dock] {
            QTimer::singleShot(0, dock, [this, dock] {
                prepareMappingReferenceDock();
                const bool editingPage = m_currentPage == "predict" || m_currentPage == "constraint" || m_currentPage == "compose";
                dock->setProgrammaticVisible(editingPage && dock->userWantsVisible());
            });
        });
    }
    const QString scope = paleoProjectScopeKey(m_projectSvc ? m_projectSvc->project() : nullptr);
    const QString key = scope.isEmpty() ? QString() : QStringLiteral("mappingEvidence/visible/") + scope;
    const auto oldKey = dock->property("referenceVisibilityKey").toString();
    if (oldKey != key) {
        const bool fallback = oldKey.isEmpty() ? dock->userWantsVisible() : true;
        dock->setUserWantsVisible(!key.isEmpty() && QSettings("paleo", "paleo").value(key, fallback).toBool());
        dock->setProperty("referenceVisibilityKey", key);
    }
    return dock;
}

bool PaleoMainWindow::showMappingReference(const QString &layerId, QString *error)
{
    auto *dock = prepareMappingReferenceDock();
    if (!dock) return false;
    auto *controller = dock->findChild<paleo::workbench::MappingReferenceController *>();
    dock->show();
    dock->raise();
    return controller && controller->showSource(layerId, error);
}
