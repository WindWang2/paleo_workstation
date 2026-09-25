#include "layoutdesignershell.h"

#include <QAction>
#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

#include <qgsfeature.h>
#include <qgslayout.h>
#include <qgslayoutatlas.h>
#include <qgslayoutitem.h>
#include <qgslayoutruler.h>
#include <qgslayoutundostack.h>
#include <qgslayoutview.h>
#include <qgslayoutviewtooleditnodes.h>
#include <qgslayoutviewtoolmoveitemcontent.h>
#include <qgslayoutviewtoolselect.h>
#include <qgsmasterlayoutinterface.h>
#include <qgsmessagebar.h>
#include <qgsprintlayout.h>

// ---------------------------------------------------------------------------
// PaleoShellDesignerInterface — adapter mirroring QgsAppLayoutDesignerInterface:
// a single-QObject-base QgsLayoutDesignerInterface that forwards to the shell.
// ---------------------------------------------------------------------------

PaleoShellDesignerInterface::PaleoShellDesignerInterface( PaleoLayoutDesignerShell *shell )
  : QgsLayoutDesignerInterface( shell )
  , m_shell( shell )
{
}

QgsLayout *PaleoShellDesignerInterface::layout() { return m_shell->layout(); }
QgsMasterLayoutInterface *PaleoShellDesignerInterface::masterLayout() { return m_shell->masterLayout(); }
QWidget *PaleoShellDesignerInterface::window() { return m_shell; }
QgsLayoutView *PaleoShellDesignerInterface::view() { return m_shell->view(); }
QgsMessageBar *PaleoShellDesignerInterface::messageBar() { return m_shell->messageBar(); }

void PaleoShellDesignerInterface::selectItems( const QList<QgsLayoutItem *> &items ) { m_shell->selectItems( items ); }
void PaleoShellDesignerInterface::setAtlasPreviewEnabled( bool enabled ) { m_shell->setAtlasPreviewEnabled( enabled ); }
bool PaleoShellDesignerInterface::atlasPreviewEnabled() const { return m_shell->atlasPreviewEnabled(); }
void PaleoShellDesignerInterface::setAtlasFeature( const QgsFeature &feature ) { m_shell->setAtlasFeature( feature ); }
void PaleoShellDesignerInterface::showItemOptions( QgsLayoutItem *item, bool bringPanelToFront ) { m_shell->showItemOptions( item, bringPanelToFront ); }

QMenu *PaleoShellDesignerInterface::layoutMenu() { return m_shell->layoutMenu(); }
QMenu *PaleoShellDesignerInterface::editMenu() { return m_shell->editMenu(); }
QMenu *PaleoShellDesignerInterface::viewMenu() { return m_shell->viewMenu(); }
QMenu *PaleoShellDesignerInterface::itemsMenu() { return m_shell->itemsMenu(); }
QMenu *PaleoShellDesignerInterface::atlasMenu() { return m_shell->atlasMenu(); }
QMenu *PaleoShellDesignerInterface::reportMenu() { return m_shell->reportMenu(); }
QMenu *PaleoShellDesignerInterface::settingsMenu() { return m_shell->settingsMenu(); }

QToolBar *PaleoShellDesignerInterface::layoutToolbar() { return m_shell->layoutToolbar(); }
QToolBar *PaleoShellDesignerInterface::navigationToolbar() { return m_shell->navigationToolbar(); }
QToolBar *PaleoShellDesignerInterface::actionsToolbar() { return m_shell->actionsToolbar(); }
QToolBar *PaleoShellDesignerInterface::atlasToolbar() { return m_shell->atlasToolbar(); }

void PaleoShellDesignerInterface::addDockWidget( Qt::DockWidgetArea area, QDockWidget *dock ) { m_shell->addDockWidget( area, dock ); }
void PaleoShellDesignerInterface::removeDockWidget( QDockWidget *dock ) { m_shell->removeDockWidget( dock ); }
void PaleoShellDesignerInterface::activateTool( StandardTool tool ) { m_shell->activateTool( tool ); }
QgsLayoutDesignerInterface::ExportResults *PaleoShellDesignerInterface::lastExportResults() const { return m_shell->lastExportResults(); }

