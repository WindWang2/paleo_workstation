// 层：视图
#pragma once
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>
#include <qgslayoutdesignerinterface.h>

class QAction;
class QDockWidget;
class QMainWindow;
class PaleoDockManager;
class QLabel;
class QMenu;
class QMenuBar;
class QSpinBox;
class QStatusBar;
class QToolBar;
class QgsFeature;
class QgsLayout;
class QgsLayoutItem;
class QgsLayoutRuler;
class QgsLayoutView;
class QgsLayoutViewTool;
class PaleoTaskService;
class QgsMasterLayoutInterface;
class QgsMessageBar;
class PaleoLayoutDesignerShell;
class PaleoLayoutExportActions;
class PaleoLayoutItemPanel;
class PaleoLayoutItemPalette;
class PaleoLayoutTemplates;
class PaleoLayoutUndoStack;

// ui/ — PaleoLayoutDesignerShell: bespoke designer host per ET9 audit.
// QGIS 4.x puts QgsLayoutDesignerInterface in qgis_gui precisely so embedders
// can host the designer widgets without src/app. This shell implements the
// interface surface minimally and hosts the gui-side view/ruler widgets.
//
// Structure mirrors upstream: like QgsAppLayoutDesignerInterface wrapping
// QgsLayoutDesignerDialog, PaleoShellDesignerInterface is a QObject adapter
// implementing QgsLayoutDesignerInterface over the PaleoLayoutDesignerShell
// dialog (two QObject bases on one class is not viable — moc requires a
// single QObject base).
//
// Subtask E composition (menu bar has EXACTLY four top-level menus):
//   &File     export PNG/PDF/SVG, template save/load + built-in page setups,
//             Close
//   &Items    add-element actions sourced from the palette's GUI-registry
//             metadata ids (same interactive path as the palette buttons),
//             plus the page-properties entry
//   &Layout   page navigation, an &Edit submenu (undo/redo via
//             PaleoLayoutUndoStack), a &View submenu (rulers + zoom), and the
//             &Atlas / &Report interface submenus
//   &Settings reserved
// The interface's seven menu accessors return real QMenus: layoutMenu() /
// itemsMenu() / settingsMenu() are the top-level ones, editMenu() / viewMenu()
// / atlasMenu() / reportMenu() are the Layout menu's submenus.

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

    // #85：注入任务服务后导出经任务池 worker（版面快照 → 后台重建导出）。
    void setTaskService( PaleoTaskService *service );

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

    // Menus — lazily created, real QMenu instances hosted in a QMenuBar whose
    // top level is exactly File / Items / Layout / Settings. layoutMenu(),
    // itemsMenu() and settingsMenu() ARE the Layout / Items / Settings top
    // level menus; the other accessors return Layout's submenus.
    QMenu *layoutMenu();
    QMenu *editMenu();
    QMenu *viewMenu();
    QMenu *itemsMenu();
    QMenu *atlasMenu();
    QMenu *reportMenu();
    QMenu *settingsMenu();

    //! The (non-interface) File top-level menu: export + template entries.
    QMenu *fileMenu();

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

    // Null until the first successful export through the shell's export
    // actions (the interface contract allows null when nothing was exported).
    QgsLayoutDesignerInterface::ExportResults *lastExportResults() const { return m_lastExportResults; }

  public slots:
    void showRulers( bool visible );

  private:
    void buildChrome();
    void buildFileMenu();
    void buildItemsMenu();
    void buildLayoutMenu();
    void connectLayoutSync();
    void updatePageNavigator();
    void gotoPage( int page );       //!< 0-based
    void openPageProperties();
    void refreshPanelAfterUndoRedo();

    QMenu *menuFor( QPointer<QMenu> &member, const QString &title );
    QMenu *submenuFor( QPointer<QMenu> &member, QMenu *parent, const QString &title );
    QToolBar *toolBarFor( QPointer<QToolBar> &member, const QString &objectName );

    QgsLayout *m_layout = nullptr;
    PaleoShellDesignerInterface *m_iface = nullptr;

    // --- subtask A/B/C/D components ----------------------------------------
    PaleoLayoutItemPalette *m_palette = nullptr;      // left element palette
    PaleoLayoutItemPanel *m_itemPanel = nullptr;      // right properties host
    PaleoLayoutExportActions *m_exportActions = nullptr;
    PaleoLayoutTemplates *m_templates = nullptr;
    PaleoLayoutUndoStack *m_undoStack = nullptr;

    QgsLayoutView *m_view = nullptr;
    QgsMessageBar *m_messageBar = nullptr;
    QStatusBar *m_statusBar = nullptr;

    QLabel *m_pageLabel = nullptr;
    QSpinBox *m_pageSpin = nullptr;
    QAction *m_prevPageAction = nullptr;
    QAction *m_nextPageAction = nullptr;

    QMenuBar *m_menuBar = nullptr;
    QWidget *m_toolBarRow = nullptr;
    QMainWindow *m_dockArea = nullptr;
    PaleoDockManager *m_dockManager = nullptr;

    QPointer<QMenu> m_fileMenu;
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

    QPointer<QgsLayoutViewTool> m_selectTool;
    QPointer<QgsLayoutViewTool> m_moveItemContentTool;
    QPointer<QgsLayoutViewTool> m_editNodesTool;

    QList<QPointer<QDockWidget>> m_extraDocks;
    bool m_atlasPreviewEnabled = false;

    QgsLayoutDesignerInterface::ExportResults *m_lastExportResults = nullptr;
};
