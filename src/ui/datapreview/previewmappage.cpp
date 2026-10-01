// 层：视图
#include "previewmappage.h"
#include "../paleodockmanager.h"
#include "../paleoviewport.h"
#include <QMainWindow>
#include <QDockWidget>

#include "../../qgis/previewrendercache.h"

#include "previewhistogramwidget.h"
#include "previewidentifypanel.h"
#include "previewmapstates.h"
#include "previewprofilepanel.h"
#include "previewtocpanel.h"

#include "../decorations/paleodecorations.h"
#include "../paleoicons.h"
#include "../paleotheme.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsrubberband.h>

namespace
{
  QString scaleText( double scale )
  {
    if ( !( scale > 0.0 ) )
      return QStringLiteral( "—" );
    return QStringLiteral( "1:%1" ).arg( QLocale().toString( qRound64( scale ) ) ); // 无层时尺度巨大——qRound 溢出 int 即 assert
  }
}

// ------------------------------------------------------------- 鹰眼 D3.9 --
// 右下角导航小图：全图 + 主视口框；点击/拖动把主画布中心带过去；可折叠。
class PreviewMapPage::PreviewOverviewMap : public QWidget
{
  public:
    PreviewOverviewMap( PreviewMapPage *host )
      : QWidget( host->m_canvas )
      , m_host( host )
    {
      setObjectName( QStringLiteral( "previewOverviewMap" ) );
      setFixedSize( 200, 150 );
      setAttribute( Qt::WA_TransparentForMouseEvents, false );

      m_canvas = new QgsMapCanvas( this );
      m_canvas->setObjectName( QStringLiteral( "previewOverviewCanvas" ) );
      m_canvas->enableAntiAliasing( true );
      m_canvas->setCanvasColor( Qt::white );
      m_canvas->setFixedSize( 200, 150 );
      // 不给工具：事件过滤接管点击/拖动（避免小画布抢滚轮/右键）。
      m_canvas->installEventFilter( this );

      auto *lay = new QVBoxLayout( this );
      lay->setContentsMargins( 0, 0, 0, 0 );
      lay->addWidget( m_canvas );

      m_viewportBand = new QgsRubberBand( m_canvas, Qgis::GeometryType::Polygon );
      m_viewportBand->setColor( QColor( 27, 115, 208, 30 ) );
      m_viewportBand->setStrokeColor( QColor( QStringLiteral( "#1B73D0" ) ) );
      m_viewportBand->setWidth( 2 );
      m_viewportBand->show();

      PaleoTheme::applyThemedStyleSheet( this, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral( "PreviewOverviewMap { border: 1px solid %1;"
                               " background: %2; }" )
            .arg( t.border.name(), t.surface.name() );
      } );
      raise();
      show();
    }

    void syncLayers( const QList<QgsMapLayer *> &layers )
    {
      m_canvas->setLayers( layers );
      syncExtent();
    }

    void syncExtent()
    {
      const QgsRectangle full = m_host->m_canvas->fullExtent();
      if ( full.isEmpty() )
        return;
      QgsRectangle grown = full;
      grown.scale( 1.15 );
      m_canvas->setDestinationCrs( m_host->m_canvas->crs() );
      m_canvas->setExtent( grown );
      m_canvas->refresh();
      updateViewportRect();
    }

    void updateViewportRect()
    {
      const QgsRectangle main = m_host->m_canvas->currentExtent();
      if ( main.isEmpty() )
        return;
      QgsPolylineXY ring;
      ring.append( QgsPointXY( main.xMinimum(), main.yMinimum() ) );
      ring.append( QgsPointXY( main.xMaximum(), main.yMinimum() ) );
      ring.append( QgsPointXY( main.xMaximum(), main.yMaximum() ) );
      ring.append( QgsPointXY( main.xMinimum(), main.yMaximum() ) );
      ring.append( ring.first() );
      m_viewportBand->setToGeometry( QgsGeometry::fromPolygonXY( QgsPolygonXY{ ring } ),
                                     QgsCoordinateReferenceSystem() );
    }

  protected:
    bool eventFilter( QObject *watched, QEvent *event ) override
    {
      if ( watched == m_canvas )
      {
        if ( event->type() == QEvent::MouseButtonPress ||
             event->type() == QEvent::MouseMove )
        {
          auto *me = static_cast<QMouseEvent *>( event );
          if ( me->buttons() & Qt::LeftButton || event->type() == QEvent::MouseButtonPress )
          {
            centerMainAt( me->pos() );
            return true; // 吃掉事件，小画布不响应
          }
        }
        else if ( event->type() == QEvent::Wheel || event->type() == QEvent::ContextMenu )
        {
          return true; // 鹰眼不吃滚轮/右键
        }
      }
      return QWidget::eventFilter( watched, event );
    }

    void resizeEvent( QResizeEvent * ) override
    {
      // 固定尺寸，不重排。
    }

  private:
    void centerMainAt( const QPoint &pixel )
    {
      const QgsPointXY center = m_canvas->mapSettings().mapToPixel().toMapCoordinates(
          pixel.x(), pixel.y() );
      const QgsRectangle cur = m_host->m_canvas->currentExtent();
      if ( cur.isEmpty() )
        return;
      QgsRectangle moved( center.x() - cur.width() / 2, center.y() - cur.height() / 2,
                          center.x() + cur.width() / 2, center.y() + cur.height() / 2 );
      m_host->m_canvas->zoomToRect( moved );
    }

    PreviewMapPage *m_host = nullptr;
    QgsMapCanvas *m_canvas = nullptr;
    QgsRubberBand *m_viewportBand = nullptr;
};

