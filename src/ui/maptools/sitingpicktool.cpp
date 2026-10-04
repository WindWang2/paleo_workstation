// 层：视图
#include "sitingpicktool.h"

#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspointlocator.h>
#include <qgssnapindicator.h>

PaleoSitingPickTool::PaleoSitingPickTool( QgsMapCanvas *canvas )
  : QgsMapToolEmitPoint( canvas )
{
  setCursor( Qt::CrossCursor );
}

void PaleoSitingPickTool::activate()
{
  QgsMapToolEmitPoint::activate();
  if ( canvas() && !m_snapIndicator )
    m_snapIndicator = std::make_unique<QgsSnapIndicator>( canvas() );
}

void PaleoSitingPickTool::deactivate()
{
  if ( m_snapIndicator )
    m_snapIndicator->setMatch( QgsPointLocator::Match() );
  QgsMapToolEmitPoint::deactivate();
}

void PaleoSitingPickTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  // 悬停吸附反馈：match 可视化，落点以 snapPoint() 为准（同 vertex 工具）。
  if ( m_snapIndicator && e )
  {
    e->snapPoint();
    m_snapIndicator->setMatch( e->mapPointMatch() );
  }
  QgsMapToolEmitPoint::canvasMoveEvent( e );
}

void PaleoSitingPickTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( !e || e->button() != Qt::LeftButton )
    return;
  const QgsPointXY pt = e->snapPoint(); // 无命中时回原始点位
  emit pointPicked( pt.x(), pt.y() );
  if ( canvas() )
    canvas()->unsetMapTool( this ); // 单发：放完即退出
  deleteLater(); // canvas 不持有 map tool——自清防会话级积累
}

void PaleoSitingPickTool::keyPressEvent( QKeyEvent *e )
{
  if ( e && e->key() == Qt::Key_Escape )
  {
    emit pickAborted();
    if ( canvas() )
      canvas()->unsetMapTool( this );
    deleteLater();
    return;
  }
  QgsMapToolEmitPoint::keyPressEvent( e );
}
