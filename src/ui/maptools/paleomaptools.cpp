#include "paleomaptools.h"

#include <memory>

#include <QKeyEvent>

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgscompoundcurve.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgscurve.h>
#include <qgsexception.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
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