// ------------------------------------------------------------------- page --

PreviewMapPage::PreviewMapPage( QWidget *parent )
  : QWidget( parent )
{
  m_assetKey = QStringLiteral( "preview" );

  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( 0 );

  // ---- 画布 + 错误态堆叠 ----
  m_canvas = new PreviewMapCanvas( this );
  m_tools = new PreviewMapToolManager( m_canvas->canvas(), m_canvas->canvas() );
  // 装饰管理器挂内画布（既有测试以 canvas->findChild 找它）。
  m_decor = new PaleoDecorationManager( m_canvas->canvas(), m_canvas->canvas() );
  m_decor->setObjectName( QStringLiteral( "previewDecorManager" ) );

  m_mapStack = new PaleoViewportStack( this );
  m_mapStack->setObjectName( QStringLiteral( "previewMapStack" ) );
  m_mapStack->addWidget( m_canvas );

  // ---- 右键菜单（D1.4）----
  m_canvas->canvas()->setContextMenuPolicy( Qt::CustomContextMenu );
  connect( m_canvas->canvas(), &QgsMapCanvas::customContextMenuRequested, this,
           [this]( const QPoint &pos ) {
             QMenu menu( this );
             auto *zoomLayer = menu.addMenu( QObject::tr( "缩放至图层" ) );
             const QList<QgsMapLayer *> layers = m_canvas->layers();
             for ( QgsMapLayer *l : layers )
             {
               if ( !l )
                 continue;
               QAction *a = zoomLayer->addAction( l->name() );
               connect( a, &QAction::triggered, this, [this, l] { m_canvas->zoomToLayer( l ); } );
             }
             QAction *copyCoord =
                 menu.addAction( QObject::tr( "复制坐标（%1）" )
                                     .arg( m_hasMousePos
                                               ? QStringLiteral( "%1, %2" )
                                                     .arg( QString::number( m_lastMousePos.x(), 'f', 2 ),
                                                           QString::number( m_lastMousePos.y(), 'f', 2 ) )
                                               : QObject::tr( "无光标位置" ) ) );
             connect( copyCoord, &QAction::triggered, this, &PreviewMapPage::copyCoordinate );
             QAction *copyShot = menu.addAction( QObject::tr( "复制画布截图" ) );
             connect( copyShot, &QAction::triggered, this, &PreviewMapPage::copyScreenshot );
             menu.addSeparator();
             QAction *bm = menu.addAction( QObject::tr( "把当前视图存为书签…" ) );
             connect( bm, &QAction::triggered, this, [this] {
               bool ok = false;
               const QString name = QInputDialog::getText(
                   this, QObject::tr( "新建书签" ), QObject::tr( "书签名" ),
                   QLineEdit::Normal, QObject::tr( "视图 %1" ).arg( bookmarkCount() + 1 ), &ok );
               if ( ok && !name.isEmpty() )
                 addBookmarkNamed( name );
             } );
             menu.exec( m_canvas->canvas()->mapToGlobal( pos ) );
           } );

  // ---- 工具条 / 侧栏 / 状态条 ----
  buildToolBar();

  m_sideTabs = new QTabWidget( this );
  m_sideTabs->setObjectName( QStringLiteral( "previewSideTabs" ) );
  m_sideTabs->setTabPosition( QTabWidget::North );

  m_toc = new PreviewTocPanel( m_sideTabs );
  m_identify = new PreviewIdentifyPanel( m_sideTabs );
  m_profile = new PreviewProfilePanel( m_sideTabs );
  m_sideTabs->addTab( m_toc, QObject::tr( "图层" ) );
  m_sideTabs->addTab( m_identify, QObject::tr( "识别" ) );
  m_sideTabs->addTab( m_profile, QObject::tr( "剖面" ) );

  m_dockWorkspace = new QMainWindow(this);
  m_dockWorkspace->setWindowFlags(Qt::Widget);
  m_dockWorkspace->setObjectName(QStringLiteral("previewDockWorkspace"));
  m_dockWorkspace->setWindowTitle(tr("当前预览面板"));
  m_dockWorkspace->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  m_dockWorkspace->setCentralWidget(m_mapStack);
  m_dockManager = new PaleoDockManager(m_dockWorkspace, QStringLiteral("ui/layout/preview"));
  auto *sideDock = new QDockWidget(tr("图层 / 识别 / 剖面"), m_dockWorkspace);
  sideDock->setObjectName(QStringLiteral("previewSideDock"));
  sideDock->setWidget(m_sideTabs);
  m_dockManager->addDock(Qt::RightDockWidgetArea, sideDock);

  m_analysisTabs = new QTabWidget(this);
  m_analysisTabs->setObjectName(QStringLiteral("previewAnalysisTabs"));
  m_analysisDock = new QDockWidget(tr("统计与分析"), m_dockWorkspace);
  m_analysisDock->setObjectName(QStringLiteral("previewAnalysisDock"));
  m_analysisDock->setWidget(m_analysisTabs);
  m_dockManager->addDock(Qt::BottomDockWidgetArea, m_analysisDock);
  m_analysisDock->hide();
  m_dockWorkspace->resizeDocks({sideDock}, {240}, Qt::Horizontal);
  m_dockManager->captureDefaultLayout();

  buildStatusBar();
  lay->addWidget(new PaleoToolRow(m_toolBarRow, this));
  lay->addWidget(m_dockWorkspace, 1);
  lay->addWidget(new PaleoToolRow(m_statusBar, this));

  // ---- identify 核心（D7 + D6.4 空间索引缓存）----
  m_identifyCore = new PreviewIdentifyCore( this );
  m_identify->flashHandler = [this]( QgsMapLayer *layer, const QgsFeatureIds &ids ) {
    if ( auto *vl = qobject_cast<QgsVectorLayer *>( layer ) )
      m_canvas->canvas()->flashFeatureIds( vl, ids ); // D7.2 定位闪烁
  };

  // ---- 画布信号 → 状态条 / 鹰眼 ----
  connect( m_canvas, &PreviewMapCanvas::renderStarted, this, [this] {
    m_renderLabel->setText( QObject::tr( "渲染中…" ) );
  } );
  connect( m_canvas, &PreviewMapCanvas::renderCompleted, this,
           [this]( qint64 ms, int layers, qint64 elements ) {
             m_renderLabel->setText( QObject::tr( "%1 层 · %2 图元 · %3 ms" )
                                         .arg( layers )
                                         .arg( elements )
                                         .arg( ms ) );
             if ( m_overview )
               m_overview->updateViewportRect();
             // D6.2：identity 在场即存渲染结果（内存 LRU + 磁盘 PNG）。
             if ( !m_cacheAssetId.isEmpty() )
             {
               const QSize sz = m_canvas->canvas()->size();
               const QString key = PreviewRenderCache::makeKey(
                   m_cacheAssetId, m_cacheVersionId, m_canvas->currentExtent(), sz.width(),
                   sz.height() );
               PreviewRenderCache::instance().store( key, m_canvas->canvas()->grab().toImage() );
             }
           } );
  connect( m_canvas, &PreviewMapCanvas::scaleChanged, this, [this]( double s ) {
    m_scaleLabel->setText( scaleText( s ) );
  } );
  connect( m_canvas, &PreviewMapCanvas::mapPositionTracked, this, [this]( const QgsPointXY &p ) {
    m_lastMousePos = p;
    m_hasMousePos = true;
    m_coordLabel->setText( QStringLiteral( "X %1  Y %2" )
                               .arg( QString::number( p.x(), 'f', 2 ),
                                     QString::number( p.y(), 'f', 2 ) ) );
  } );
  connect( m_canvas, &PreviewMapCanvas::extentChanged, this, [this]( const QgsRectangle & ) {
    updateNavActions();
  } );

  // ---- 工具结果路由 ----
  connect( m_tools, &PreviewMapToolManager::identifyPointRequested, this,
           [this]( const QgsPointXY &p ) { runIdentifyAt( p ); } );
  connect( m_tools, &PreviewMapToolManager::identifyRectRequested, this,
           [this]( const QgsRectangle &r ) { runIdentifyRect( r ); } );
  connect( m_tools, &PreviewMapToolManager::measurementChanged, this,
           [this]( const QVector<QgsPointXY> &, double length, double area, bool finished ) {
             QString text;
             if ( m_tools->activeToolId() == PreviewMapToolManager::kMeasureArea )
               text = QObject::tr( "周长 %1 · 面积 %2%3" )
                          .arg( PreviewMapFormat::length( length ),
                                PreviewMapFormat::area( area ),
                                finished ? QObject::tr( " · 完成" ) : QString() );
             else
               text = QObject::tr( "长度 %1%2" )
                          .arg( PreviewMapFormat::length( length ),
                                finished ? QObject::tr( " · 完成" ) : QString() );
             m_measureLabel->setText( text );
           } );
  connect( m_tools, &PreviewMapToolManager::measurementCleared, this,
           [this] { m_measureLabel->setText( QString() ); } );
  connect( m_tools, &PreviewMapToolManager::profileLineDrawn, this,
           [this]( const QgsPointXY &p1, const QgsPointXY &p2, int total ) {
             m_sideTabs->setCurrentIndex( 2 ); // 切到剖面页签
             emit profileLineDrawn( p1, p2, total );
           } );

  // ---- TOC 联动（D4.x → 画布）----
  connect( m_toc, &PreviewTocPanel::layerVisibilityChanged, this,
           [this]( QgsMapLayer *l, bool on ) {
             m_canvas->setLayerVisible( l, on );
             m_canvas->canvas()->refresh();
           } );
  connect( m_toc, &PreviewTocPanel::layerOrderChanged, this,
           [this]( const QList<QgsMapLayer *> &order ) {
             m_canvas->setLayers( order );
             m_canvas->canvas()->refresh();
           } );
  connect( m_toc, &PreviewTocPanel::layerRemoveRequested, this, &PreviewMapPage::removeMapLayer );
  connect( m_toc, &PreviewTocPanel::rasterStyleChanged, this,
           [this]( QgsRasterLayer * ) { m_canvas->canvas()->refresh(); } );
  connect( m_toc, &PreviewTocPanel::layerOpacityChanged, this,
           [this]( QgsMapLayer *l, double o ) {
             m_canvas->setLayerOpacity( l, o );
             m_canvas->canvas()->refresh();
           } );
  connect( m_toc, &PreviewTocPanel::layerBlendChanged, this, [this]( QgsMapLayer *l, int mode ) {
    m_canvas->setLayerBlendMode( l, QPainter::CompositionMode( mode ) );
    m_canvas->canvas()->refresh();
  } );
  connect( m_toc, &PreviewTocPanel::attributeTableRequested, this, [this]( QgsVectorLayer *vl ) {
    auto *dlg = new PreviewAttributeTableDialog( vl, this );
    dlg->show();
  } );

  // 画布侧层变化（移除等）→ TOC 回写
  connect( m_canvas, &PreviewMapCanvas::layersChanged, this, [this] {
    emit mapLayersChanged();
    if ( m_overview )
      m_overview->syncLayers( m_canvas->layers() );
  } );

  // 默认 pan 在岗 + 鹰眼常开（D3.9，可折叠）
  m_tools->activate( PreviewMapToolManager::kPan );
  setOverviewVisible( true );
  rebuildBookmarkMenu();
  updateNavActions();
}

