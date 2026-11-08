// 层：视图
#include "paleomainwindow.h"
#include "workbench/mappingreferencecontroller.h"
#include "workflow/mappingworkbench.h"

bool PaleoMainWindow::showMappingReference(const QString &layerId, QString *error)
{
    if (!m_workbench || !m_canvasCtl || !m_layerSvc || !m_selection)
        return false;

    using paleo::workbench::MappingReferenceController;
    auto *dock = findChild<PaleoDockWidget *>(QStringLiteral("mappingReferenceDock"));
    auto *controller = dock ? dock->findChild<MappingReferenceController *>() : nullptr;
    const bool created = !dock;
    if (created) {
        dock = new PaleoDockWidget(tr("参考证据"), this);
        dock->setObjectName(QStringLiteral("mappingReferenceDock"));
        dock->setProperty("paleo.mappingReference", true);
        controller = new MappingReferenceController(
            *dock, *m_workbench, *m_canvasCtl, *m_layerSvc, m_projectSvc, *m_selection);
        addDockWidget(Qt::BottomDockWidgetArea, dock);
    }

    dock->show();
    dock->raise();
    if (created && !dock->isFloating() && dockWidgetArea(dock) == Qt::BottomDockWidgetArea)
        resizeDocks({dock}, {qMin(360, qMax(240, height() / 2))}, Qt::Vertical);
    return controller && controller->showSource(layerId, error);
}
