#pragma once
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>
#include <qgslayoutdesignerinterface.h>

class QDockWidget;
class QMenu;
class QMenuBar;
class QToolBar;
class QgsFeature;
class QgsLayout;
class QgsLayoutItem;
class QgsLayoutRuler;
class QgsLayoutView;
class QgsLayoutViewTool;
class QgsMasterLayoutInterface;
class QgsMessageBar;
class PaleoLayoutDesignerShell;

// ui/ — PaleoLayoutDesignerShell: bespoke designer host per ET9 audit.
// QGIS 4.x puts QgsLayoutDesignerInterface in qgis_gui precisely so embedders
// can host the designer widgets without src/app. This shell implements the
// interface surface minimally and hosts the gui-side view/ruler widgets.
// The full chrome (item properties docks, atlas tools) is assembled
// incrementally — P1 ships: view canvas + rulers + menus/toolbars + undo +
// save/close; item docks land behind usage data.
//
// Structure mirrors upstream: like QgsAppLayoutDesignerInterface wrapping
// QgsLayoutDesignerDialog, PaleoShellDesignerInterface is a QObject adapter
// implementing QgsLayoutDesignerInterface over the PaleoLayoutDesignerShell
// dialog (two QObject bases on one class is not viable — moc requires a
// single QObject base).

class PaleoShellDesignerInterface : public QgsLayoutDesignerInterface
{
    Q_OBJECT

  public:
    explicit PaleoShellDesignerInterface( PaleoLayoutDesignerShell *shell );

    QgsLayout *layout() override;
    QgsMasterLayoutInterface *masterLayout() override;
    QWidget *window() override;
    QgsLayoutView *view() override;
    QgsMessageBar *messageBar() override;

    void selectItems( const QList<QgsLayoutItem *> &items ) override;
    void setAtlasPreviewEnabled( bool enabled ) override;
    bool atlasPreviewEnabled() const override;
    void setAtlasFeature( const QgsFeature &feature ) override;
    void showItemOptions( QgsLayoutItem *item, bool bringPanelToFront = true ) override;

    QMenu *layoutMenu() override;
    QMenu *editMenu() override;
    QMenu *viewMenu() override;
    QMenu *itemsMenu() override;
    QMenu *atlasMenu() override;
    QMenu *reportMenu() override;
    QMenu *settingsMenu() override;

    QToolBar *layoutToolbar() override;
    QToolBar *navigationToolbar() override;
    QToolBar *actionsToolbar() override;
    QToolBar *atlasToolbar() override;

    void addDockWidget( Qt::DockWidgetArea area, QDockWidget *dock ) override;
    void removeDockWidget( QDockWidget *dock ) override;
    void activateTool( StandardTool tool ) override;
    QgsLayoutDesignerInterface::ExportResults *lastExportResults() const override;

  public slots:
    void close() override;
    void showRulers( bool visible ) override;

  private:
    PaleoLayoutDesignerShell *m_shell = nullptr;
};

class PaleoLayoutDesignerShell : public QDialog
{
    Q_OBJECT

  public:
    explicit PaleoLayoutDesignerShell( QgsLayout *layout, QWidget *parent = nullptr );
    ~PaleoLayoutDesignerShell() override;

    //! The QgsLayoutDesignerInterface view of this shell (adapter, owned).
    QgsLayoutDesignerInterface *designerInterface() { return m_iface; }

    // --- designer host API (what the interface adapter forwards to) ---
    QgsLayout *layout() const { return m_layout; }
    QgsMasterLayoutInterface *masterLayout() const;
    QgsLayoutView *view() const { return m_view; }
    QgsMessageBar *messageBar() const { return m_messageBar; }

    void selectItems( const QList<QgsLayoutItem *> &items );
    void setAtlasPreviewEnabled( bool enabled );
    bool atlasPreviewEnabled() const { return m_atlasPreviewEnabled; }
    void setAtlasFeature( const QgsFeature &feature );
    void showItemOptions( QgsLayoutItem *item, bool bringPanelToFront = true );

    // Menus — lazily created, real QMenu instances hosted in a QMenuBar.
    QMenu *layoutMenu();
    QMenu *editMenu();
    QMenu *viewMenu();
    QMenu *itemsMenu();
    QMenu *atlasMenu();
    QMenu *reportMenu();
    QMenu *settingsMenu();

    // Toolbars — lazily created, real QToolBar instances in a toolbar row.
    QToolBar *layoutToolbar();
    QToolBar *navigationToolbar();
    QToolBar *actionsToolbar();
    QToolBar *atlasToolbar();

    // Docks — real QDockWidgets adopted into the shell's side dock area.
    void addDockWidget( Qt::DockWidgetArea area, QDockWidget *dock );
    void removeDockWidget( QDockWidget *dock );

    // Standard tools — backed by real QgsLayoutViewTool instances.
    void activateTool( QgsLayoutDesignerInterface::StandardTool tool );

    // No export pipeline yet — always nullptr.
    QgsLayoutDesignerInterface::ExportResults *lastExportResults() const { return nullptr; }

  public slots:
    void showRulers( bool visible );

  private:
    QMenu *menuFor( QPointer<QMenu> &member, const QString &title );
    QToolBar *toolBarFor( QPointer<QToolBar> &member, const QString &objectName );

    QgsLayout *m_layout = nullptr;
    PaleoShellDesignerInterface *m_iface = nullptr;

    QgsLayoutView *m_view = nullptr;
    QgsMessageBar *m_messageBar = nullptr;

    QMenuBar *m_menuBar = nullptr;
    QWidget *m_toolBarRow = nullptr;
    QWidget *m_dockArea = nullptr;

    QPointer<QMenu> m_layoutMenu;
    QPointer<QMenu> m_editMenu;
    QPointer<QMenu> m_viewMenu;
    QPointer<QMenu> m_itemsMenu;
    QPointer<QMenu> m_atlasMenu;
    QPointer<QMenu> m_reportMenu;
    QPointer<QMenu> m_settingsMenu;

    QPointer<QToolBar> m_layoutToolbar;
    QPointer<QToolBar> m_navigationToolbar;
    QPointer<QToolBar> m_actionsToolbar;
    QPointer<QToolBar> m_atlasToolbar;

    QgsLayoutRuler *m_horizontalRuler = nullptr;
    QgsLayoutRuler *m_verticalRuler = nullptr;

    QPointer<QgsLayoutViewTool> m_moveItemContentTool;
    QPointer<QgsLayoutViewTool> m_editNodesTool;

    QList<QPointer<QDockWidget>> m_extraDocks;
    bool m_atlasPreviewEnabled = false;
};