PreviewMapPage::~PreviewMapPage()
{
  m_toc->saveMemory();
  // 工具条不再用 QWidgetAction（addWidget）：其析构序在 ~QToolBar 子链里
  // 会对悬空按钮 releaseWidget（实测 SIGSEGV）；弹出按钮改挂扩展条。
}

void PreviewMapPage::buildToolBar()
{
  m_toolBar = new QToolBar( this );
  m_toolBar->setObjectName( QStringLiteral( "previewMapToolBar" ) );
  m_toolBar->setToolButtonStyle( Qt::ToolButtonIconOnly );
  m_toolBar->setIconSize( QSize( 18, 18 ) );
  m_toolBar->setMovable( false );
  // 工具条 chrome 走 token（活体注册随主题）。hover 用 border 档（surfaceAlt
  // 底上可见）；checked = 活动地图工具惯例（primary 描边 + surfaceAltRaised
  // 底，同 PaleoTheme::ribbonStyleSheet 的 checked 范式）。#E2E8F0/#E1EFFE
  // 原字面量无对应 token——见 docs/progress/ui-polish.md §5 hover 档 token 提案。
  PaleoTheme::applyThemedStyleSheet( m_toolBar, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
        "QToolBar { background: %1; border-bottom: 1px solid %2; padding: 2px; }"
        "QToolButton { background: transparent; border: none; padding: 3px; border-radius: 4px; }"
        "QToolButton:hover { background: %2; }"
        "QToolButton:checked { background: %3; border: 1px solid %4; }" )
        .arg( t.surfaceAlt.name(), t.border.name(), t.surfaceAltRaised.name(),
              t.primary.name() );
  } );

  // ---- 扩展条：弹出按钮直挂（不用 addWidget/QWidgetAction——销毁序雷区）----
  m_toolBarExt = new QWidget( this );
  m_toolBarExt->setObjectName( QStringLiteral( "previewMapToolBarExt" ) );
  PaleoTheme::applyThemedStyleSheet( m_toolBarExt, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral( "background: %1; border-bottom: 1px solid %2;" )
        .arg( t.surfaceAlt.name(), t.border.name() );
  } );
  auto *extLay = new QHBoxLayout( m_toolBarExt );
  extLay->setContentsMargins( 4, 2, 4, 2 );
  extLay->setSpacing( 2 );

  // 行容器：工具条（QAction 区）+ 扩展条（直挂按钮区）同一视觉行。
  m_toolBarRow = new QWidget( this );
  m_toolBarRow->setObjectName( QStringLiteral( "previewMapToolBarRow" ) );
  auto *rowLay = new QHBoxLayout( m_toolBarRow );
  rowLay->setContentsMargins( 0, 0, 0, 0 );
  rowLay->setSpacing( 0 );
  rowLay->addWidget( m_toolBar );
  rowLay->addWidget( m_toolBarExt, 1 );

  m_toolGroup = new QActionGroup( this );
  const auto addTool = [this]( const QString &id, const QString &text, const QString &icon,
                               const QKeySequence &shortcut = QKeySequence() ) {
    auto *a = new QAction( PaleoIcons::qgisTheme( icon ), text, this );
    a->setObjectName( QStringLiteral( "previewAction_" ) + id );
    a->setCheckable( true );
    a->setToolTip( text );
    if ( !shortcut.isEmpty() )
      a->setShortcut( shortcut );
    m_toolGroup->addAction( a );
    m_toolBar->addAction( a );
    m_toolActions.insert( id, a );
    connect( a, &QAction::triggered, this, [this, id, a] {
      m_tools->activate( id );
      a->setChecked( true ); // QActionGroup 互斥下点击已选工具会取消勾选——钉回
    } );
    return a;
  };

  m_panAction = addTool( PreviewMapToolManager::kPan, QObject::tr( "漫游" ),
                         QStringLiteral( "mActionPan.svg" ) );
  m_zoomInAction = addTool( PreviewMapToolManager::kZoomIn, QObject::tr( "框选放大" ),
                            QStringLiteral( "mActionZoomToSelected.svg" ) );
  m_zoomOutAction = addTool( PreviewMapToolManager::kZoomOut, QObject::tr( "框选缩小" ),
                             QStringLiteral( "mActionZoomOut.svg" ) );
  m_identifyAction = addTool( PreviewMapToolManager::kIdentify, QObject::tr( "识别要素" ),
                              QStringLiteral( "mActionIdentify.svg" ) );
  m_measureLineAction = addTool( PreviewMapToolManager::kMeasureLine,
                                 QObject::tr( "距离测量" ),
                                 QStringLiteral( "mActionMeasure.svg" ) );
  m_measureAreaAction = addTool( PreviewMapToolManager::kMeasureArea, QObject::tr( "面积测量" ),
                                 QStringLiteral( "mActionMeasureArea.svg" ) );
  m_profileAction = addTool( PreviewMapToolManager::kProfile, QObject::tr( "层位剖面线" ),
                             QStringLiteral( "mActionProfile.svg" ) );
  m_toolGroup->setExclusive( true );
  m_panAction->setChecked( true );

  // 工具激活状态回写 action（键盘 Esc / 程序切换的同步）
  connect( m_tools, &PreviewMapToolManager::toolActivated, this,
           [this]( const QString &id ) {
             if ( QAction *a = m_toolActions.value( id ) )
               a->setChecked( true );
           } );

  m_toolBar->addSeparator();

  auto *fullAction = new QAction( PaleoIcons::qgisTheme( QStringLiteral( "mActionZoomFullExtent.svg" ) ),
                                  QObject::tr( "全图复位" ), this );
  fullAction->setToolTip( QObject::tr( "缩放到全部图层范围（快捷键 0）" ) );
  connect( fullAction, &QAction::triggered, this, [this] { m_canvas->zoomToFullExtent(); } );
  m_toolBar->addAction( fullAction );

  m_backAction = new QAction( PaleoIcons::qgisTheme( QStringLiteral( "mActionArrowBack.svg" ) ),
                              QObject::tr( "上一视图" ), this );
  connect( m_backAction, &QAction::triggered, this, [this] { m_canvas->zoomBack(); } );
  m_toolBar->addAction( m_backAction );
  m_fwdAction = new QAction( PaleoIcons::qgisTheme( QStringLiteral( "mActionArrowForward.svg" ) ),
                             QObject::tr( "下一视图" ), this );
  connect( m_fwdAction, &QAction::triggered, this, [this] { m_canvas->zoomForward(); } );
  m_toolBar->addAction( m_fwdAction );

  m_toolBar->addSeparator();

  auto *copyCoordAction = new QAction(
      PaleoIcons::qgisTheme( QStringLiteral( "mActionEditCopy.svg" ) ),
      QObject::tr( "复制坐标" ), this );
  copyCoordAction->setObjectName( QStringLiteral( "previewAction_copyCoord" ) );
  copyCoordAction->setToolTip( QObject::tr( "复制光标处工程坐标（右键菜单亦可）" ) );
  connect( copyCoordAction, &QAction::triggered, this, &PreviewMapPage::copyCoordinate );
  m_toolBar->addAction( copyCoordAction );

  auto *copyShotAction = new QAction(
      PaleoIcons::qgisTheme( QStringLiteral( "mActionSaveMapAsImage.svg" ) ),
      QObject::tr( "复制画布截图" ), this );
  copyShotAction->setObjectName( QStringLiteral( "previewAction_copyShot" ) );
  connect( copyShotAction, &QAction::triggered, this, &PreviewMapPage::copyScreenshot );
  m_toolBar->addAction( copyShotAction );

  // ---- 书签菜单（D3.7）----
  auto *bookmarkBtn = new QToolButton( m_toolBarExt );
  bookmarkBtn->setObjectName( QStringLiteral( "previewBookmarkButton" ) );
  bookmarkBtn->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mActionAddBookmark.svg" ) ) );
  bookmarkBtn->setToolTip( QObject::tr( "书签：保存/跳转/删除视图" ) );
  bookmarkBtn->setPopupMode( QToolButton::InstantPopup );
  m_bookmarkMenu = new QMenu( bookmarkBtn );
  bookmarkBtn->setMenu( m_bookmarkMenu );
  extLay->addWidget( bookmarkBtn );

  // ---- 装饰件菜单（D1.5）----
  auto *decorBtn = new QToolButton( m_toolBarExt );
  decorBtn->setObjectName( QStringLiteral( "previewDecorButton" ) );
  decorBtn->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mActionDecorationGrid.svg" ) ) );
  decorBtn->setToolTip( QObject::tr( "画布装饰：比例尺/指北针/网格" ) );
  decorBtn->setPopupMode( QToolButton::InstantPopup );
  auto *decorMenu = new QMenu( decorBtn );
  const auto decorToggle = [this]( QMenu *menu, const QString &name, const QString &text,
                                   bool on ) {
    QAction *a = menu->addAction( text );
    a->setCheckable( true );
    a->setChecked( on );
    a->setObjectName( QStringLiteral( "previewDecorAction_" ) + name );
    connect( a, &QAction::toggled, this, [this, name]( bool checked ) {
      setDecorationEnabled( name, checked );
    } );
    return a;
  };
  decorToggle( decorMenu, QStringLiteral( "scaleBar" ), QObject::tr( "比例尺" ), true );
  decorToggle( decorMenu, QStringLiteral( "northArrow" ), QObject::tr( "指北针" ), true );
  decorToggle( decorMenu, QStringLiteral( "grid" ), QObject::tr( "网格" ), false );
  decorBtn->setMenu( decorMenu );
  extLay->addWidget( decorBtn );
  extLay->addStretch( 1 );
  m_decor->setScaleBarEnabled( true );
  m_decor->setNorthArrowEnabled( true );

  // ---- 鹰眼开关（D3.9）----
  auto *overviewAction = new QAction(
      PaleoIcons::qgisTheme( QStringLiteral( "mActionAddMap.svg" ) ),
      QObject::tr( "鹰眼" ), this );
  overviewAction->setObjectName( QStringLiteral( "previewOverviewAction" ) );
  overviewAction->setCheckable( true );
  overviewAction->setChecked( true );
  connect( overviewAction, &QAction::toggled, this, &PreviewMapPage::setOverviewVisible );
  m_toolBar->addAction( overviewAction );
}