void PaleoShellDesignerInterface::close() { m_shell->close(); }
void PaleoShellDesignerInterface::showRulers( bool visible ) { m_shell->showRulers( visible ); }

// ---------------------------------------------------------------------------
// PaleoLayoutDesignerShell
// ---------------------------------------------------------------------------

PaleoLayoutDesignerShell::PaleoLayoutDesignerShell( QgsLayout *layout, QWidget *parent )
  : QDialog( parent )
  , m_layout( layout )
  , m_iface( new PaleoShellDesignerInterface( this ) )
{
  setObjectName( QStringLiteral( "PaleoLayoutDesignerShell" ) );
  const QString layoutName = masterLayout() ? masterLayout()->name() : QString();
  setWindowTitle( !layoutName.isEmpty()
                    ? tr( "%1 — Layout Designer" ).arg( layoutName )
                    : tr( "Layout Designer" ) );
  resize( 1200, 800 );

  auto *grid = new QGridLayout( this );
  grid->setContentsMargins( 0, 0, 0, 0 );
  grid->setSpacing( 0 );

  // Menu bar — QLayout::setMenuBar works for dialogs too, so the interface's
  // menu accessors return real, populated QMenus.
  m_menuBar = new QMenuBar( this );
  grid->setMenuBar( m_menuBar );

  m_messageBar = new QgsMessageBar( this );
  grid->addWidget( m_messageBar, 0, 0, 1, 3 );

  // Toolbar row — standard QToolBars (not floatable; the shell is a dialog).
  m_toolBarRow = new QWidget( this );
  auto *tbRow = new QHBoxLayout( m_toolBarRow );
  tbRow->setContentsMargins( 0, 0, 0, 0 );
  tbRow->setSpacing( 0 );
  grid->addWidget( m_toolBarRow, 1, 0, 1, 3 );

  m_view = new QgsLayoutView( this );

  m_horizontalRuler = new QgsLayoutRuler( this, Qt::Horizontal );
  m_horizontalRuler->setLayoutView( m_view );
  m_verticalRuler = new QgsLayoutRuler( this, Qt::Vertical );
  m_verticalRuler->setLayoutView( m_view );
  m_horizontalRuler->setCursorPosition( QPointF( 0, 0 ) );
  m_verticalRuler->setCursorPosition( QPointF( 0, 0 ) );

  auto *corner = new QWidget( this );
  corner->setFixedSize( m_verticalRuler->rulerSize(), m_horizontalRuler->rulerSize() );

  grid->addWidget( corner, 2, 0 );
  grid->addWidget( m_horizontalRuler, 2, 1 );
  grid->addWidget( m_verticalRuler, 3, 0 );
  grid->addWidget( m_view, 3, 1 );

  // Right-hand dock area — populated via addDockWidget().
  m_dockArea = new QWidget( this );
  auto *dockLayout = new QVBoxLayout( m_dockArea );
  dockLayout->setContentsMargins( 0, 0, 0, 0 );
  dockLayout->setSpacing( 0 );
  m_dockArea->hide();
  grid->addWidget( m_dockArea, 2, 2, 2, 1 );

  grid->setColumnStretch( 1, 1 );
  grid->setRowStretch( 3, 1 );

  if ( m_layout )
    m_view->setCurrentLayout( m_layout );

  // --- chrome: toolbars + menus ------------------------------------------

  // Layout toolbar: undo/redo wired to the layout's own undo stack, close.
  QToolBar *ltb = layoutToolbar();
  if ( m_layout && m_layout->undoStack() && m_layout->undoStack()->stack() )
  {
    QUndoStack *stack = m_layout->undoStack()->stack();
    ltb->addAction( stack->createUndoAction( this, tr( "&Undo" ) ) );
    ltb->addAction( stack->createRedoAction( this, tr( "&Redo" ) ) );
    ltb->addSeparator();
  }
  QAction *closeAction = ltb->addAction( tr( "Close" ) );
  connect( closeAction, &QAction::triggered, this, &QDialog::close );

  // Navigation toolbar: zoom.
  QToolBar *ntb = navigationToolbar();
  ntb->addAction( tr( "Zoom In" ), m_view, &QgsLayoutView::zoomIn );
  ntb->addAction( tr( "Zoom Out" ), m_view, &QgsLayoutView::zoomOut );
  ntb->addAction( tr( "Zoom Full" ), m_view, &QgsLayoutView::zoomFull );
  ntb->addAction( tr( "Zoom to Width" ), m_view, &QgsLayoutView::zoomWidth );
  ntb->addAction( tr( "Zoom 100%" ), m_view, &QgsLayoutView::zoomActual );

  // Menus — real QMenus in the menu bar so plugins can customise them.
  layoutMenu()->addAction( closeAction );

  QAction *rulerAction = new QAction( tr( "Show Rulers" ), this );
  rulerAction->setCheckable( true );
  rulerAction->setChecked( true );
  connect( rulerAction, &QAction::toggled, this, &PaleoLayoutDesignerShell::showRulers );
  viewMenu()->addAction( rulerAction );
  viewMenu()->addSeparator();
  for ( QAction *a : ntb->actions() )
    viewMenu()->addAction( a );

  editMenu();   // ensure created (empty until commands land)
  itemsMenu();
  atlasMenu();
  reportMenu();
  settingsMenu();
  actionsToolbar();
  atlasToolbar();

  // Default interaction: selection tool.
  m_view->setTool( new QgsLayoutViewToolSelect( m_view ) );

  // Keep rulers in sync with view zoom.
  connect( m_view, &QgsLayoutView::zoomLevelChanged, this, [this]() {
    m_horizontalRuler->setSceneTransform( m_view->transform() );
    m_verticalRuler->setSceneTransform( m_view->transform() );
  } );

  QMetaObject::invokeMethod( m_view, &QgsLayoutView::zoomFull, Qt::QueuedConnection );
}

