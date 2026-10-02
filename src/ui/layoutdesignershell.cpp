// 层：视图
#include "layoutdesignershell.h"

#include "layout/layoutexportactions.h"
#include "layout/layoutitempalette.h"
#include "layout/layoutitempanel.h"
#include "layout/layouttemplates.h"
#include "layout/layoutundostack.h"

#include <QAction>
#include <QDialog>
#include "paleodockmanager.h"
#include <QMainWindow>
#include <QCursor>
#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QSet>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QSignalBlocker>
#include <QUndoStack>
#include <QVBoxLayout>

#include <qgsfeature.h>
#include <qgsgui.h>
#include <qgslayout.h>
#include <qgslayoutatlas.h>
#include <qgslayoutexporter.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemguiregistry.h>
#include <qgslayoutitempage.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutpagepropertieswidget.h>
#include <qgslayoutruler.h>
#include <qgslayoutundostack.h>
#include <qgslayoutview.h>
#include <qgslayoutviewtooladditem.h>
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
                    ? tr( "%1 — 图件设计器" ).arg( layoutName )
                    : tr( "图件设计器" ) );
  resize( 1200, 800 );

  auto *outer = new QVBoxLayout( this );
  outer->setContentsMargins( 0, 0, 0, 0 );
  outer->setSpacing( 0 );

  // Menu bar — QLayout::setMenuBar works for dialogs too, so the interface's
  // menu accessors return real, populated QMenus (exactly four top levels).
  m_menuBar = new QMenuBar( this );
  outer->setMenuBar( m_menuBar );

  m_messageBar = new QgsMessageBar( this );
  outer->addWidget( m_messageBar );

  // Toolbar row — standard QToolBars (not floatable; the shell is a dialog).
  m_toolBarRow = new QWidget( this );
  auto *tbRow = new QHBoxLayout( m_toolBarRow );
  tbRow->setContentsMargins( 0, 0, 0, 0 );
  tbRow->setSpacing( 0 );
  outer->addWidget( m_toolBarRow );

  // --- body: left palette | center rulers+view | right properties ----------

  m_dockArea = new QMainWindow(this);
  m_dockArea->setWindowFlags(Qt::Widget);
  m_dockArea->setObjectName(QStringLiteral("designerDockWorkspace"));
  m_dockManager = new PaleoDockManager(m_dockArea, QStringLiteral("ui/layout/designer"));

  // Left: element palette (subtask A). Constructed BEFORE the properties
  // panel on purpose: the palette registers QGIS's native item GUI metadata,
  // so the panel's fallback-metadata guard then finds it and the native
  // per-type widgets win (subtask B's integration order).
  auto *leftHost = new QWidget( this );
  leftHost->setObjectName( QStringLiteral( "paletteHost" ) );
  auto *leftLay = new QVBoxLayout( leftHost );
  leftLay->setContentsMargins( 0, 0, 0, 0 );
  leftLay->setSpacing( 0 );
  m_palette = new PaleoLayoutItemPalette( leftHost );
  leftLay->addWidget( m_palette );
  auto *paletteDock = new QDockWidget(tr("图件元素"), m_dockArea);
  paletteDock->setObjectName(QStringLiteral("designerPaletteDock"));
  paletteDock->setWidget(leftHost);
  m_dockManager->addDock(Qt::LeftDockWidgetArea, paletteDock);

  // Center: view with rulers (unchanged from the original shell).
  m_view = new QgsLayoutView( this );

  m_horizontalRuler = new QgsLayoutRuler( this, Qt::Horizontal );
  m_horizontalRuler->setLayoutView( m_view );
  m_verticalRuler = new QgsLayoutRuler( this, Qt::Vertical );
  m_verticalRuler->setLayoutView( m_view );
  m_horizontalRuler->setCursorPosition( QPointF( 0, 0 ) );
  m_verticalRuler->setCursorPosition( QPointF( 0, 0 ) );

  auto *corner = new QWidget( this );
  corner->setFixedSize( m_verticalRuler->rulerSize(), m_horizontalRuler->rulerSize() );

  auto *center = new QWidget( this );
  auto *grid = new QGridLayout( center );
  grid->setContentsMargins( 0, 0, 0, 0 );
  grid->setSpacing( 0 );
  grid->addWidget( corner, 0, 0 );
  grid->addWidget( m_horizontalRuler, 0, 1 );
  grid->addWidget( m_verticalRuler, 1, 0 );
  grid->addWidget( m_view, 1, 1 );
  grid->setColumnStretch( 1, 1 );
  grid->setRowStretch( 1, 1 );
  m_dockArea->setCentralWidget(center);

  // Right: item properties panel (subtask B) above the adoptable dock area.
  auto *rightHost = new QWidget( this );
  rightHost->setObjectName( QStringLiteral( "propertiesHost" ) );
  auto *rightLay = new QVBoxLayout( rightHost );
  rightLay->setContentsMargins( 0, 0, 0, 0 );
  rightLay->setSpacing( 0 );
  m_itemPanel = new PaleoLayoutItemPanel( rightHost );
  rightLay->addWidget( m_itemPanel );

  auto *propertiesDock = new QDockWidget(tr("元素属性"), m_dockArea);
  propertiesDock->setObjectName(QStringLiteral("designerPropertiesDock"));
  propertiesDock->setWidget(rightHost);
  m_dockManager->addDock(Qt::RightDockWidgetArea, propertiesDock);
  outer->addWidget(m_dockArea, 1);
  m_dockManager->captureDefaultLayout();

  // Bottom: status bar with page navigator + zoom controls.
  m_statusBar = new QStatusBar( this );
  m_statusBar->setSizeGripEnabled( false );
  outer->addWidget( m_statusBar );

  if ( m_layout )
    m_view->setCurrentLayout( m_layout );

  // --- subtask C/D controller objects ---------------------------------------

  m_undoStack = new PaleoLayoutUndoStack( m_layout, this );

  m_exportActions = new PaleoLayoutExportActions( this );
  m_exportActions->setLayoutProvider( [this]() -> QgsLayout * { return m_layout; } );
  m_exportActions->setCurrentPageProvider( [this]() -> int { return m_view ? m_view->currentPage() : 0; } );
  m_exportActions->setStatusTarget( m_statusBar );

  m_templates = new PaleoLayoutTemplates( this );
  m_templates->setLayoutProvider( [this]() -> QgsLayout * { return m_layout; } );

  // Element palette interactive path: itemRequested -> QgsLayoutViewToolAddItem.
  m_palette->attach( m_view );

  buildChrome();
  settingsMenu()->addAction(tr("布局与面板…"), this, [this] {
    auto *menu = m_dockManager->createMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(QCursor::pos());
  });
  connectLayoutSync();

  // Default interaction: selection tool.
  // QgsLayoutViewToolSelect::setLayout() 在场景上创建 QgsLayoutMouseHandles
  // (qgslayoutdesignerdialog.cpp:1210)——缺了它 mMouseHandles 为空，鼠标
  // 在画布上移动即 SEGV（QgsLayoutViewToolSelect::layoutMoveEvent）。
  auto *selectTool = new QgsLayoutViewToolSelect( m_view );
  if ( m_layout )
    selectTool->setLayout( m_layout );
  m_selectTool = selectTool;
  m_view->setTool( selectTool );

  // goal/ui-experience-polish：Delete 直删所选 layout 项（QgsLayoutView 原生
  // 能力 deleteSelectedItems——QGIS designer 把它挂在 app 层动作上，这里补
  // QAction 挂壳；方向键微调/Space 平移是视图内建，E3 不重复实现）。
  auto *deleteAction = new QAction( tr( "删除所选项" ), this );
  deleteAction->setObjectName( QStringLiteral( "layoutDeleteSelectedAction" ) );
  deleteAction->setShortcut( Qt::Key_Delete );
  connect( deleteAction, &QAction::triggered, this, [this] {
    if ( m_view )
      m_view->deleteSelectedItems();
  } );
  addAction( deleteAction );

  // 元素放置完成后回到选择工具（QGIS designer 惯例: createdItem -> Select）。
  if ( auto *addTool = m_palette->addItemTool() )
    connect( addTool, &QgsLayoutViewToolAddItem::createdItem, this, [this] {
      if ( m_selectTool && m_view )
        m_view->setTool( m_selectTool );
    } );

  // Keep rulers in sync with view zoom.
  connect( m_view, &QgsLayoutView::zoomLevelChanged, this, [this]() {
    m_horizontalRuler->setSceneTransform( m_view->transform() );
    m_verticalRuler->setSceneTransform( m_view->transform() );
  } );

  updatePageNavigator();
  QMetaObject::invokeMethod( m_view, &QgsLayoutView::zoomFull, Qt::QueuedConnection );
}

