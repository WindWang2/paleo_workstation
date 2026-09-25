#include "constraintdrawcontroller.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../workflow/workflows.h"
#include "maptools/paleomaptools.h"
#include "maptools/paleoshapetools.h"

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgsmapcanvas.h>

ConstraintDrawController::ConstraintDrawController( QgisCanvasController *canvasCtl,
                                                    ConstraintWorkflow *wf,
                                                    QObject *parent )
  : QObject( parent )
  , m_canvasCtl( canvasCtl )
  , m_wf( wf )
{
}

void ConstraintDrawController::startCapture( const QString &horizon,
                                             const QString &shape,
                                             int faciesCode )
{
  if ( m_tool )
    teardown(); // replace any live capture; the owner clicked a new shape

  if ( horizon.isEmpty() )
  {
    emit captureFailed( tr( "no active horizon — cannot draw a constraint" ) );
    return;
  }
  if ( !m_canvasCtl || !m_wf )
  {
    emit captureFailed( tr( "constraint drawing is not fully wired" ) );
    return;
  }

  QgsMapCanvas *canvas = m_canvasCtl->canvas();
  if ( !m_cadDock )
    m_cadDock = new QgsAdvancedDigitizingDockWidget( canvas, canvas );

  if ( shape == QLatin1String( "polygon" ) )
    m_tool = new PaleoDrawPolygonTool( canvas, m_cadDock );
  else if ( shape == QLatin1String( "rect" ) )
    m_tool = new PaleoDrawRectTool( canvas, m_cadDock );
  else if ( shape == QLatin1String( "line" ) )
    m_tool = new PaleoDrawConstraintTool( canvas, m_cadDock );
  else if ( shape == QLatin1String( "point" ) )
    m_tool = new PaleoDrawPointTool( canvas, m_cadDock );
  else if ( shape == QLatin1String( "circle" ) )
    m_tool = new PaleoDrawCircleTool( canvas, m_cadDock );
  else if ( shape == QLatin1String( "ellipse" ) )
    m_tool = new PaleoDrawEllipseTool( canvas, m_cadDock );
  else
  {
    emit captureFailed( tr( "unknown constraint shape '%1'" ).arg( shape ) );
    return;
  }

  m_horizon = horizon;
  m_shape = shape;
  m_faciesCode = faciesCode;

  connect( m_tool, SIGNAL( constraintDrawn( QString ) ),
           this, SLOT( onDrawn( QString ) ) );
  connect( m_tool, SIGNAL( drawAborted() ),
           this, SLOT( onAborted() ) );

  m_canvasCtl->setMapTool( m_tool ); // activates; the Esc/right-click aborts
                                     // come back through onAborted()
}

void ConstraintDrawController::cancel()
{
  if ( m_tool )
    teardown();
  emit captureCancelled();
}

void ConstraintDrawController::onDrawn( const QString &wkt )
{
  // commit through the workflow, then tear down — the tool has already
  // finished its capture stroke (stopCapturing inside the emit path).
  QString err;
  QString constraintId;
  const bool ok = m_wf->addConstraint( m_horizon, wkt, m_shape, m_faciesCode,
                                       &err, &constraintId );
  const QString horizon = m_horizon; // capture before teardown clears state
  teardown();
  if ( ok )
    emit captureFinished( horizon, constraintId );
  else
    emit captureFailed( err.isEmpty() ? tr( "constraint commit failed" ) : err );
}

void ConstraintDrawController::onAborted()
{
  teardown();
  emit captureCancelled();
}

void ConstraintDrawController::teardown()
{
  if ( !m_tool )
    return;
  QgsMapTool *tool = m_tool;
  m_tool = nullptr;
  m_horizon.clear();
  m_shape.clear();
  m_faciesCode = -1;
  if ( m_canvasCtl )
    m_canvasCtl->setMapTool( nullptr ); // proper unset path (deactivate)
  tool->deleteLater();
}
