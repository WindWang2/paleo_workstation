// 层：QGIS 封装
#include "previewmapcanvas.h"

#include "../catalog/datacatalog.h" // localGridCrsWkt（datum-free 工程网格）

#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <qgslayertreemapcanvasbridge.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmaprenderercustompainterjob.h>
#include <qgsmapsettings.h>
#include <qgsproject.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include <qgsmessagelog.h>

#include <cmath>

namespace
{
  // 相邻范围判等（浮点容差按视口比例）。
  bool sameExtent( const QgsRectangle &a, const QgsRectangle &b )
  {
    const double tol = std::max( std::abs( a.width() ), 1.0 ) * 1e-9;
    return std::abs( a.xMinimum() - b.xMinimum() ) < tol &&
           std::abs( a.yMinimum() - b.yMinimum() ) < tol &&
           std::abs( a.width() - b.width() ) < tol &&
           std::abs( a.height() - b.height() ) < tol;
  }
} // namespace

PreviewMapCanvas::PreviewMapCanvas( QWidget *parent )
  : QWidget( parent )
{
  setFocusPolicy( Qt::StrongFocus );
  m_trackTimer.start();

  m_canvas = new QgsMapCanvas( this );
  m_canvas->setObjectName( QStringLiteral( "previewMapInnerCanvas" ) );
  m_canvas->enableAntiAliasing( true );
  m_canvas->setCanvasColor( Qt::white ); // 地图按纸面白底（DESIGN.md 数据符号不随暗色）
  m_canvas->setFocusPolicy( Qt::ClickFocus );
  m_canvas->installEventFilter( this ); // 鼠标移动 → mapPositionTracked（节流）

  // 缺省 CRS：工程 datum-free 局部网格（米）。QGIS 空画布不设 CRS 时会按
  // 首层猜，预览必须钉死——绝不允许隐式 EPSG:4326。
  const QgsCoordinateReferenceSystem local =
      QgsCoordinateReferenceSystem::fromWkt( DataCatalog::localGridCrsWkt() );
  if ( local.isValid() )
    m_canvas->setDestinationCrs( local );

  // 渲染状态接线（D1.6）：renderStarting 开表，mapCanvasRefreshed 结表。
  connect( m_canvas, &QgsMapCanvas::renderStarting, this, [this] {
    m_rendering = true;
    m_renderTimer.restart();
    emit renderStarted();
  } );
  connect( m_canvas, &QgsMapCanvas::mapCanvasRefreshed, this, [this] {
    m_rendering = false;
    m_lastRenderMs = m_renderTimer.isValid() ? m_renderTimer.elapsed() : 0;
    const QList<QgsMapLayer *> current = m_canvas->layers();
    m_lastRenderElements = estimateElements( current );
    hidePreviewOverlay(); // 低清/缓存快照让位真渲（D6.1）
    emit renderCompleted( m_lastRenderMs, current.size(), m_lastRenderElements );
  } );
  // 视图历史（D3.3）：交互平移/缩放经 extentsChanged 入栈；程序式缩放由
  // setExtentInternal 直接压栈并置抑制位，避免双记。
  connect( m_canvas, &QgsMapCanvas::extentsChanged, this, [this] {
    const QgsRectangle ext = m_canvas->extent();
    if ( !m_suppressHistory )
      pushHistory( ext );
    emit extentChanged( ext );
  } );
  connect( m_canvas, &QgsMapCanvas::scaleChanged, this, &PreviewMapCanvas::scaleChanged );

  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( 0 );
  lay->addWidget( m_canvas );
}

PreviewMapCanvas::~PreviewMapCanvas()
{
  // D1.9 销毁安全：在飞渲染任务全部取消 + 卸下工具，无悬挂 job。
  if ( m_canvas )
  {
    if ( QgsMapTool *tool = m_canvas->mapTool() )
      m_canvas->unsetMapTool( tool );
    m_canvas->stopRendering();
  }
}

void PreviewMapCanvas::setLayers( const QList<QgsMapLayer *> &layers )
{
  m_layers = layers;
  for ( QgsMapLayer *l : layers )
  {
    if ( !m_visible.contains( l ) )
      m_visible.insert( l, true );
    if ( !m_opacity.contains( l ) )
      m_opacity.insert( l, 1.0 );
    if ( !m_blend.contains( l ) )
      m_blend.insert( l, QPainter::CompositionMode_SourceOver );
  }
  syncCanvasLayers();
}

void PreviewMapCanvas::addLayer( QgsMapLayer *layer )
{
  if ( !layer || m_layers.contains( layer ) )
    return;
  m_layers.prepend( layer );
  m_visible.insert( layer, true );
  m_opacity.insert( layer, 1.0 );
  m_blend.insert( layer, QPainter::CompositionMode_SourceOver );
  syncCanvasLayers();
}