void PreviewMapPage::buildStatusBar()
{
  m_statusBar = new QWidget( this );
  m_statusBar->setObjectName( QStringLiteral( "previewMapStatusBar" ) );
  PaleoTheme::applyThemedStyleSheet( m_statusBar, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral( "background: %1; border-top: 1px solid %2;" )
        .arg( t.surfaceAlt.name(), t.border.name() );
  } );
  auto *lay = new QHBoxLayout( m_statusBar );
  lay->setContentsMargins( 8, 2, 8, 2 );
  lay->setSpacing( 12 );

  m_renderLabel = new QLabel( m_statusBar );
  m_renderLabel->setObjectName( QStringLiteral( "previewRenderLabel" ) );
  PaleoTheme::applyThemedStyleSheet( m_renderLabel, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + QStringLiteral( " font-size: 8pt;" );
  } );
  lay->addWidget( m_renderLabel );

  m_measureLabel = new QLabel( m_statusBar );
  m_measureLabel->setObjectName( QStringLiteral( "previewMeasureLabel" ) );
  PaleoTheme::applyThemedStyleSheet( m_measureLabel, [] {
    return QStringLiteral( "color: %1; font-size: 8pt;" )
        .arg( PaleoTheme::tokens().text.name() );
  } );
  lay->addWidget( m_measureLabel );

  lay->addStretch( 1 );

  m_scaleLabel = new QLabel( m_statusBar );
  m_scaleLabel->setObjectName( QStringLiteral( "previewScaleLabel" ) );
  m_scaleLabel->setFont( PaleoTheme::monoFont() );
  PaleoTheme::applyThemedStyleSheet( m_scaleLabel, [] {
    return QStringLiteral( "color: %1;" ).arg( PaleoTheme::tokens().text.name() );
  } );
  lay->addWidget( m_scaleLabel );

  m_coordLabel = new QLabel( m_statusBar );
  m_coordLabel->setObjectName( QStringLiteral( "previewCoordLabel" ) );
  m_coordLabel->setFont( PaleoTheme::monoFont() );
  PaleoTheme::applyThemedStyleSheet( m_coordLabel, [] {
    return QStringLiteral( "color: %1;" ).arg( PaleoTheme::tokens().text.name() );
  } );
  m_coordLabel->setMinimumWidth( 180 );
  lay->addWidget( m_coordLabel );
}