PaleoLayoutDesignerShell::~PaleoLayoutDesignerShell()
{
  delete m_lastExportResults;
}

void PaleoLayoutDesignerShell::setTaskService( PaleoTaskService *service )
{
  m_exportActions->setTaskService( service );
}

void PaleoLayoutDesignerShell::buildChrome()
{
  // --- page navigation actions (needed by both the Layout menu and the
  // status-bar navigator — created first so buildLayoutMenu can add them) ----

  m_prevPageAction = new QAction( tr( "上一页" ), this );
  m_prevPageAction->setObjectName( QStringLiteral( "actionPreviousPage" ) );
  connect( m_prevPageAction, &QAction::triggered, this,
           [this]() { gotoPage( m_view->currentPage() - 1 ); } );

  m_nextPageAction = new QAction( tr( "下一页" ), this );
  m_nextPageAction->setObjectName( QStringLiteral( "actionNextPage" ) );
  connect( m_nextPageAction, &QAction::triggered, this,
           [this]() { gotoPage( m_view->currentPage() + 1 ); } );

  // --- toolbars -------------------------------------------------------------

  // Layout toolbar: undo/redo through the subtask-D wrapper (single attach —
  // never a second createUndoAction wiring), then Close.
  QToolBar *ltb = layoutToolbar();
  m_undoStack->attachWidget( ltb );
  QAction *closeAction = ltb->addAction( tr( "关闭" ) );
  closeAction->setObjectName( QStringLiteral( "actionCloseLayoutDesigner" ) );
  connect( closeAction, &QAction::triggered, this, &QDialog::close );

  // Navigation toolbar: zoom (also mirrored into the View submenu and the
  // status bar — one set of shared QAction instances).
  QToolBar *ntb = navigationToolbar();
  ntb->addAction( tr( "放大" ), m_view, &QgsLayoutView::zoomIn );
  ntb->addAction( tr( "缩小" ), m_view, &QgsLayoutView::zoomOut );
  ntb->addAction( tr( "全图" ), m_view, &QgsLayoutView::zoomFull );
  ntb->addAction( tr( "适应宽度" ), m_view, &QgsLayoutView::zoomWidth );
  ntb->addAction( tr( "原始比例" ), m_view, &QgsLayoutView::zoomActual );

  // --- menus (exactly four top levels: File / Items / Layout / Settings) ----

  buildFileMenu();
  buildItemsMenu();
  buildLayoutMenu();
  settingsMenu(); // eagerly created so the top-level order is deterministic

  fileMenu()->addSeparator();
  fileMenu()->addAction( closeAction );

  actionsToolbar();
  atlasToolbar();

  // --- status bar: page navigator + zoom controls ----------------------------

  auto *navigator = new QWidget( m_statusBar );
  navigator->setObjectName( QStringLiteral( "pageNavigator" ) );
  auto *navLay = new QHBoxLayout( navigator );
  navLay->setContentsMargins( 6, 0, 6, 0 );
  navLay->setSpacing( 4 ); // DESIGN.md spacing.xs

  auto *prevButton = new QToolButton( navigator );
  prevButton->setDefaultAction( m_prevPageAction );
  prevButton->setArrowType( Qt::LeftArrow );
  auto *nextButton = new QToolButton( navigator );
  nextButton->setDefaultAction( m_nextPageAction );
  nextButton->setArrowType( Qt::RightArrow );

  m_pageLabel = new QLabel( navigator );
  m_pageLabel->setObjectName( QStringLiteral( "pageLabel" ) );

  m_pageSpin = new QSpinBox( navigator );
  m_pageSpin->setObjectName( QStringLiteral( "pageSpinBox" ) );
  m_pageSpin->setToolTip( tr( "跳转到页" ) );
  m_pageSpin->setAccessibleName( tr( "页码" ) );
  m_pageSpin->setKeyboardTracking( false );
  connect( m_pageSpin, &QSpinBox::valueChanged, this,
           [this]( int page ) { gotoPage( page - 1 ); } );

  navLay->addWidget( prevButton );
  navLay->addWidget( nextButton );
  navLay->addWidget( m_pageLabel );
  navLay->addWidget( m_pageSpin );
  m_statusBar->addWidget( navigator );

  for ( QAction *a : ntb->actions() )
  {
    auto *btn = new QToolButton( m_statusBar );
    btn->setDefaultAction( a );
    btn->setToolButtonStyle( Qt::ToolButtonTextBesideIcon ); // actions are text-only
    m_statusBar->addPermanentWidget( btn );
  }
}