void PreviewMapCanvas::insertLayer( int index, QgsMapLayer *layer )
{
  if ( !layer || m_layers.contains( layer ) )
    return;
  const int clamped = qBound( 0, index, m_layers.size() );
  m_layers.insert( clamped, layer );
  m_visible.insert( layer, true );
  m_opacity.insert( layer, 1.0 );
  m_blend.insert( layer, QPainter::CompositionMode_SourceOver );
  syncCanvasLayers();
}

void PreviewMapCanvas::removeLayer( QgsMapLayer *layer )
{
  if ( !layer )
    return;
  m_layers.removeOne( layer );
  m_visible.remove( layer );
  m_opacity.remove( layer );
  m_blend.remove( layer );
  syncCanvasLayers();
}

void PreviewMapCanvas::moveLayer( int from, int to )
{
  if ( from < 0 || from >= m_layers.size() || to < 0 || to >= m_layers.size() || from == to )
    return;
  QgsMapLayer *l = m_layers.takeAt( from );
  m_layers.insert( to, l );
  syncCanvasLayers();
}

int PreviewMapCanvas::layerCount() const
{
  return m_layers.size();
}

QList<QgsMapLayer *> PreviewMapCanvas::layers() const
{
  return m_layers;
}

int PreviewMapCanvas::indexOfLayer( const QgsMapLayer *layer ) const
{
  for ( int i = 0; i < m_layers.size(); ++i )
    if ( m_layers.at( i ) == layer )
      return i;
  return -1;
}

QgsMapLayer *PreviewMapCanvas::layerAt( int index ) const
{
  return ( index >= 0 && index < m_layers.size() ) ? m_layers.at( index ) : nullptr;
}

void PreviewMapCanvas::setLayerVisible( QgsMapLayer *layer, bool visible )
{
  if ( !layer || !m_visible.contains( layer ) )
    return;
  m_visible[layer] = visible;
  syncCanvasLayers();
}

bool PreviewMapCanvas::isLayerVisible( const QgsMapLayer *layer ) const
{
  return m_visible.value( layer, true );
}

void PreviewMapCanvas::setLayerOpacity( QgsMapLayer *layer, double opacity )
{
  if ( !layer || !m_layers.contains( layer ) )
    return;
  const double clamped = qBound( 0.0, opacity, 1.0 );
  m_opacity[layer] = clamped;
  layer->setOpacity( clamped ); // QGIS 4 原生层透明度（D4.2）
}

double PreviewMapCanvas::layerOpacity( const QgsMapLayer *layer ) const
{
  return m_opacity.value( layer, 1.0 );
}

void PreviewMapCanvas::setLayerBlendMode( QgsMapLayer *layer, QPainter::CompositionMode mode )
{
  if ( !layer || !m_layers.contains( layer ) )
    return;
  m_blend[layer] = mode;
  layer->setBlendMode( mode ); // D4.8
}

QPainter::CompositionMode PreviewMapCanvas::layerBlendMode( const QgsMapLayer *layer ) const
{
  return m_blend.value( layer, QPainter::CompositionMode_SourceOver );
}

void PreviewMapCanvas::syncCanvasLayers()
{
  if ( m_projectBound )
    return; // 桥接模式：项目图层树管 setLayers（测区全景）
  QList<QgsMapLayer *> shown;
  shown.reserve( m_layers.size() );
  for ( QgsMapLayer *l : m_layers )
    if ( m_visible.value( l, true ) )
      shown.append( l );
  if ( m_canvas )
  {
    m_canvas->stopRendering(); // D6.3：换层时中止在飞渲染
    m_canvas->setLayers( shown );
  }
  emit layersChanged();
}

void PreviewMapCanvas::setOverrideCrs( const QgsCoordinateReferenceSystem &crs )
{
  m_overrideCrs = crs;
  if ( m_canvas && crs.isValid() )
    m_canvas->setDestinationCrs( crs );
  emit crsChanged();
}

QgsCoordinateReferenceSystem PreviewMapCanvas::crs() const
{
  if ( m_overrideCrs.isValid() )
    return m_overrideCrs;
  return m_canvas ? m_canvas->mapSettings().destinationCrs()
                  : QgsCoordinateReferenceSystem();
}

QgsRectangle PreviewMapCanvas::fullExtent() const
{
  QgsRectangle ext;
  for ( QgsMapLayer *l : m_layers )
  {
    if ( !m_visible.value( l, true ) || !l )
      continue;
    const QgsRectangle le = l->extent();
    if ( le.isEmpty() )
      continue;
    if ( ext.isNull() || ext.isEmpty() )
      ext = le;
    else
      ext.combineExtentWith( le );
  }
  return ext;
}