void PreviewMapPage::addMapLayer( QgsMapLayer *layer, const QString &name,
                                  const QString &sourcePath )
{
  if ( !layer )
    return;
  m_canvas->addLayer( layer );
  m_toc->addLayer( layer, name, sourcePath );
  if ( m_overview )
    m_overview->syncLayers( m_canvas->layers() );
  // mapLayersChanged 由 canvas layersChanged 统一转发（不双发）。
}

void PreviewMapPage::removeMapLayer( QgsMapLayer *layer )
{
  if ( !layer )
    return;
  m_canvas->removeLayer( layer );
  m_toc->removeLayer( layer );
  m_identifyCore->dropIndex( layer );
  m_canvas->canvas()->refresh();
  if ( m_overview )
    m_overview->syncLayers( m_canvas->layers() );
}

void PreviewMapPage::clearMapLayers()
{
  for ( QgsMapLayer *l : m_canvas->layers() )
    m_identifyCore->dropIndex( l );
  m_canvas->setLayers( {} );
  m_toc->clear();
  m_canvas->canvas()->refresh();
  if ( m_overview )
    m_overview->syncLayers( {} );
}

int PreviewMapPage::mapLayerCount() const
{
  return m_canvas->layerCount();
}

void PreviewMapPage::setToolVisible( const QString &toolId, bool visible )
{
  if ( QAction *a = m_toolActions.value( toolId ) )
  {
    a->setVisible( visible );
    if ( !visible && a->isChecked() )
    {
      m_tools->activate( PreviewMapToolManager::kPan );
      a->setChecked( false );
      if ( QAction *pan = m_toolActions.value( PreviewMapToolManager::kPan ) )
        pan->setChecked( true );
    }
  }
}