void PaleoLayoutDesignerShell::buildFileMenu()
{
  QMenu *file = fileMenu();
  file->addAction( m_exportActions->exportPngAction() );
  file->addAction( m_exportActions->exportPdfAction() );
  file->addAction( m_exportActions->exportSvgAction() );
  file->addSeparator();
  file->addAction( m_templates->saveAsTemplateAction() );
  file->addAction( m_templates->loadFromTemplateAction() );

  QMenu *builtins = file->addMenu( tr( "内置模板(&T)" ) );
  builtins->setObjectName( QStringLiteral( "menuBuiltinTemplates" ) );
  for ( const QString &key : PaleoLayoutTemplates::builtinKeys() )
  {
    QAction *a = m_templates->applyBuiltinAction( key );
    if ( a )
      builtins->addAction( a );
  }
}

void PaleoLayoutDesignerShell::buildItemsMenu()
{
  // Same source as the palette: the GUI-registry metadata ids behind its
  // buttons, re-emitted through the palette's interactive add path.
  QMenu *menu = itemsMenu();
  for ( int metadataId : m_palette->itemMetadataIds() )
  {
    if ( metadataId < 0 )
      continue; // unavailable in this QGIS build (palette shows it disabled)
    QgsLayoutItemAbstractGuiMetadata *metadata = QgsGui::layoutItemGuiRegistry()->itemMetadata( metadataId );
    if ( !metadata )
      continue;
    QAction *a = menu->addAction( metadata->creationIcon(), tr( "添加 %1" ).arg( metadata->visibleName() ) );
    a->setObjectName( QStringLiteral( "menuAddItem_%1" ).arg( metadataId ) );
    connect( a, &QAction::triggered, this, [this, metadataId]() { m_palette->requestItem( metadataId ); } );
  }

  menu->addSeparator();
  QAction *pageAction = menu->addAction( tr( "页面属性…" ) );
  pageAction->setObjectName( QStringLiteral( "menuPageProperties" ) );
  connect( pageAction, &QAction::triggered, m_palette, &PaleoLayoutItemPalette::requestPageProperties );
}