QgsRectangle PreviewMapCanvas::currentExtent() const
{
  return m_canvas ? m_canvas->extent() : QgsRectangle();
}

void PreviewMapCanvas::setExtentInternal( const QgsRectangle &rect )
{
  if ( !m_canvas || rect.isEmpty() )
    return;
  const QgsRectangle cur = m_canvas->extent();
  if ( !sameExtent( cur, rect ) )
    pushHistory( cur );
  m_suppressHistory = true; // extentsChanged 里的 pushHistory 会被抑制
  m_canvas->setExtent( rect );
  m_canvas->refresh();
  m_suppressHistory = false;
}

void PreviewMapCanvas::zoomToFullExtent()
{
  QgsRectangle ext = fullExtent();
  if ( ext.isNull() || ext.isEmpty() )
  {
    if ( m_canvas )
      m_canvas->zoomToFullExtent();
    return;
  }
  ext.scale( 1.08 ); // 留 8% 边距
  setExtentInternal( ext );
}

void PreviewMapCanvas::zoomToLayer( const QgsMapLayer *layer )
{
  if ( !layer )
    return;
  const QgsRectangle ext = layer->extent();
  if ( ext.isEmpty() )
    return;
  QgsRectangle grown = ext;
  grown.scale( 1.08 );
  setExtentInternal( grown );
}

void PreviewMapCanvas::zoomToRect( const QgsRectangle &rect )
{
  if ( rect.isEmpty() )
    return;
  QgsRectangle grown = rect;
  grown.scale( 1.05 );
  setExtentInternal( grown );
}

void PreviewMapCanvas::pushHistory( const QgsRectangle &extent )
{
  if ( extent.isNull() || extent.isEmpty() )
    return;
  if ( !m_backStack.isEmpty() && sameExtent( m_backStack.top(), extent ) )
    return; // 去重：相邻同范围不入栈
  m_backStack.push( extent );
  if ( m_backStack.size() > 100 )
    m_backStack.remove( 0 ); // 上限 100 步，防长会话膨胀
  m_forwardStack.clear();    // 新分支截断前进栈
}

bool PreviewMapCanvas::canZoomBack() const
{
  return !m_backStack.isEmpty();
}

void PreviewMapCanvas::zoomBack()
{
  if ( m_backStack.isEmpty() || !m_canvas )
    return;
  const QgsRectangle cur = m_canvas->extent();
  const QgsRectangle prev = m_backStack.pop();
  m_forwardStack.push( cur );
  m_suppressHistory = true;
  m_canvas->setExtent( prev );
  m_canvas->refresh();
  m_suppressHistory = false;
  emit extentChanged( prev );
}

bool PreviewMapCanvas::canZoomForward() const
{
  return !m_forwardStack.isEmpty();
}

void PreviewMapCanvas::zoomForward()
{
  if ( m_forwardStack.isEmpty() || !m_canvas )
    return;
  const QgsRectangle cur = m_canvas->extent();
  const QgsRectangle next = m_forwardStack.pop();
  m_backStack.push( cur );
  m_suppressHistory = true;
  m_canvas->setExtent( next );
  m_canvas->refresh();
  m_suppressHistory = false;
  emit extentChanged( next );
}

void PreviewMapCanvas::clearHistory()
{
  m_backStack.clear();
  m_forwardStack.clear();
}

void PreviewMapCanvas::cancelRendering()
{
  if ( m_canvas )
    m_canvas->stopRendering();
}

QImage PreviewMapCanvas::renderSnapshot( int maxWidthPx ) const
{
  if ( !m_canvas )
    return QImage();
  QgsMapSettings settings = m_canvas->mapSettings();
  const QSize full = settings.outputSize();
  if ( full.isEmpty() )
    return QImage();
  const int targetW = maxWidthPx > 0 ? qMin( maxWidthPx, full.width() ) : 320;
  const double ratio = static_cast<double>( targetW ) / full.width();
  const int targetH = qMax( 1, qRound( full.height() * ratio ) );
  settings.setOutputSize( QSize( targetW, targetH ) );
  settings.setOutputDpi( settings.outputDpi() * ratio );

  QImage img( targetW, targetH, QImage::Format_ARGB32_Premultiplied );
  img.fill( Qt::white );
  {
    QPainter p( &img );
    QgsMapRendererCustomPainterJob job( settings, &p );
    job.start();
    job.waitForFinished();
  }
  return img;
}

