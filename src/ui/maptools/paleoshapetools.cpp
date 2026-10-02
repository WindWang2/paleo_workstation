// 层：视图
#include "paleoshapetools.h"
#include "capturehelpers.h"

#include <cmath>
#include <numbers> // std::numbers::pi——M_PI 在 MSVC <cmath> 下不定义
#include <memory>

#include <QKeyEvent>

#include <qgscompoundcurve.h>
#include <qgsellipse.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspoint.h>
#include <qgspolygon.h>

// ---------------------------------------------------------------------------
// PaleoDrawPointTool
// ---------------------------------------------------------------------------

PaleoDrawPointTool::PaleoDrawPointTool( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cadDock )
  : QgsMapToolCapture( canvas, CaptureHelpers::resolveCadDock( canvas, cadDock ), CapturePoint )
{
  setToolName( tr( "绘制点约束" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
}

void PaleoDrawPointTool::activate()
{
  QgsMapToolCapture::activate();
  startCapturing();
}

void PaleoDrawPointTool::deactivate()
{
  stopCapturing();
  QgsMapToolCapture::deactivate();
}

void PaleoDrawPointTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit drawAborted();
  QgsMapToolCapture::keyPressEvent( e );
}

void PaleoDrawPointTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( e->button() == Qt::RightButton )
  {
    emit drawAborted();
    stopCapturing();
    return;
  }

  // Left click calls base which invokes pointCaptured(savePoint)
  QgsMapToolCapture::cadCanvasReleaseEvent( e );
}

void PaleoDrawPointTool::pointCaptured( const QgsPoint &point )
{
  if ( point.isEmpty() )
    return;

  // Reproject from current vector layer's CRS to canvas CRS if they differ
  QgsPoint canvasPt = point;
  if ( !CaptureHelpers::transformToCanvas( canvasPt, currentVectorLayer(), mCanvas ) )
  {
    emit messageEmitted( tr( "无法将约束点转换到地图坐标" ), Qgis::MessageLevel::Warning );
    return;
  }

  emit constraintDrawn( canvasPt.asWkt() );
  stopCapturing();
}

// ---------------------------------------------------------------------------
// PaleoDrawCircleTool
// ---------------------------------------------------------------------------

