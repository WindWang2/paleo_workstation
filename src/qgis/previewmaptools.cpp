// 层：QGIS 封装
#include "previewmaptools.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>

#include <qgsdistancearea.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolpan.h>
#include <qgsmaptoolzoom.h>
#include <qgsrubberband.h>
#include <qgswkbtypes.h>

#include <cmath>

namespace
{
  // 量测距离（平面局部网格，米制）。
  double planarLength( const QVector<QgsPointXY> &pts )
  {
    if ( pts.size() < 2 )
      return 0.0;
    QgsPolylineXY line;
    line.reserve( pts.size() );
    for ( const QgsPointXY &p : pts )
      line.append( p );
    const QgsGeometry geom = QgsGeometry::fromPolylineXY( line );
    QgsDistanceArea da; // 不设椭球 → 平面（工程网格语义）
    return da.measureLength( geom );
  }

  double planarArea( const QVector<QgsPointXY> &pts )
  {
    if ( pts.size() < 3 )
      return 0.0;
    QgsPolygonXY ring;
    QgsPolylineXY boundary;
    boundary.reserve( pts.size() + 1 );
    for ( const QgsPointXY &p : pts )
      boundary.append( p );
    if ( boundary.first() != boundary.last() )
      boundary.append( boundary.first() );
    ring.append( boundary );
    const QgsGeometry geom = QgsGeometry::fromPolygonXY( ring );
    QgsDistanceArea da;
    return da.measureArea( geom );
  }

  double planarPerimeter( const QVector<QgsPointXY> &pts )
  {
    if ( pts.size() < 3 )
      return 0.0;
    QgsPolylineXY line;
    line.reserve( pts.size() + 1 );
    for ( const QgsPointXY &p : pts )
      line.append( p );
    if ( line.first() != line.last() )
      line.append( line.first() );
    const QgsGeometry geom = QgsGeometry::fromPolylineXY( line );
    QgsDistanceArea da;
    return da.measureLength( geom );
  }
} // namespace

namespace PreviewMapFormat
{
QString length( double meters )
{
  if ( !std::isfinite( meters ) )
    return QStringLiteral( "—" );
  if ( std::abs( meters ) >= 1000.0 )
    return QStringLiteral( "%1 km" ).arg( meters / 1000.0, 0, 'f', 2 );
  return QStringLiteral( "%1 m" ).arg( meters, 0, 'f', 1 );
}

QString area( double squareMeters )
{
  if ( !std::isfinite( squareMeters ) )
    return QStringLiteral( "—" );
  if ( std::abs( squareMeters ) >= 1e6 )
    return QStringLiteral( "%1 km²" ).arg( squareMeters / 1e6, 0, 'f', 3 );
  return QStringLiteral( "%1 m²" ).arg( squareMeters, 0, 'f', 1 );
}
} // namespace PreviewMapFormat

// ---------------------------------------------------------------- measure --

PreviewMeasureTool::PreviewMeasureTool( QgsMapCanvas *canvas, bool areaMode )
  : QgsMapTool( canvas )
  , m_areaMode( areaMode )
{
  m_band = new QgsRubberBand( canvas, m_areaMode ? Qgis::GeometryType::Polygon
                                                  : Qgis::GeometryType::Line );
  m_band->setColor( QColor( 27, 115, 208, 40 ) );       // #1B73D0 交互蓝 40%
  m_band->setStrokeColor( QColor( QStringLiteral( "#1B73D0" ) ) );
  m_band->setWidth( 2 );
  m_band->setLineStyle( Qt::DashLine );
  m_band->hide();
  m_emitTimer.start();
  setCursor( Qt::CrossCursor );
}

PreviewMeasureTool::~PreviewMeasureTool()
{
  // band 的生命周期归画布（QObject 父子链 + 场景）；析构里不碰——
  // scene 先销毁时 QGraphicsItem 已回收，hide() 也会踩悬空（实测 SIGSEGV）。
}

double PreviewMeasureTool::currentLength() const
{
  return m_areaMode ? planarPerimeter( m_points ) : planarLength( m_points );
}

double PreviewMeasureTool::currentArea() const
{
  return m_areaMode ? planarArea( m_points ) : 0.0;
}

void PreviewMeasureTool::canvasPressEvent( QgsMapMouseEvent *e )
{
  if ( e->button() != Qt::LeftButton )
    return;
  const QgsPointXY pt = toMapCoordinates( e->pos() );
  if ( m_finished )
  {
    // 上一段已结束：新按下开启新量测。
    m_points.clear();
    m_finished = false;
  }
  m_points.append( pt );
  m_hasPreview = false;
  rebuildRubberBand( false );
  emitChanged( false );
}

void PreviewMeasureTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  if ( m_points.isEmpty() || m_finished )
    return;
  m_previewPoint = toMapCoordinates( e->pos() );
  m_hasPreview = true;
  rebuildRubberBand( true );
  emitChanged( false ); // 内部节流 ≤30Hz（D6.5）
}

void PreviewMeasureTool::canvasReleaseEvent( QgsMapMouseEvent * )
{
  // 点由 press 落；双击结束在 canvasDoubleClickEvent。
}

void PreviewMeasureTool::canvasDoubleClickEvent( QgsMapMouseEvent * )
{
  if ( m_points.size() < ( m_areaMode ? 3 : 2 ) )
    return;
  m_finished = true;
  m_hasPreview = false;
  m_band->setLineStyle( Qt::SolidLine ); // 终局帧实线
  rebuildRubberBand( false );
  emitChanged( true );
}

void PreviewMeasureTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
  {
    clear();
    emit escapeRequested();
    e->accept();
    return;
  }
  QgsMapTool::keyPressEvent( e );
}

void PreviewMeasureTool::clear()
{
  m_points.clear();
  m_finished = false;
  m_hasPreview = false;
  m_band->setLineStyle( Qt::DashLine );
  rebuildRubberBand( false );
  emit measurementCleared();
}

void PreviewMeasureTool::rebuildRubberBand( bool withPreview )
{
  if ( !m_band )
    return;
  m_band->reset( m_areaMode ? Qgis::GeometryType::Polygon : Qgis::GeometryType::Line );
  QVector<QgsPointXY> pts = m_points;
  if ( withPreview && m_hasPreview )
    pts.append( m_previewPoint );
  if ( m_areaMode && pts.size() >= 3 && pts.first() != pts.last() )
    pts.append( pts.first() ); // 面模式闭合显示
  if ( pts.isEmpty() )
  {
    m_band->hide();
    return;
  }
  for ( const QgsPointXY &p : pts )
    m_band->addPoint( p, false );
  m_band->show();
  m_band->update();
}

void PreviewMeasureTool::emitChanged( bool finished )
{
  const qint64 now = m_emitTimer.elapsed();
  if ( !finished && now - m_lastEmitMs < 33 ) // ≤30Hz
    return;
  m_lastEmitMs = now;
  QVector<QgsPointXY> pts = m_points;
  if ( !finished && m_hasPreview )
    pts.append( m_previewPoint );
  emit measurementChanged( pts, currentLength(), currentArea(), finished );
}

// --------------------------------------------------------------- identify --

PreviewIdentifyTool::PreviewIdentifyTool( QgsMapCanvas *canvas )
  : QgsMapTool( canvas )
{
  m_rectBand = new QgsRubberBand( canvas, Qgis::GeometryType::Polygon );
  m_rectBand->setColor( QColor( 27, 115, 208, 30 ) );
  m_rectBand->setStrokeColor( QColor( QStringLiteral( "#1B73D0" ) ) );
  m_rectBand->setWidth( 1 );
  m_rectBand->setLineStyle( Qt::DashLine );
  m_rectBand->hide();
  setCursor( Qt::CrossCursor );
}

PreviewIdentifyTool::~PreviewIdentifyTool()
{
  // 同 PreviewMeasureTool：不碰 band。
}

void PreviewIdentifyTool::canvasPressEvent( QgsMapMouseEvent *e )
{
  if ( e->button() != Qt::LeftButton )
    return;
  m_pressPixel = e->pos();
  m_dragging = false;
}

void PreviewIdentifyTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  if ( m_pressPixel.isNull() || !( e->buttons() & Qt::LeftButton ) )
    return;
  if ( ( e->pos() - m_pressPixel ).manhattanLength() > 5 )
  {
    m_dragging = true;
    const QgsPointXY a = toMapCoordinates( m_pressPixel );
    const QgsPointXY b = toMapCoordinates( e->pos() );
    m_rectBand->setToCanvasRectangle( QRect( m_pressPixel, e->pos() ).normalized() );
    m_rectBand->show();
    Q_UNUSED( a );
    Q_UNUSED( b );
  }
}

void PreviewIdentifyTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( e->button() != Qt::LeftButton )
    return;
  m_rectBand->hide();
  if ( m_dragging )
  {
    const QgsPointXY a = toMapCoordinates( m_pressPixel );
    const QgsPointXY b = toMapCoordinates( e->pos() );
    QgsRectangle rect( a, b );
    rect.normalize();
    m_pressPixel = QPoint();
    m_dragging = false;
    emit identifyRectRequested( rect );
  }
  else
  {
    m_pressPixel = QPoint();
    emit identifyPointRequested( toMapCoordinates( e->pos() ) );
  }
}

void PreviewIdentifyTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
  {
    m_rectBand->hide();
    m_dragging = false;
    m_pressPixel = QPoint();
    emit escapeRequested();
    e->accept();
    return;
  }
  QgsMapTool::keyPressEvent( e );
}

void PreviewIdentifyTool::deactivate()
{
  m_rectBand->hide();
  m_dragging = false;
  m_pressPixel = QPoint();
  QgsMapTool::deactivate();
}

// ---------------------------------------------------------------- profile --

PreviewProfileTool::PreviewProfileTool( QgsMapCanvas *canvas )
  : QgsMapTool( canvas )
{
  m_activeBand = new QgsRubberBand( canvas, Qgis::GeometryType::Line );
  m_activeBand->setColor( QColor( QStringLiteral( "#E65100" ) ) ); // 剖面橙（交互读出用色）
  m_activeBand->setWidth( 2 );
  m_activeBand->setLineStyle( Qt::DashLine );
  m_activeBand->hide();

  m_doneBand = new QgsRubberBand( canvas, Qgis::GeometryType::Line );
  m_doneBand->setColor( QColor( QStringLiteral( "#E65100" ) ) );
  m_doneBand->setWidth( 2 );
  m_doneBand->hide();
  setCursor( Qt::CrossCursor );
}

PreviewProfileTool::~PreviewProfileTool()
{
  // 同 PreviewMeasureTool：不碰 band。
}

void PreviewProfileTool::canvasPressEvent( QgsMapMouseEvent *e )
{
  if ( e->button() == Qt::LeftButton )
  {
    m_pressPixel = e->pos();
    m_anchor = toMapCoordinates( e->pos() );
    m_current = m_anchor;
    m_anchorValid = true;
    m_activeBand->reset( Qgis::GeometryType::Line );
    m_activeBand->addPoint( m_anchor );
    m_activeBand->addPoint( m_current );
    m_activeBand->show();
  }
  else if ( e->button() == Qt::RightButton )
  {
    clearAllBands();
  }
}

void PreviewProfileTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  if ( !m_anchorValid )
    return;
  m_current = toMapCoordinates( e->pos() );
  m_activeBand->reset( Qgis::GeometryType::Line );
  m_activeBand->addPoint( m_anchor );
  m_activeBand->addPoint( m_current );
  m_activeBand->update();
}

void PreviewProfileTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( e->button() != Qt::LeftButton || !m_anchorValid )
    return;
  const QgsPointXY released = toMapCoordinates( e->pos() );
  m_anchorValid = false;
  m_activeBand->hide();
  // 过短的抖动线（<8px 拖距）不落线。
  if ( ( e->pos() - m_pressPixel ).manhattanLength() < 8 )
    return;

  ++m_lines;
  m_linesList.append( { m_anchor, released } );
  m_doneBand->addPoint( m_anchor );
  m_doneBand->addPoint( released );
  m_doneBand->show();
  emit profileLineDrawn( m_anchor, released, m_lines );
}

void PreviewProfileTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
  {
    clearAllBands();
    emit escapeRequested();
    e->accept();
    return;
  }
  QgsMapTool::keyPressEvent( e );
}

void PreviewProfileTool::deactivate()
{
  m_anchorValid = false;
  m_activeBand->hide();
  QgsMapTool::deactivate();
}

void PreviewProfileTool::clearAllBands()
{
  m_lines = 0;
  m_linesList.clear();
  m_activeBand->reset( Qgis::GeometryType::Line );
  m_doneBand->reset( Qgis::GeometryType::Line );
  m_activeBand->hide();
  m_doneBand->hide();
  m_anchorValid = false;
  emit profileLinesCleared();
}

// --------------------------------------------------------------- registry --

const QString PreviewMapToolManager::kPan = QStringLiteral( "pan" );
const QString PreviewMapToolManager::kZoomIn = QStringLiteral( "zoomIn" );
const QString PreviewMapToolManager::kZoomOut = QStringLiteral( "zoomOut" );
const QString PreviewMapToolManager::kIdentify = QStringLiteral( "identify" );
const QString PreviewMapToolManager::kMeasureLine = QStringLiteral( "measureLine" );
const QString PreviewMapToolManager::kMeasureArea = QStringLiteral( "measureArea" );
const QString PreviewMapToolManager::kProfile = QStringLiteral( "profile" );

PreviewMapToolManager::PreviewMapToolManager( QgsMapCanvas *canvas, QObject *parent )
  : QObject( parent )
  , m_canvas( canvas )
{
  registerBuiltins();
}

