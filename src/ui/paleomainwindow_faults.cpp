// 层：视图
// paleomainwindow_faults — goal/fault-interpretation 壳接线：
// 剖面 dock 挂断层编排器 + 右栏断层管理面板 dock。面板是纯视图，
// 意图全走 FaultInterpretationController。
#include "faults/faultmanagerpanel.h"
#include "paleomainwindow.h"
#include "seismicsection/seismicsectiondockwidget.h"
#include "workflow/faultinterpretationcontroller.h"

#include <QDockWidget>

void PaleoMainWindow::attachFaults(paleo::fault::FaultInterpretationController *controller)
{
    if (m_seismicSectionDock)
        m_seismicSectionDock->setFaultController(controller); // 剖面拾取 → FaultSet

    if (!m_faultPanelDock) {
        m_faultPanel = new paleo::fault::FaultManagerPanel(controller, m_canvasCtl, this);
        m_faultPanelDock = new PaleoDockWidget(tr("断层解释"), this);
        m_faultPanelDock->setObjectName(QStringLiteral("faultPanelDock"));
        m_faultPanelDock->setWidget(m_faultPanel);
        addDockWidget(Qt::RightDockWidgetArea, m_faultPanelDock);
        if (m_rightDock)
            tabifyDockWidget(m_rightDock, m_faultPanelDock);
        m_faultPanelDock->hide(); // 跟其他专业 dock 同惯例：按需唤出
    }
}