PaleoDrawCircleTool::PaleoDrawCircleTool( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cadDock )
  : QgsMapToolCapture( canvas, CaptureHelpers::resolveCadDock( canvas, cadDock ), CapturePolygon )
{
  setToolName( tr( "绘制圆形约束" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

void PaleoDrawCircleTool::activate()
{
  QgsMapToolCapture::activate();
  startCapturing();
}

void PaleoDrawCircleTool::deactivate()
{
  stopCapturing();
  QgsMapToolCapture::deactivate();
}

void PaleoDrawCircleTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit drawAborted();
  QgsMapToolCapture::keyPressEvent( e );
}

void PaleoDrawCircleTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( e->button() == Qt::RightButton )
  {
    if ( size() >= 1 )
    {
      // Radius point = cursor position
      const QgsPointXY radiusPoint = e->mapPoint();
      emitCircle( &radiusPoint );
      return;
    }
    // Bare right-click is a cancel gesture
    QgsMapToolCapture::cadCanvasReleaseEvent( e );
    emit drawAborted();
    return;
  }

  QgsMapToolCapture::cadCanvasReleaseEvent( e );

  if ( e->button() == Qt::LeftButton && size() >= 2 )
    emitCircle();
}

void PaleoDrawCircleTool::emitCircle( const QgsPointXY *eventRadiusPoint )
{
  const QgsCompoundCurve *curve = captureCurve();
  if ( !curve || size() < 1 )
    return;

  std::unique_ptr<QgsCurve> canvasCurve( curve->clone() );
  if ( !CaptureHelpers::transformToCanvas( *canvasCurve, currentVectorLayer(), mCanvas ) )
  {
    emit messageEmitted( tr( "无法将约束圆转换到地图坐标" ), Qgis::MessageLevel::Warning );
    return;
  }

  QgsPointSequence vertices;
  canvasCurve->points( vertices );
  if ( vertices.isEmpty() )
    return;

  const QgsPointXY center( vertices.constFirst().x(), vertices.constFirst().y() );
  QgsPointXY radPt;
  if ( eventRadiusPoint )
    radPt = *eventRadiusPoint;
  else if ( vertices.size() >= 2 )
    radPt = QgsPointXY( vertices.at( 1 ).x(), vertices.at( 1 ).y() );
  else
    return;

  const double dx = radPt.x() - center.x();
  const double dy = radPt.y() - center.y();
  const double r = std::hypot( dx, dy );
  if ( r <= 0.0 )
    return;

  const int numSegments = 24;
  QVector<QgsPoint> ringPts;
  ringPts.reserve( numSegments + 1 );
  for ( int i = 0; i <= numSegments; ++i )
  {
    const double angle = 2.0 * std::numbers::pi * i / numSegments;
    ringPts.append( QgsPoint( center.x() + r * std::cos( angle ),
                              center.y() + r * std::sin( angle ) ) );
  }

  QgsPolygon poly;
  poly.setExteriorRing( new QgsLineString( ringPts ) );
  emit constraintDrawn( poly.asWkt() );
  stopCapturing();
}

// ---------------------------------------------------------------------------
// PaleoDrawEllipseTool
// ---------------------------------------------------------------------------

PaleoDrawEllipseTool::PaleoDrawEllipseTool( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cadDock )
  : QgsMapToolCapture( canvas, CaptureHelpers::resolveCadDock( canvas, cadDock ), CapturePolygon )
{
  setToolName( tr( "绘制椭圆约束" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

void PaleoDrawEllipseTool::activate()
{
  QgsMapToolCapture::activate();
  startCapturing();
}

void PaleoDrawEllipseTool::deactivate()
{
  stopCapturing();
  QgsMapToolCapture::deactivate();
}

void PaleoDrawEllipseTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit drawAborted();
  QgsMapToolCapture::keyPressEvent( e );
}

void PaleoDrawEllipseTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( e->button() == Qt::RightButton )
  {
    if ( size() >= 2 )
    {
      const QgsPointXY axis2Point = e->mapPoint();
      emitEllipse( &axis2Point );
      return;
    }
    // Bare right-click or right-click with 1 point is a cancel gesture
    QgsMapToolCapture::cadCanvasReleaseEvent( e );
    emit drawAborted();
    return;
  }

  QgsMapToolCapture::cadCanvasReleaseEvent( e );

  if ( e->button() == Qt::LeftButton && size() >= 3 )
    emitEllipse();
}

void PaleoDrawEllipseTool::emitEllipse( const QgsPointXY *eventAxis2Point )
{
  const QgsCompoundCurve *curve = captureCurve();
  if ( !curve || size() < 2 )
    return;

  std::unique_ptr<QgsCurve> canvasCurve( curve->clone() );
  if ( !CaptureHelpers::transformToCanvas( *canvasCurve, currentVectorLayer(), mCanvas ) )
  {
    emit messageEmitted( tr( "无法将约束椭圆转换到地图坐标" ), Qgis::MessageLevel::Warning );
    return;
  }

  QgsPointSequence vertices;
  canvasCurve->points( vertices );
  if ( vertices.size() < 2 )
    return;

  const QgsPoint center( vertices.at( 0 ).x(), vertices.at( 0 ).y() );
  const QgsPoint pt1( vertices.at( 1 ).x(), vertices.at( 1 ).y() );
  QgsPoint pt2;
  if ( eventAxis2Point )
    pt2 = QgsPoint( eventAxis2Point->x(), eventAxis2Point->y() );
  else if ( vertices.size() >= 3 )
    pt2 = QgsPoint( vertices.at( 2 ).x(), vertices.at( 2 ).y() );
  else
    return;

  if ( center.distance( pt1 ) <= 0.0 || center.distance( pt2 ) <= 0.0 )
    return;

  QgsEllipse elp = QgsEllipse::fromCenter2Points( center, pt1, pt2 );
  if ( elp.isEmpty() )
    return;

  std::unique_ptr<QgsPolygon> poly( elp.toPolygon( 36 ) );
  if ( !poly )
    return;

  emit constraintDrawn( poly->asWkt() );
  stopCapturing();
}
