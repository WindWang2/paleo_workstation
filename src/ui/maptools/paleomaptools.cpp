#include "paleomaptools.h"

#include <algorithm>
#include <memory>

#include <QKeyEvent>

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgscompoundcurve.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgscurve.h>
#include <qgscurvepolygon.h>
#include <qgsexception.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspolygon.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

namespace
{
// QgsMapToolAdvancedDigitizing's ctor Q_ASSERTs a non-null dock and
// activate()/canvasReleaseEvent() dereference it unconditionally — fabricate a
// canvas-owned dock when the embedder does not inject a shared one.
QgsAdvancedDigitizingDockWidget *resolveCadDock( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *given )
{
  return given ? given : new QgsAdvancedDigitizingDockWidget( canvas, canvas );
}
} // namespace

PaleoDrawConstraintTool::PaleoDrawConstraintTool( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cadDock )
  : QgsMapToolCapture( canvas, resolveCadDock( canvas, cadDock ), CaptureLine )
{
  setToolName( tr( "Draw constraint" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

void PaleoDrawConstraintTool::activate()
{
  // Base chain: QgsMapToolEdit arms the canvas cursor, QgsMapToolAdvancedDigitizing
  // registers with the CAD dock, QgsMapToolCapture adds the extra snap layer.
  QgsMapToolCapture::activate();
  startCapturing(); // capture-mode state machine live ahead of the first vertex
}

void PaleoDrawConstraintTool::deactivate()
{
  stopCapturing(); // drop rubber bands + capture curve (idempotent)
  QgsMapToolCapture::deactivate();
}

void PaleoDrawConstraintTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit drawAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  QgsMapToolCapture::keyPressEvent( e ); // Esc → stopCapturing(), e->ignore()
}

void PaleoDrawConstraintTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  // Right-click commits the polyline once ≥2 vertices exist (base path);
  // with fewer it is a cancel gesture — the base just stopCapturing()s without
  // emitting lineCaptured(), so surface that as drawAborted().
  const bool cancelClick = e->button() == Qt::RightButton && size() < 2;
  QgsMapToolCapture::cadCanvasReleaseEvent( e );
  if ( cancelClick )
    emit drawAborted();
}

void PaleoDrawConstraintTool::lineCaptured( const QgsCurve *line )
{
  // The base calls lineCaptured( curveToAdd.release() ): ownership passes here.
  const std::unique_ptr<const QgsCurve> ownedLine( line );
  if ( !line || line->isEmpty() )
    return;

  // captureCurve() stores coordinates in the current vector layer's CRS when
  // one is set; the signal contract is canvas CRS, so reproject if they differ.
  std::unique_ptr<QgsCurve> canvasCurve( line->clone() );
  if ( QgsVectorLayer *vlayer = currentVectorLayer() )
  {
    const QgsCoordinateReferenceSystem layerCrs = vlayer->crs();
    const QgsCoordinateReferenceSystem canvasCrs = mCanvas->mapSettings().destinationCrs();
    if ( layerCrs.isValid() && canvasCrs.isValid() && layerCrs != canvasCrs )
    {
      try
      {
        canvasCurve->transform( QgsCoordinateTransform( layerCrs, canvasCrs, QgsProject::instance()->transformContext() ) );
      }
      catch ( QgsCsException & )
      {
        emit messageEmitted( tr( "Cannot transform constraint line to map coordinates" ), Qgis::MessageLevel::Warning );
        return;
      }
    }
  }

  // Straight-segment captures arrive wrapped in a one-segment QgsCompoundCurve;
  // the signal contract is plain LINESTRING WKT, so unwrap single-segment compounds.
  const QgsCurve *emitCurve = canvasCurve.get();
  if ( const QgsCompoundCurve *compound = qgsgeometry_cast<const QgsCompoundCurve *>( canvasCurve.get() ) )
  {
    if ( compound->nCurves() == 1 )
      emitCurve = compound->curveAt( 0 );
  }

  mPendingWkt = emitCurve->asWkt();
  cadLineCaptureFinished();
}

void PaleoDrawConstraintTool::cadLineCaptureFinished()
{
  if ( mPendingWkt.isEmpty() )
    return;
  const QString wkt = mPendingWkt;
  mPendingWkt.clear();
  emit constraintDrawn( wkt );
  stopCapturing(); // clean capture state/rubber band (idempotent; base also calls it)
}

// ---------------------------------------------------------------------------
// PaleoDrawPolygonTool
// ---------------------------------------------------------------------------

PaleoDrawPolygonTool::PaleoDrawPolygonTool( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cadDock )
  : QgsMapToolCapture( canvas, resolveCadDock( canvas, cadDock ), CapturePolygon )
{
  setToolName( tr( "Draw polygon constraint" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

void PaleoDrawPolygonTool::activate()
{
  // Same base chain as PaleoDrawConstraintTool::activate().
  QgsMapToolCapture::activate();
  startCapturing(); // capture-mode state machine live ahead of the first vertex
}

void PaleoDrawPolygonTool::deactivate()
{
  stopCapturing(); // drop rubber bands + capture curve (idempotent)
  QgsMapToolCapture::deactivate();
}

void PaleoDrawPolygonTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit drawAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  QgsMapToolCapture::keyPressEvent( e ); // Esc → stopCapturing(), e->ignore()
}

void PaleoDrawPolygonTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  // Right-click commits the polygon once ≥3 vertices exist (base path);
  // with fewer it is a cancel gesture — the base just stopCapturing()s without
  // emitting polygonCaptured(), so surface that as drawAborted().
  const bool cancelClick = e->button() == Qt::RightButton && size() < 3;
  QgsMapToolCapture::cadCanvasReleaseEvent( e );
  if ( cancelClick )
    emit drawAborted();
}

void PaleoDrawPolygonTool::polygonCaptured( const QgsCurvePolygon *polygon )
{
  // The base calls polygonCaptured( poly.get() ): a borrowed pointer valid for
  // this call only — clone before transforming (contrast lineCaptured(), which
  // releases the curve's ownership to the callee).
  if ( !polygon || polygon->isEmpty() )
    return;

  // captureCurve() stores coordinates in the current vector layer's CRS when
  // one is set; the signal contract is canvas CRS, so reproject if they differ.
  std::unique_ptr<QgsCurvePolygon> canvasPolygon( polygon->clone() );
  if ( QgsVectorLayer *vlayer = currentVectorLayer() )
  {
    const QgsCoordinateReferenceSystem layerCrs = vlayer->crs();
    const QgsCoordinateReferenceSystem canvasCrs = mCanvas->mapSettings().destinationCrs();
    if ( layerCrs.isValid() && canvasCrs.isValid() && layerCrs != canvasCrs )
    {
      try
      {
        canvasPolygon->transform( QgsCoordinateTransform( layerCrs, canvasCrs, QgsProject::instance()->transformContext() ) );
      }
      catch ( QgsCsException & )
      {
        emit messageEmitted( tr( "Cannot transform constraint polygon to map coordinates" ), Qgis::MessageLevel::Warning );
        return;
      }
    }
  }

  const QgsCurve *ring = canvasPolygon->exteriorRing();
  if ( !ring || ring->isEmpty() )
    return;

  // The base wraps the compound capture curve in a QgsCurvePolygon, so a naive
  // asWkt() would read "CurvePolygon (CompoundCurve (...))"; the signal
  // contract is plain "Polygon ((...))", so linearize the ring into a
  // QgsPolygon (setExteriorRing takes ownership and closes an open ring).
  QgsPolygon flat;
  flat.setExteriorRing( ring->curveToLine() );
  mPendingWkt = flat.asWkt();
  cadPolygonCaptureFinished();
}

void PaleoDrawPolygonTool::cadPolygonCaptureFinished()
{
  if ( mPendingWkt.isEmpty() )
    return;
  const QString wkt = mPendingWkt;
  mPendingWkt.clear();
  emit constraintDrawn( wkt );
  stopCapturing(); // clean capture state/rubber band (idempotent; base also calls it)
}

// ---------------------------------------------------------------------------
// PaleoDrawRectTool
// ---------------------------------------------------------------------------

PaleoDrawRectTool::PaleoDrawRectTool( QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cadDock )
  : QgsMapToolCapture( canvas, resolveCadDock( canvas, cadDock ), CapturePolygon )
{
  setToolName( tr( "Draw rectangle constraint" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  // NB: CaptureTechnique::Shape is unusable standalone — its concrete shape
  // tools (rectangle/ellipse/...) live in qgis_app, and with none registered
  // the base emits a "select a shape tool" warning and ignores clicks. The
  // polygon capture mode still gives a polygon rubber band for the preview.
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

void PaleoDrawRectTool::activate()
{
  QgsMapToolCapture::activate();
  startCapturing(); // capture-mode state machine live ahead of the first corner
}

void PaleoDrawRectTool::deactivate()
{
  stopCapturing(); // drop rubber bands + capture curve (idempotent)
  QgsMapToolCapture::deactivate();
}

void PaleoDrawRectTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit drawAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  QgsMapToolCapture::keyPressEvent( e ); // Esc → stopCapturing(), e->ignore()
}

void PaleoDrawRectTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  if ( e->button() == Qt::RightButton )
  {
    if ( size() >= 1 )
    {
      // Corner 2 = the cursor position (rectangle-from-extent convention).
      // Handle the commit here: the base would discard the capture curve
      // outright (CapturePolygon requires ≥3 vertices for its own commit).
      const QgsPointXY corner2 = e->mapPoint();
      emitRectangle( &corner2 );
      return;
    }
    // Bare right-click is a cancel gesture — the base stopCapturing()s.
    QgsMapToolCapture::cadCanvasReleaseEvent( e );
    emit drawAborted();
    return;
  }

  QgsMapToolCapture::cadCanvasReleaseEvent( e ); // left-click adds a corner vertex

  // A second left-click plants corner 2 → commit the axis-aligned rectangle.
  if ( e->button() == Qt::LeftButton && size() >= 2 )
    emitRectangle();
}

void PaleoDrawRectTool::emitRectangle( const QgsPointXY *eventCorner )
{
  const QgsCompoundCurve *curve = captureCurve();
  if ( !curve || size() < 1 )
    return;

  // captureCurve() stores coordinates in the current vector layer's CRS when
  // one is set; the signal contract is canvas CRS, so reproject if they differ.
  std::unique_ptr<QgsCurve> canvasCurve( curve->clone() );
  if ( QgsVectorLayer *vlayer = currentVectorLayer() )
  {
    const QgsCoordinateReferenceSystem layerCrs = vlayer->crs();
    const QgsCoordinateReferenceSystem canvasCrs = mCanvas->mapSettings().destinationCrs();
    if ( layerCrs.isValid() && canvasCrs.isValid() && layerCrs != canvasCrs )
    {
      try
      {
        canvasCurve->transform( QgsCoordinateTransform( layerCrs, canvasCrs, QgsProject::instance()->transformContext() ) );
      }
      catch ( QgsCsException & )
      {
        emit messageEmitted( tr( "Cannot transform constraint rectangle to map coordinates" ), Qgis::MessageLevel::Warning );
        return;
      }
    }
  }

  QgsPointSequence vertices;
  canvasCurve->points( vertices );
  if ( vertices.isEmpty() )
    return;

  const QgsPointXY c1( vertices.constFirst().x(), vertices.constFirst().y() );
  QgsPointXY c2;
  if ( eventCorner )
    c2 = *eventCorner; // right-click cursor position, already canvas CRS
  else if ( vertices.size() >= 2 )
    c2 = QgsPointXY( vertices.at( 1 ).x(), vertices.at( 1 ).y() );
  else
    return; // fewer than two corners resolve — not a rectangle yet

  const double xmin = std::min( c1.x(), c2.x() );
  const double xmax = std::max( c1.x(), c2.x() );
  const double ymin = std::min( c1.y(), c2.y() );
  const double ymax = std::max( c1.y(), c2.y() );

  QgsPolygon rect;
  rect.setExteriorRing( new QgsLineString( QVector<QgsPoint> {
      QgsPoint( xmin, ymin ), QgsPoint( xmax, ymin ), QgsPoint( xmax, ymax ),
      QgsPoint( xmin, ymax ), QgsPoint( xmin, ymin ) } ) );
  emit constraintDrawn( rect.asWkt() );
  stopCapturing(); // clean capture state/rubber band (idempotent)
}