void PaleoLayoutDesignerShell::buildLayoutMenu()
{
  QMenu *layout = layoutMenu();

  QAction *goAction = layout->addAction( tr( "跳转到页…" ) );
  goAction->setObjectName( QStringLiteral( "actionGoToPage" ) );
  connect( goAction, &QAction::triggered, this, [this]()
  {
    const int count = m_layout && m_layout->pageCollection() ? m_layout->pageCollection()->pageCount() : 0;
    if ( count < 1 )
      return;
    bool ok = false;
    const int page = QInputDialog::getInt( this, tr( "跳转到页" ), tr( "页码：" ),
                                           m_view->currentPage() + 1, 1, count, 1, &ok );
    if ( ok )
      gotoPage( page - 1 );
  } );
  layout->addAction( m_prevPageAction );
  layout->addAction( m_nextPageAction );

  layout->addSeparator();

  // Interface submenus: &Edit (undo/redo via subtask D), &View (rulers+zoom),
  // &Atlas and &Report hang off the Layout top-level menu.
  m_undoStack->attachMenu( editMenu() );

  QAction *rulerAction = new QAction( tr( "显示标尺" ), this );
  rulerAction->setObjectName( QStringLiteral( "actionShowRulers" ) );
  rulerAction->setCheckable( true );
  rulerAction->setChecked( true );
  connect( rulerAction, &QAction::toggled, this, &PaleoLayoutDesignerShell::showRulers );
  viewMenu()->addAction( rulerAction );
  viewMenu()->addSeparator();
  for ( QAction *a : navigationToolbar()->actions() )
    viewMenu()->addAction( a );

  layout->addMenu( atlasMenu() );
  layout->addMenu( reportMenu() );
}