bool PreviewMapPage::isToolVisible( const QString &toolId ) const
{
  const QAction *a = m_toolActions.value( toolId );
  return a && a->isVisible();
}

void PreviewMapPage::setError( const QString &title, const QString &detail )
{
  if ( m_errorPage )
  {
    m_mapStack->removeWidget( m_errorPage );
    delete m_errorPage;
  }
  m_errorPage = PreviewMapStates::buildErrorPage( title, detail, this );
  m_mapStack->addWidget( m_errorPage );
  m_mapStack->setCurrentWidget( m_errorPage );
}

void PreviewMapPage::clearError()
{
  if ( m_errorPage )
  {
    m_mapStack->setCurrentIndex( 0 );
    m_mapStack->removeWidget( m_errorPage );
    delete m_errorPage;
    m_errorPage = nullptr;
  }
}

bool PreviewMapPage::errorActive() const
{
  return m_errorPage != nullptr && m_mapStack->currentWidget() == m_errorPage;
}

void PreviewMapPage::setProfileEnabled( bool on )
{
  setToolVisible( PreviewMapToolManager::kProfile, on );
}

void PreviewMapPage::addAnalysisTab( const QString &title, QWidget *w )
{
  m_analysisTabs->addTab( w, title );
  if (m_analysisTabs->count() == 1) {
    m_analysisDock->show();
    m_dockWorkspace->resizeDocks({m_analysisDock}, {200}, Qt::Vertical);
    m_dockManager->captureDefaultLayout();
  }
}

