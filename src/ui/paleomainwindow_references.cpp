// 层：视图
#include "paleomainwindow.h"
#include "paleomainwindow_internal.h"
#include "workbench/mappingreferencecontroller.h"
#include "workbench/workspacecomparison.h"
#include "pages/mappingreferencepanel.h"
#include "qgis/projectscope.h"
#include "qgis/qgisprojectservice.h"
#include <QCoreApplication>
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
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { savePlacement(); });
        connect(this, &QDockWidget::dockLocationChanged, this, [this] { scheduleSave(); });
        connect(this, &QDockWidget::topLevelChanged, this, [this] { scheduleSave(); });
        connect(this, &PaleoDockWidget::userVisibilityChanged, this, [this](bool visible) {
            if (!m_restoring && !m_scope.isEmpty())
                QSettings("paleo", "paleo").setValue(visibilityKey(), visible);
        });
    }

    void rememberGlobalLayout(bool restored) {
        m_defaultPlacement = placement();
        const bool standard = !isFloating() && m_window->dockWidgetArea(this) == Qt::BottomDockWidgetArea &&
                              m_window->tabifiedDockWidgets(this).isEmpty();
        m_defaultPlacement.insert("workspace", !restored || standard);
        m_defaultVisible = userWantsVisible();
    }

    void connectWorkspace(WorkspaceComparison *workspace, MappingEvidencePanel *panel) {
        if (!workspace) return;
        m_workspace = workspace;
        m_panel = panel;
        connect(panel, &MappingEvidencePanel::legendCreated, workspace, &WorkspaceComparison::refreshLegends);
        connect(workspace, &WorkspaceComparison::closeRequested, this, [this] { closeWorkspace(); });
        connect(workspace, &WorkspaceComparison::dockRequested, this, [this] {
            returnToDock();
            m_useWorkspace = false;
            show();
            raise();
            savePlacement();
        });
        connect(panel, &MappingEvidencePanel::workspaceRequested, this, [this] {
            m_useWorkspace = true;
            openWorkspace();
        });
        connect(panel, &MappingEvidencePanel::lastViewRemoved, this, [this] {
            if (inWorkspace()) closeWorkspace();
        });
        connect(panel, &MappingEvidencePanel::referenceRequested, this, [this](auto *, const QString &) {
            if (isVisible() && usesWorkspace() && !m_restoring) openWorkspace();
        });
        connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (visible && inWorkspace()) {
                returnToDock();
                m_useWorkspace = false;
                savePlacement();
            }
        });
        const auto rememberNative = [this] {
            if (!m_restoring && !m_scope.isEmpty() && !inWorkspace()) m_useWorkspace = false;
        };
        connect(this, &QDockWidget::dockLocationChanged, this, rememberNative);
        connect(this, &QDockWidget::topLevelChanged, this, rememberNative);
    }

    void present(bool sourceSelected) {
        if (usesWorkspace() && (sourceSelected || !m_panel->compact())) openWorkspace();
        else { show(); raise(); }
    }

    void finishSaving() {
        m_saveTimer.stop();
        savePlacement();
        m_restoring = true;
        returnToDock();
        m_scope.clear();
        m_window = nullptr;
    }

    void selectProject(const QString &scope) {
        if (!m_window) return;
        if (m_bound && scope == m_scope) return;
        m_saveTimer.stop();
        savePlacement();
        QScopedValueRollback restoring(m_restoring, true);
        returnToDock();
        m_scope = scope;
        m_bound = true;
        if (scope.isEmpty()) {
            setUserWantsVisible(false);
            return;
        }
        const QSettings settings("paleo", "paleo");
        const auto saved = settings.value(placementKey(), m_defaultPlacement).toMap();
        restorePlacement(saved);
        m_useWorkspace = saved.value("workspace", false).toBool();
        if (m_workspace) m_workspace->restoreSplit(saved.value("split").toByteArray());
        setUserWantsVisible(settings.value(visibilityKey(), m_defaultVisible).toBool());
        if (usesWorkspace() && saved.value("workspaceOpen").toBool()) openWorkspace();
    }