PreviewMapToolManager::~PreviewMapToolManager()
{
  // 工具的 QObject 父是 canvas（QgsMapTool 构造即挂 canvas）——画布销毁顺序
  // 先于本管理器时子链自动回收；这里只断转发连接（成员连接随本对象析构
  // 自动断开，无需手工 disconnect）。
}

void PreviewMapToolManager::registerBuiltins()
{
  if ( !m_canvas )
    return;
  m_tools.insert( kPan, new QgsMapToolPan( m_canvas ) );
  m_tools.insert( kZoomIn, new QgsMapToolZoom( m_canvas, false ) );
  m_tools.insert( kZoomOut, new QgsMapToolZoom( m_canvas, true ) );

  auto *identify = new PreviewIdentifyTool( m_canvas );
  connect( identify, &PreviewIdentifyTool::identifyPointRequested, this,
           &PreviewMapToolManager::identifyPointRequested );
  connect( identify, &PreviewIdentifyTool::identifyRectRequested, this,
           &PreviewMapToolManager::identifyRectRequested );
  connect( identify, &PreviewIdentifyTool::escapeRequested, this,
           &PreviewMapToolManager::cancelToDefault );
  m_tools.insert( kIdentify, identify );

  auto *measureLine = new PreviewMeasureTool( m_canvas, false );
  connect( measureLine, &PreviewMeasureTool::measurementChanged, this,
           &PreviewMapToolManager::measurementChanged );
  connect( measureLine, &PreviewMeasureTool::measurementCleared, this,
           &PreviewMapToolManager::measurementCleared );
  connect( measureLine, &PreviewMeasureTool::escapeRequested, this,
           &PreviewMapToolManager::cancelToDefault );
  m_tools.insert( kMeasureLine, measureLine );

  auto *measureArea = new PreviewMeasureTool( m_canvas, true );
  connect( measureArea, &PreviewMeasureTool::measurementChanged, this,
           &PreviewMapToolManager::measurementChanged );
  connect( measureArea, &PreviewMeasureTool::measurementCleared, this,
           &PreviewMapToolManager::measurementCleared );
  connect( measureArea, &PreviewMeasureTool::escapeRequested, this,
           &PreviewMapToolManager::cancelToDefault );
  m_tools.insert( kMeasureArea, measureArea );

  auto *profile = new PreviewProfileTool( m_canvas );
  connect( profile, &PreviewProfileTool::profileLineDrawn, this,
           &PreviewMapToolManager::profileLineDrawn );
  connect( profile, &PreviewProfileTool::profileLinesCleared, this,
           &PreviewMapToolManager::profileLinesCleared );
  connect( profile, &PreviewProfileTool::escapeRequested, this,
           &PreviewMapToolManager::cancelToDefault );
  m_tools.insert( kProfile, profile );

  m_displayNames = {
      { kPan, QStringLiteral( "漫游" ) },
      { kZoomIn, QStringLiteral( "框选放大" ) },
      { kZoomOut, QStringLiteral( "框选缩小" ) },
      { kIdentify, QStringLiteral( "识别要素" ) },
      { kMeasureLine, QStringLiteral( "距离测量" ) },
      { kMeasureArea, QStringLiteral( "面积测量" ) },
      { kProfile, QStringLiteral( "层位剖面线" ) },
  };
}

QStringList PreviewMapToolManager::toolIds() const
{
  return { kPan, kZoomIn, kZoomOut, kIdentify, kMeasureLine, kMeasureArea, kProfile };
}

QString PreviewMapToolManager::toolDisplayName( const QString &id ) const
{
  return m_displayNames.value( id );
}

bool PreviewMapToolManager::activate( const QString &id )
{
  QgsMapTool *t = m_tools.value( id );
  if ( !t || !m_canvas )
    return false;
  if ( id == m_activeId )
    return true; // 幂等
  const QString prev = m_activeId;
  m_canvas->setMapTool( t ); // QGIS 内部先 deactivate 旧工具
  m_activeId = id;
  if ( !prev.isEmpty() && prev != id )
    emit toolDeactivated( prev );
  emit toolActivated( id );
  return true;
}

void PreviewMapToolManager::cancelToDefault()
{
  if ( m_activeId.isEmpty() || m_activeId == kPan )
  {
    activate( kPan ); // 无工具在场时也保证 pan 在岗
    return;
  }
  activate( kPan );
}

QString PreviewMapToolManager::activeToolId() const
{
  return m_activeId;
}

QgsMapTool *PreviewMapToolManager::tool( const QString &id ) const
{
  return m_tools.value( id );
}