PaleoLayoutDesignerShell::~PaleoLayoutDesignerShell() = default;

QgsMasterLayoutInterface *PaleoLayoutDesignerShell::masterLayout() const
{
  return dynamic_cast<QgsMasterLayoutInterface *>( m_layout );
}

void PaleoLayoutDesignerShell::selectItems( const QList<QgsLayoutItem *> &items )
{
  if ( !m_layout )
    return;
  m_layout->deselectAll();
  for ( QgsLayoutItem *item : items )
  {
    if ( item )
      item->setSelected( true );
  }
}

void PaleoLayoutDesignerShell::setAtlasPreviewEnabled( bool enabled )
{
  m_atlasPreviewEnabled = enabled;
  if ( QgsPrintLayout *printLayout = dynamic_cast<QgsPrintLayout *>( m_layout ) )
  {
    if ( QgsLayoutAtlas *atlas = printLayout->atlas() )
      atlas->setEnabled( enabled );
  }
}

void PaleoLayoutDesignerShell::setAtlasFeature( const QgsFeature &feature )
{
  if ( QgsPrintLayout *printLayout = dynamic_cast<QgsPrintLayout *>( m_layout ) )
  {
    if ( QgsLayoutAtlas *atlas = printLayout->atlas() )
      atlas->seekTo( feature );
  }
}

void PaleoLayoutDesignerShell::showItemOptions( QgsLayoutItem *item, bool bringPanelToFront )
{
  // No item-properties dock yet (P2 chrome). Selecting the item is the most
  // useful behaviour the shell can offer — harmless if bringPanelToFront.
  Q_UNUSED( bringPanelToFront );
  if ( item )
    selectItems( { item } );
}

QMenu *PaleoLayoutDesignerShell::menuFor( QPointer<QMenu> &member, const QString &title )
{
  if ( !member )
    member = m_menuBar->addMenu( title );
  return member;
}

