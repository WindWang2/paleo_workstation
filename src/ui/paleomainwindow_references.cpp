// 层：视图
#include "paleomainwindow.h"
#include "workbench/mappingreferencecontroller.h"
#include "qgis/projectscope.h"
#include "qgis/qgisprojectservice.h"
#include <QEvent>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTimer>
#include "workflow/mappingworkbench.h"

namespace {
class ReferenceDock final : public PaleoDockWidget {
public:
    ReferenceDock(const QString &title, QMainWindow *window) : PaleoDockWidget(title, window), m_window(window) {
        m_saveTimer.setSingleShot(true);
        m_saveTimer.setInterval(160);
        connect(&m_saveTimer, &QTimer::timeout, this, [this] { savePlacement(); });
        connect(this, &QDockWidget::dockLocationChanged, this, [this] { scheduleSave(); });
        connect(this, &QDockWidget::topLevelChanged, this, [this] { scheduleSave(); });
        connect(this, &PaleoDockWidget::userVisibilityChanged, this, [this](bool visible) {
            if (!m_restoring && !m_scope.isEmpty())
                QSettings("paleo", "paleo").setValue(visibilityKey(), visible);
        });
    }

    void rememberGlobalLayout() {
        m_defaultPlacement = placement();
        m_defaultVisible = userWantsVisible();
    }

    void selectProject(const QString &scope) {
        if (m_bound && scope == m_scope) return;
        m_saveTimer.stop();
        savePlacement();
        QScopedValueRollback restoring(m_restoring, true);
        m_scope = scope;
        m_bound = true;
        if (scope.isEmpty()) {
            setUserWantsVisible(false);
            return;
        }
        const QSettings settings("paleo", "paleo");
        const auto saved = settings.value(placementKey(), m_defaultPlacement).toMap();
        restorePlacement(saved);
        setUserWantsVisible(settings.value(visibilityKey(), m_defaultVisible).toBool());
    }

protected:
    bool event(QEvent *event) override {
        const auto type = event->type();
        const bool handled = PaleoDockWidget::event(event);
        if (type == QEvent::Move || type == QEvent::Resize) scheduleSave();
        else if (type == QEvent::Close) savePlacement();
        return handled;
    }

private:
    QString visibilityKey() const { return QStringLiteral("mappingEvidence/visible/") + m_scope; }
    QString placementKey() const { return QStringLiteral("mappingEvidence/placement/") + m_scope; }
    void scheduleSave() {
        if (!m_restoring && !m_scope.isEmpty()) m_saveTimer.start();
    }
    QVariantMap placement() {
        const auto area = m_window->dockWidgetArea(this);
        QVariantMap value{{"area", int(area)}, {"floating", isFloating()}};
        if (isFloating()) value.insert("geometry", saveGeometry());
        else {
            const auto peers = m_window->tabifiedDockWidgets(this);
            for (auto *peer : peers) if (!peer->objectName().isEmpty()) {
                value.insert("tabPeer", peer->objectName());
                break;
            }
        }
        return value;
    }
    void savePlacement() {
        if (!m_restoring && !m_scope.isEmpty())
            QSettings("paleo", "paleo").setValue(placementKey(), placement());
    }
    void restorePlacement(const QVariantMap &saved) {
        const auto area = static_cast<Qt::DockWidgetArea>(saved.value("area").toInt());
        const auto tabName = saved.value("tabPeer").toString();
        auto *peer = tabName.isEmpty() ? nullptr : m_window->findChild<QDockWidget *>(tabName, Qt::FindDirectChildrenOnly);
        const bool floating = saved.value("floating").toBool();
        if (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea ||
            area == Qt::BottomDockWidgetArea || area == Qt::TopDockWidgetArea) {
            const auto tabs = m_window->tabifiedDockWidgets(this);
            if (m_window->dockWidgetArea(this) != area || (!floating && !peer && !tabs.isEmpty()))
                m_window->addDockWidget(area, this);
            if (!floating && peer && m_window->dockWidgetArea(peer) == area && !tabs.contains(peer))
                m_window->tabifyDockWidget(peer, this);
        }
        if (isFloating() != floating) setFloating(floating);
        const auto geometry = saved.value("geometry").toByteArray();
        if (floating && !geometry.isEmpty()) restoreGeometry(geometry);
    }
    QMainWindow *m_window;
    QTimer m_saveTimer;
    QVariantMap m_defaultPlacement;
    QString m_scope;
    bool m_bound = false, m_restoring = false, m_defaultVisible = true;
};
}

PaleoDockWidget *PaleoMainWindow::prepareMappingReferenceDock()
{
    if (!m_workbench || !m_canvasCtl || !m_layerSvc || !m_selection)
        return nullptr;

    using paleo::workbench::MappingReferenceController;
    auto *dock = dynamic_cast<ReferenceDock *>(findChild<PaleoDockWidget *>(QStringLiteral("mappingReferenceDock")));
    if (!dock) {
        dock = new ReferenceDock(tr("参考证据"), this);
        dock->setObjectName(QStringLiteral("mappingReferenceDock"));
        dock->setProperty("paleo.mappingReference", true);
        new MappingReferenceController(
            *dock, *m_workbench, *m_canvasCtl, *m_layerSvc, m_projectSvc, *m_selection);
        const bool restored = restoreDockWidget(dock);
        if (!restored) addDockWidget(Qt::BottomDockWidgetArea, dock);
        dock->setUserWantsVisible(!restored || !dock->isHidden());
        dock->rememberGlobalLayout();
        connect(m_workbench, &MappingWorkbench::catalogBound, dock, [this, dock] {
            QTimer::singleShot(0, dock, [this, dock] {
                prepareMappingReferenceDock();
                const bool editingPage = m_currentPage == "predict" || m_currentPage == "constraint" || m_currentPage == "compose";
                dock->setProgrammaticVisible(editingPage && dock->userWantsVisible());
            });
        });
    }
    const QString scope = paleoProjectScopeKey(m_projectSvc ? m_projectSvc->project() : nullptr);
    dock->selectProject(scope);
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