void PaleoLayoutDesignerShell::connectLayoutSync()
{
  // Selection -> properties panel (subtask B). setItem itself emits nothing —
  // selection is not a modification.
  if ( m_layout )
  {
    connect( m_layout, &QgsLayout::selectedItemChanged, this,
             [this]( QgsLayoutItem *item ) { m_itemPanel->setItem( item ); } );

    // Undo/redo rebuilds item instances by UUID (subtask D's warning): the
    // panel's QPointer guards against dangling, and this hook re-reads the
    // selection so the panel shows the re-created item without a stale host.
    if ( m_layout->undoStack() )
    {
      connect( m_layout->undoStack(), &QgsLayoutUndoStack::undoRedoOccurredForItems, this,
               [this]( const QSet<QString> & ) { refreshPanelAfterUndoRedo(); } );
    }

    if ( QgsLayoutPageCollection *pages = m_layout->pageCollection() )
      connect( pages, &QgsLayoutPageCollection::changed, this, [this]() { updatePageNavigator(); } );
  }

  connect( m_view, &QgsLayoutView::pageChanged, this, [this]( int ) { updatePageNavigator(); } );

  connect( m_palette, &PaleoLayoutItemPalette::pagePropertiesRequested, this,
           &PaleoLayoutDesignerShell::openPageProperties );

  // Templates replace the whole layout (clearExisting) — page count and any
  // hosted item go away; refresh the navigator (the panel clears itself via
  // its destroyed() guard).
  connect( m_templates, &PaleoLayoutTemplates::templateFinished, this,
           [this]( const QString &, bool ) { updatePageNavigator(); } );

  // Fill the interface's lastExportResults after the first successful export.
  connect( m_exportActions, &PaleoLayoutExportActions::exportFinished, this,
           [this]( const QString &, bool ok )
           {
             if ( !ok )
               return;
             if ( !m_lastExportResults )
               m_lastExportResults = new QgsLayoutDesignerInterface::ExportResults();
             m_lastExportResults->result = QgsLayoutExporter::Success;
             m_lastExportResults->labelingResults.clear();
           } );
}

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
  // Bare QGraphicsItem::setSelected does not emit QgsLayout::selectedItemChanged
  // (only setSelectedItem/deselectAll do), so the panel sync is driven here too
  // rather than relying on the layout signal alone.
  m_itemPanel->setItem( items.isEmpty() ? nullptr : items.first() );
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
  if ( item )
    selectItems( { item } ); // keeps the shell's selection-driven contract
  m_itemPanel->setItem( item );
  if ( bringPanelToFront && item )
  {
    m_itemPanel->show();
    m_itemPanel->raise();
  }
}

QMenu *PaleoLayoutDesignerShell::menuFor( QPointer<QMenu> &member, const QString &title )
{
  if ( !member )
    member = m_menuBar->addMenu( title );
  return member;
}