void PreviewMapPage::setAssetKey( const QString &key )
{
  m_assetKey = key;
  m_toc->setAssetKey( key );
}

int PreviewMapPage::bookmarkCount() const
{
  return PreviewStateMemory::bookmarks( m_assetKey ).size();
}

void PreviewMapPage::addBookmarkNamed( const QString &name )
{
  PreviewStateMemory::Bookmark bm;
  bm.name = name;
  bm.extent = m_canvas->currentExtent();
  PreviewStateMemory::addBookmark( m_assetKey, bm );
  rebuildBookmarkMenu();
}

bool PreviewMapPage::jumpToBookmark( const QString &name )
{
  const auto bms = PreviewStateMemory::bookmarks( m_assetKey );
  for ( const auto &bm : bms )
    if ( bm.name == name )
    {
      m_canvas->setViewExtent( bm.extent ); // 精确复位（D3.7）
      return true;
    }
  return false;
}

bool PreviewMapPage::removeBookmark( const QString &name )
{
  const bool ok = PreviewStateMemory::removeBookmark( m_assetKey, name );
  rebuildBookmarkMenu();
  return ok;
}

void PreviewMapPage::rebuildBookmarkMenu()
{
  m_bookmarkMenu->clear();
  QAction *add = m_bookmarkMenu->addAction( QObject::tr( "保存当前视图…" ) );
  connect( add, &QAction::triggered, this, [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QObject::tr( "新建书签" ), QObject::tr( "书签名" ), QLineEdit::Normal,
        QObject::tr( "视图 %1" ).arg( bookmarkCount() + 1 ), &ok );
    if ( ok && !name.isEmpty() )
      addBookmarkNamed( name );
  } );
  const auto bms = PreviewStateMemory::bookmarks( m_assetKey );
  if ( !bms.isEmpty() )
  {
    m_bookmarkMenu->addSeparator();
    for ( const auto &bm : bms )
    {
      QAction *jump = m_bookmarkMenu->addAction(
          QStringLiteral( "%1  (%2 × %3)" )
              .arg( bm.name, QString::number( bm.extent.width(), 'f', 0 ),
                    QString::number( bm.extent.height(), 'f', 0 ) ) );
      connect( jump, &QAction::triggered, this, [this, name = bm.name] { jumpToBookmark( name ); } );
    }
    m_bookmarkMenu->addSeparator();
    for ( const auto &bm : bms )
    {
      QAction *del = m_bookmarkMenu->addAction( QObject::tr( "删除「%1」" ).arg( bm.name ) );
      connect( del, &QAction::triggered, this, [this, name = bm.name] { removeBookmark( name ); } );
    }
  }
}