protected:
    bool event(QEvent *event) override {
        const auto type = event->type();
        const bool handled = PaleoDockWidget::event(event);
        if (type == QEvent::Move || type == QEvent::Resize) scheduleSave();
        else if ((type == QEvent::Hide || type == QEvent::Close) && m_saveTimer.isActive()) {
            m_saveTimer.stop();
            savePlacement();
        }
        return handled;
    }

private:
    bool inWorkspace() const { return m_workspace && m_workspace->comparing(); }
    bool usesWorkspace() const {
        return m_useWorkspace && m_workspace;
    }
    void openWorkspace() {
        if (!inWorkspace()) {
            setProgrammaticVisible(false);
            setUserWantsVisible(false);
            m_panel->setInWorkspace(true);
        }
        m_workspace->present(m_panel);
        savePlacement();
    }
    void returnToDock() {
        if (!inWorkspace()) return;
        m_workspace->release();
        m_panel->setInWorkspace(false);
        setWidget(m_panel);
        m_panel->show();
    }
    void closeWorkspace() {
        returnToDock();
        setProgrammaticVisible(false);
        setUserWantsVisible(false);
        if (!m_scope.isEmpty()) QSettings("paleo", "paleo").setValue(visibilityKey(), false);
        savePlacement();
    }
    QString visibilityKey() const { return QStringLiteral("mappingEvidence/visible/") + m_scope; }
    QString placementKey() const { return QStringLiteral("mappingEvidence/placement/") + m_scope; }
    void scheduleSave() {
        if (!m_restoring && !m_scope.isEmpty()) m_saveTimer.start();
    }
    QVariantMap placement() {
        const auto area = m_window->dockWidgetArea(this);
        QVariantMap value{{"area", int(area)}, {"floating", isFloating()},
                          {"workspace", m_useWorkspace}, {"workspaceOpen", inWorkspace()}};
        if (m_workspace) value.insert("split", m_workspace->splitState());
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
                m_window->addDockWidget(area, this,
                    area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea ? Qt::Vertical : Qt::Horizontal);
            if (!floating && peer && m_window->dockWidgetArea(peer) == area && !tabs.contains(peer))
                m_window->tabifyDockWidget(peer, this);
        }
        if (isFloating() != floating) setFloating(floating);
        const auto geometry = saved.value("geometry").toByteArray();
        if (floating && !geometry.isEmpty()) restoreGeometry(geometry);
    }
    QMainWindow *m_window;
    QPointer<WorkspaceComparison> m_workspace;
    MappingEvidencePanel *m_panel = nullptr;
    bool m_useWorkspace = true;
    QTimer m_saveTimer;
    QVariantMap m_defaultPlacement;
    QString m_scope;
    bool m_bound = false, m_restoring = false, m_defaultVisible = true;
};
}

void paleo::mainwindow_internal::finishReferenceDockLayout(QMainWindow *window)
{
    if (auto *dock = dynamic_cast<ReferenceDock *>(window->findChild<PaleoDockWidget *>(QStringLiteral("mappingReferenceDock"))))
        dock->finishSaving();
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
        auto *controller = new MappingReferenceController(
            *dock, *m_workbench, *m_canvasCtl, *m_layerSvc, m_projectSvc, *m_selection);
        const bool restored = restoreDockWidget(dock);
        if (!restored) {
            addDockWidget(Qt::BottomDockWidgetArea, dock);
            // The managed dock is registered above. Give the reference panel its
            // own row below the existing native bottom tab group.
            QMainWindow::addDockWidget(Qt::BottomDockWidgetArea, dock, Qt::Vertical);
        }
        dock->setUserWantsVisible(restored && !dock->isHidden());
        auto *workspace = findChild<WorkspaceComparison *>();
        if (workspace) workspace->setPrimaryLegend(m_decorMgr);
        dock->connectWorkspace(workspace, controller->panel());
        dock->rememberGlobalLayout(restored);
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
    if (!controller) return false;
    static_cast<ReferenceDock *>(dock)->present(!layerId.isEmpty());
    return controller->showSource(layerId, error);
}