QToolBar *PaleoLayoutDesignerShell::toolBarFor( QPointer<QToolBar> &member, const QString &objectName )
{
  if ( !member )
  {
    member = new QToolBar( this );
    member->setObjectName( objectName );
    m_toolBarRow->layout()->addWidget( member );
  }
  return member;
}

QMenu *PaleoLayoutDesignerShell::layoutMenu() { return menuFor( m_layoutMenu, tr( "&Layout" ) ); }
QMenu *PaleoLayoutDesignerShell::editMenu() { return menuFor( m_editMenu, tr( "&Edit" ) ); }
QMenu *PaleoLayoutDesignerShell::viewMenu() { return menuFor( m_viewMenu, tr( "&View" ) ); }
QMenu *PaleoLayoutDesignerShell::itemsMenu() { return menuFor( m_itemsMenu, tr( "&Items" ) ); }
QMenu *PaleoLayoutDesignerShell::atlasMenu() { return menuFor( m_atlasMenu, tr( "&Atlas" ) ); }
QMenu *PaleoLayoutDesignerShell::reportMenu() { return menuFor( m_reportMenu, tr( "&Report" ) ); }
QMenu *PaleoLayoutDesignerShell::settingsMenu() { return menuFor( m_settingsMenu, tr( "&Settings" ) ); }

QToolBar *PaleoLayoutDesignerShell::layoutToolbar() { return toolBarFor( m_layoutToolbar, QStringLiteral( "mLayoutToolbar" ) ); }
QToolBar *PaleoLayoutDesignerShell::navigationToolbar() { return toolBarFor( m_navigationToolbar, QStringLiteral( "mNavigationToolbar" ) ); }
QToolBar *PaleoLayoutDesignerShell::actionsToolbar() { return toolBarFor( m_actionsToolbar, QStringLiteral( "mActionsToolbar" ) ); }
QToolBar *PaleoLayoutDesignerShell::atlasToolbar() { return toolBarFor( m_atlasToolbar, QStringLiteral( "mAtlasToolbar" ) ); }

void PaleoLayoutDesignerShell::addDockWidget( Qt::DockWidgetArea area, QDockWidget *dock )
{
  Q_UNUSED( area );
  if ( !dock )
    return;
  if ( dock->objectName().isEmpty() )
    dock->setObjectName( QStringLiteral( "paleoDock_%1" ).arg( dock->windowTitle() ) );
  dock->setParent( m_dockArea );
  dock->setFeatures( QDockWidget::NoDockWidgetFeatures ); // fixed inside the dialog
  m_dockArea->layout()->addWidget( dock );
  m_extraDocks.append( dock );
  m_dockArea->show();
  dock->show();
}

void PaleoLayoutDesignerShell::removeDockWidget( QDockWidget *dock )
{
  if ( !dock )
    return;
  m_dockArea->layout()->removeWidget( dock );
  m_extraDocks.removeAll( dock );
  dock->setParent( nullptr );
  if ( m_extraDocks.isEmpty() )
    m_dockArea->hide();
}

void PaleoLayoutDesignerShell::activateTool( QgsLayoutDesignerInterface::StandardTool tool )
{
  switch ( tool )
  {
    case QgsLayoutDesignerInterface::ToolMoveItemContent:
      if ( !m_moveItemContentTool )
        m_moveItemContentTool = new QgsLayoutViewToolMoveItemContent( m_view );
      m_view->setTool( m_moveItemContentTool );
      break;
    case QgsLayoutDesignerInterface::ToolMoveItemNodes:
      if ( !m_editNodesTool )
        m_editNodesTool = new QgsLayoutViewToolEditNodes( m_view );
      m_view->setTool( m_editNodesTool );
      break;
  }
}

void PaleoLayoutDesignerShell::showRulers( bool visible )
{
  m_horizontalRuler->setVisible( visible );
  m_verticalRuler->setVisible( visible );
}
