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
  m_shapePreview = std::make_unique<PaleoShapePreview>(mCanvas);
  QgsMapToolCapture::activate();
  startCapturing();
}

void PaleoDrawCircleTool::deactivate()
{
  m_shapePreview.reset();
  stopCapturing();
  QgsMapToolCapture::deactivate();
}

void PaleoDrawCircleTool::keyPressEvent( QKeyEvent *e )
{
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
  {
    const QgsGeometry geometry = m_shapePreview ? m_shapePreview->current() : QgsGeometry();
    if (!geometry.isNull())
    {
      stopCapturing();
      m_shapePreview->clear();
      emit constraintDrawn(geometry.asWkt(17));
    }
    e->accept();
    return;
  }

  if ( e->key() == Qt::Key_Escape )
    emit drawAborted();
  QgsMapToolCapture::keyPressEvent( e );
}

void PaleoDrawCircleTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  e->snapPoint();
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

void PaleoDrawCircleTool::cadCanvasMoveEvent(QgsMapMouseEvent *e)
{
  e->snapPoint();
  QgsMapToolCapture::cadCanvasMoveEvent(e);
  m_shapePreview->update(QStringLiteral("circle"), captureCurve(), currentVectorLayer(), e);
}

void PaleoDrawCircleTool::emitCircle(const QgsPointXY *cursor)
{
  const auto geometry = PaleoShapePreview::geometry(QStringLiteral("circle"), captureCurve(),
      currentVectorLayer(), mCanvas, cursor);
  if (geometry.isNull())
    return;
  stopCapturing();
  m_shapePreview->clear();
  emit constraintDrawn(geometry.asWkt(17));
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
  m_shapePreview = std::make_unique<PaleoShapePreview>(mCanvas);
  QgsMapToolCapture::activate();
  startCapturing();
}

void PaleoDrawEllipseTool::deactivate()
{
  m_shapePreview.reset();
  stopCapturing();
  QgsMapToolCapture::deactivate();
}

void PaleoDrawEllipseTool::keyPressEvent( QKeyEvent *e )
{
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
  {
    const QgsGeometry geometry = m_shapePreview ? m_shapePreview->current() : QgsGeometry();
    if (!geometry.isNull())
    {
      stopCapturing();
      m_shapePreview->clear();
      emit constraintDrawn(geometry.asWkt(17));
    }
    e->accept();
    return;
  }

  if ( e->key() == Qt::Key_Escape )
    emit drawAborted();
  QgsMapToolCapture::keyPressEvent( e );
}

void PaleoDrawEllipseTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  e->snapPoint();
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

void PaleoDrawEllipseTool::cadCanvasMoveEvent(QgsMapMouseEvent *e)
{
  e->snapPoint();
  QgsMapToolCapture::cadCanvasMoveEvent(e);
  m_shapePreview->update(QStringLiteral("ellipse"), captureCurve(), currentVectorLayer(), e);
}

void PaleoDrawEllipseTool::emitEllipse(const QgsPointXY *cursor)
{
  const auto geometry = PaleoShapePreview::geometry(QStringLiteral("ellipse"), captureCurve(),
      currentVectorLayer(), mCanvas, cursor);
  if (geometry.isNull())
    return;
  stopCapturing();
  m_shapePreview->clear();
  emit constraintDrawn(geometry.asWkt(17));
}