void PreviewMapCanvas::showPreviewOverlay( const QImage &image )
{
  if ( !m_canvas || image.isNull() )
    return;
  if ( !m_overlay )
  {
    m_overlay = new QLabel( m_canvas );
    m_overlay->setObjectName( QStringLiteral( "previewSnapshotOverlay" ) );
    m_overlay->setAlignment( Qt::AlignCenter );
    m_overlay->setScaledContents( true );
  }
  m_overlay->setPixmap( QPixmap::fromImage( image ) );
  m_overlay->setGeometry( m_canvas->rect() );
  m_overlay->raise();
  m_overlay->show();
}

void PreviewMapCanvas::hidePreviewOverlay()
{
  if ( m_overlay )
    m_overlay->hide();
}

bool PreviewMapCanvas::overlayVisible() const
{
  return m_overlay && m_overlay->isVisible();
}

QgsPointXY PreviewMapCanvas::toMapCoordinates( const QPoint &pixel ) const
{
  return m_canvas ? m_canvas->mapSettings().mapToPixel().toMapCoordinates( pixel.x(), pixel.y() )
                  : QgsPointXY();
}

double PreviewMapCanvas::mapUnitsPerPixel() const
{
  return m_canvas ? m_canvas->mapSettings().mapUnitsPerPixel() : 0.0;
}

double PreviewMapCanvas::scale() const
{
  return m_canvas ? m_canvas->scale() : 0.0;
}

qint64 PreviewMapCanvas::estimateElements( const QList<QgsMapLayer *> &layers )
{
  qint64 total = 0;
  for ( QgsMapLayer *l : layers )
  {
    if ( !l )
      continue;
    if ( auto *vl = qobject_cast<QgsVectorLayer *>( l ) )
      total += vl->featureCount();
    else if ( auto *rl = qobject_cast<QgsRasterLayer *>( l ) )
    {
      if ( rl->dataProvider() )
      {
        const int w = rl->width();
        const int h = rl->height();
        if ( w > 0 && h > 0 )
          total += static_cast<qint64>( w ) * h;
      }
    }
  }
  return total;
}

void PreviewMapCanvas::attachProjectLayers( QgsProject *project )
{
  if ( !m_canvas || !project )
    return;
  m_projectBound = true;
  m_canvas->setProject( project );
  m_canvas->setDestinationCrs( project->crs() );
  // 桥接管 setLayers（延迟到事件循环，与主画布同语义）。
  new QgsLayerTreeMapCanvasBridge( project->layerTreeRoot(), m_canvas, m_canvas );
  emit layersChanged();
}

void PreviewMapCanvas::keyPressEvent( QKeyEvent *event )
{
  if ( !m_canvas )
  {
    QWidget::keyPressEvent( event );
    return;
  }
  const QgsRectangle ext = m_canvas->extent();
  switch ( event->key() )
  {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
      m_canvas->zoomWithCenter( width() / 2, height() / 2, true );
      event->accept();
      return;
    case Qt::Key_Minus:
      m_canvas->zoomWithCenter( width() / 2, height() / 2, false );
      event->accept();
      return;
    case Qt::Key_0:
      zoomToFullExtent();
      event->accept();
      return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    {
      const double dx = ext.width() * 0.2 *
                        ( event->key() == Qt::Key_Left ? -1.0 : event->key() == Qt::Key_Right ? 1.0 : 0.0 );
      const double dy = ext.height() * 0.2 *
                        ( event->key() == Qt::Key_Down ? -1.0 : event->key() == Qt::Key_Up ? 1.0 : 0.0 );
      QgsRectangle moved = ext;
      moved.setXMinimum( ext.xMinimum() + dx );
      moved.setXMaximum( ext.xMaximum() + dx );
      moved.setYMinimum( ext.yMinimum() + dy );
      moved.setYMaximum( ext.yMaximum() + dy );
      setExtentInternal( moved );
      event->accept();
      return;
    }
    default:
      QWidget::keyPressEvent( event );
      return;
  }
}

// 事件过滤：鼠标移动 → mapPositionTracked（节流 ≤30Hz，D6.5）+ 覆盖层跟随
// 画布尺寸。Esc 的「取消工具」语义在工具管理器里（QgsMapTool::keyPressEvent）。
bool PreviewMapCanvas::eventFilter( QObject *watched, QEvent *event )
{
  if ( watched == m_canvas )
  {
    if ( event->type() == QEvent::MouseMove )
    {
      const qint64 now = m_trackTimer.isValid() ? m_trackTimer.elapsed() : 0;
      if ( now - m_lastTrackMs >= 33 ) // ≤30Hz
      {
        m_lastTrackMs = now;
        auto *me = static_cast<QMouseEvent *>( event );
        emit mapPositionTracked( toMapCoordinates( me->pos() ) );
      }
    }
    else if ( event->type() == QEvent::Resize && m_overlay && m_overlay->isVisible() )
    {
      m_overlay->setGeometry( m_canvas->rect() );
    }
  }
  return QWidget::eventFilter( watched, event );
}