QMenu *PaleoLayoutDesignerShell::submenuFor( QPointer<QMenu> &member, QMenu *parent, const QString &title )
{
  if ( !member && parent )
    member = parent->addMenu( title );
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

QMenu *PaleoLayoutDesignerShell::layoutMenu() { return menuFor( m_layoutMenu, tr( "版面(&L)" ) ); }
QMenu *PaleoLayoutDesignerShell::editMenu() { return submenuFor( m_editMenu, layoutMenu(), tr( "编辑(&E)" ) ); }
QMenu *PaleoLayoutDesignerShell::viewMenu() { return submenuFor( m_viewMenu, layoutMenu(), tr( "视图(&V)" ) ); }
QMenu *PaleoLayoutDesignerShell::itemsMenu() { return menuFor( m_itemsMenu, tr( "项(&I)" ) ); }
QMenu *PaleoLayoutDesignerShell::atlasMenu() { return submenuFor( m_atlasMenu, layoutMenu(), tr( "地图集(&A)" ) ); }
QMenu *PaleoLayoutDesignerShell::reportMenu() { return submenuFor( m_reportMenu, layoutMenu(), tr( "报告(&R)" ) ); }
QMenu *PaleoLayoutDesignerShell::settingsMenu() { return menuFor( m_settingsMenu, tr( "设置(&S)" ) ); }
QMenu *PaleoLayoutDesignerShell::fileMenu() { return menuFor( m_fileMenu, tr( "文件(&F)" ) ); }

QToolBar *PaleoLayoutDesignerShell::layoutToolbar() { return toolBarFor( m_layoutToolbar, QStringLiteral( "mLayoutToolbar" ) ); }
QToolBar *PaleoLayoutDesignerShell::navigationToolbar() { return toolBarFor( m_navigationToolbar, QStringLiteral( "mNavigationToolbar" ) ); }
QToolBar *PaleoLayoutDesignerShell::actionsToolbar() { return toolBarFor( m_actionsToolbar, QStringLiteral( "mActionsToolbar" ) ); }
QToolBar *PaleoLayoutDesignerShell::atlasToolbar() { return toolBarFor( m_atlasToolbar, QStringLiteral( "mAtlasToolbar" ) ); }

void PaleoLayoutDesignerShell::addDockWidget( Qt::DockWidgetArea area, QDockWidget *dock )
{
  if ( !dock )
    return;
  if ( dock->objectName().isEmpty() )
    dock->setObjectName( QStringLiteral( "paleoDock_%1" ).arg( dock->windowTitle() ) );
  m_dockManager->addDock(area, dock);
  m_extraDocks.append( dock );
  dock->show();
}

void PaleoLayoutDesignerShell::removeDockWidget( QDockWidget *dock )
{
  if ( !dock )
    return;
  m_dockManager->removeDock(dock);
  m_extraDocks.removeAll( dock );
  dock->setParent( nullptr );

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

void PaleoLayoutDesignerShell::updatePageNavigator()
{
  if ( !m_pageLabel || !m_pageSpin )
    return;

  const int count = m_layout && m_layout->pageCollection() ? m_layout->pageCollection()->pageCount() : 0;
  const int current = m_view ? m_view->currentPage() : 0;

  m_pageLabel->setText( count > 0 ? tr( "第 %1 / %2 页" ).arg( current + 1 ).arg( count )
                                  : tr( "暂无页面" ) );

  {
    const QSignalBlocker block( m_pageSpin );
    m_pageSpin->setRange( 1, qMax( 1, count ) );
    m_pageSpin->setValue( qBound( 1, current + 1, qMax( 1, count ) ) );
  }
  m_pageSpin->setEnabled( count > 1 );
  if ( m_prevPageAction )
    m_prevPageAction->setEnabled( count > 0 && current > 0 );
  if ( m_nextPageAction )
    m_nextPageAction->setEnabled( count > 0 && current < count - 1 );
}

void PaleoLayoutDesignerShell::gotoPage( int page )
{
  if ( !m_layout || !m_view )
    return;
  QgsLayoutPageCollection *pages = m_layout->pageCollection();
  if ( !pages || page < 0 || page >= pages->pageCount() )
    return;
  QgsLayoutItemPage *pageItem = pages->page( page );
  if ( !pageItem )
    return;

  // QGIS 4.2 has no QgsLayoutView::showPage(); the sanctioned navigation is
  // QGraphicsView::fitInView over the page's scene rect, then viewChanged()
  // so rulers/currentPage/pageChanged catch up (subtask D's verified recipe).
  m_view->fitInView( pageItem->mapRectToScene( pageItem->rect() ), Qt::KeepAspectRatio );
  m_view->viewChanged();
  updatePageNavigator();
}

void PaleoLayoutDesignerShell::openPageProperties()
{
  QgsLayoutPageCollection *pages = m_layout ? m_layout->pageCollection() : nullptr;
  if ( !pages || pages->pageCount() < 1 )
  {
    if ( m_statusBar )
      m_statusBar->showMessage( tr( "版面还没有页面。" ) );
    return;
  }

  const int current = m_view ? qBound( 0, m_view->currentPage(), pages->pageCount() - 1 ) : 0;
  QgsLayoutItem *pageItem = pages->page( current );

  // Non-modal: page setup is a side panel concept in the QGIS designer, and a
  // modal dialog would block the palette/status flows that share this shell.
  auto *dialog = new QDialog( this );
  dialog->setObjectName( QStringLiteral( "paleoPagePropertiesDialog" ) );
  dialog->setWindowTitle( tr( "页面属性" ) );
  auto *lay = new QVBoxLayout( dialog );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->addWidget( new QgsLayoutPagePropertiesWidget( dialog, pageItem ) );
  dialog->setAttribute( Qt::WA_DeleteOnClose );
  dialog->show();
}

void PaleoLayoutDesignerShell::refreshPanelAfterUndoRedo()
{
  if ( !m_layout )
    return;
  const QList<QgsLayoutItem *> selected = m_layout->selectedLayoutItems();
  m_itemPanel->setItem( selected.isEmpty() ? nullptr : selected.first() );
  updatePageNavigator();
}