void PreviewMapPage::setOverviewVisible( bool on )
{
  if ( on && !m_overview )
  {
    m_overview = new PreviewOverviewMap( this );
    m_overview->syncLayers( m_canvas->layers() );
    m_overview->updateViewportRect();
  }
  m_overviewOn = on;
  if ( m_overview )
    m_overview->setVisible( on );
}

bool PreviewMapPage::overviewVisible() const
{
  return m_overviewOn; // 显式开关位：页未显示时也算「开」
}

void PreviewMapPage::setDecorationEnabled( const QString &name, bool on )
{
  if ( name == QLatin1String( "scaleBar" ) )
    m_decor->setScaleBarEnabled( on );
  else if ( name == QLatin1String( "northArrow" ) )
    m_decor->setNorthArrowEnabled( on );
  else if ( name == QLatin1String( "grid" ) )
    m_decor->setGridEnabled( on );
  m_canvas->canvas()->refresh();
}

bool PreviewMapPage::decorationEnabled( const QString &name ) const
{
  if ( name == QLatin1String( "scaleBar" ) )
    return m_decor->isScaleBarEnabled();
  if ( name == QLatin1String( "northArrow" ) )
    return m_decor->isNorthArrowEnabled();
  if ( name == QLatin1String( "grid" ) )
    return m_decor->isGridEnabled();
  return false;
}

QString PreviewMapPage::coordinateReadout() const
{
  return m_coordLabel->text();
}

QString PreviewMapPage::scaleReadout() const
{
  return m_scaleLabel->text();
}

QString PreviewMapPage::renderReadout() const
{
  return m_renderLabel->text();
}

QString PreviewMapPage::measureReadout() const
{
  return m_measureLabel->text();
}

void PreviewMapPage::showLowResSnapshot()
{
  const QImage img = m_canvas->renderSnapshot( 320 );
  if ( !img.isNull() )
    m_canvas->showPreviewOverlay( img );
  m_canvas->canvas()->refresh();
}

void PreviewMapPage::setRenderCacheIdentity( const QString &assetId, const QString &versionId )
{
  m_cacheAssetId = assetId;
  m_cacheVersionId = versionId;
}

void PreviewMapPage::primeRenderCache()
{
  if ( m_cacheAssetId.isEmpty() )
    return;
  const QSize sz = m_canvas->canvas()->size();
  if ( sz.isEmpty() )
    return;
  const QString key = PreviewRenderCache::makeKey(
      m_cacheAssetId, m_cacheVersionId, m_canvas->currentExtent(), sz.width(), sz.height() );
  const QImage cached = PreviewRenderCache::instance().lookup( key );
  if ( !cached.isNull() )
    m_canvas->showPreviewOverlay( cached ); // 命中：缓存图立即上屏（D6.2）
}

void PreviewMapPage::addToolBarAction( QAction *action )
{
  m_toolBar->addAction( action );
}

void PreviewMapPage::addToolBarSeparator()
{
  m_toolBar->addSeparator();
}

void PreviewMapPage::addToolBarWidget( QWidget *widget )
{
  if ( m_toolBarExt && widget )
    m_toolBarExt->layout()->addWidget( widget ); // 直挂扩展条（无 QWidgetAction）
}

void PreviewMapPage::updateNavActions()
{
  if ( m_backAction )
    m_backAction->setEnabled( m_canvas->canZoomBack() );
  if ( m_fwdAction )
    m_fwdAction->setEnabled( m_canvas->canZoomForward() );
}

void PreviewMapPage::runIdentifyAt( const QgsPointXY &point )
{
  const double tolerance = qMax( m_canvas->mapUnitsPerPixel() * 3.0, 1.0 );
  const auto results = m_identifyCore->identifyPoint( m_canvas->layers(), point, tolerance );
  m_identify->setResults( results );
  m_sideTabs->setCurrentIndex( 1 ); // 切到识别页签
}

void PreviewMapPage::runIdentifyRect( const QgsRectangle &rect )
{
  const auto results = m_identifyCore->identifyRect( m_canvas->layers(), rect );
  m_identify->setResults( results );
  m_sideTabs->setCurrentIndex( 1 );
}

void PreviewMapPage::copyCoordinate()
{
  if ( !m_hasMousePos )
    return;
  QApplication::clipboard()->setText(
      QStringLiteral( "%1, %2" )
          .arg( QString::number( m_lastMousePos.x(), 'f', 2 ),
                QString::number( m_lastMousePos.y(), 'f', 2 ) ) );
}

void PreviewMapPage::copyScreenshot()
{
  QApplication::clipboard()->setPixmap( m_canvas->canvas()->grab() );
}
