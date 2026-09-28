// 层：视图
#include "typedconstraintdrawcontroller.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../workflow/workflows.h"
#include "maptools/paleomaptools.h"
#include "maptools/paleoshapetools.h"

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgsmapcanvas.h>

// 层：视图
// m2(B) 三入口（物源线/展布线/控制点）——结构镜像 ConstraintDrawController
// （§42），差异仅在提交：type 列写地质类型词表而不是 shape。

TypedConstraintDrawController::TypedConstraintDrawController( QgisCanvasController *canvasCtl,
                                                              ConstraintWorkflow *wf,
                                                              QObject *parent )
  : QObject( parent )
  , m_canvasCtl( canvasCtl )
  , m_wf( wf )
{
}

void TypedConstraintDrawController::shareCadDock( QgsAdvancedDigitizingDockWidget *dock )
{
  if ( m_ownCadDock && m_cadDock && m_cadDock != dock )
    m_cadDock->deleteLater(); // 换共享 dock，弃自建
  m_ownCadDock = false;
  m_cadDock = dock;
}

void TypedConstraintDrawController::startCapture( const QString &horizon, const QString &shape,
                                                  const QString &constraintType, int faciesCode )
{
  if ( m_tool )
    teardown(); // replace any live capture; the owner clicked a new entry

  if ( horizon.isEmpty() )
  {
    emit captureFailed( tr( "no active horizon — cannot draw a typed constraint" ) );
    return;
  }
  if ( !m_canvasCtl || !m_wf )
  {
    emit captureFailed( tr( "typed constraint drawing is not fully wired" ) );
    return;
  }
  if ( constraintType.isEmpty() )
  {
    emit captureFailed( tr( "typed constraint needs a constraint type" ) );
    return;
  }

  QgsMapCanvas *canvas = m_canvasCtl->canvas();
  if ( !m_cadDock )
  {
    m_cadDock = new QgsAdvancedDigitizingDockWidget( canvas, canvas );
    m_ownCadDock = true;
  }

  if ( shape == QLatin1String( "line" ) )
    m_tool = new PaleoDrawConstraintTool( canvas, m_cadDock );
  else if ( shape == QLatin1String( "point" ) )
    m_tool = new PaleoDrawPointTool( canvas, m_cadDock );
  else
  {
    emit captureFailed( tr( "unsupported typed constraint shape '%1'" ).arg( shape ) );
    return;
  }

  m_horizon = horizon;
  m_shape = shape;
  m_constraintType = constraintType;
  m_faciesCode = faciesCode;

  connect( m_tool, SIGNAL( constraintDrawn( QString ) ),
           this, SLOT( onDrawn( QString ) ) );
  connect( m_tool, SIGNAL( drawAborted() ),
           this, SLOT( onAborted() ) );

  m_canvasCtl->setMapTool( m_tool );
}

void TypedConstraintDrawController::cancel()
{
  if ( m_tool )
    teardown();
  emit captureCancelled();
}

void TypedConstraintDrawController::onDrawn( const QString &wkt )
{
  // 提交时 type 列落地质类型（物源线/展布线/控制点），facies code 与 horizon
  // 语义同通用约束。同 layerId 声明（constraints.<horizon>）照用。
  QString err;
  QString constraintId;
  const bool ok = m_wf->addConstraint( m_horizon, wkt, m_constraintType, m_faciesCode,
                                       &err, &constraintId );
  const QString horizon = m_horizon;
  const QString type = m_constraintType;
  teardown();
  if ( ok )
    emit captureFinished( horizon, type, constraintId );
  else
    emit captureFailed( err.isEmpty() ? tr( "typed constraint commit failed" ) : err );
}

void TypedConstraintDrawController::onAborted()
{
  teardown();
  emit captureCancelled();
}

void TypedConstraintDrawController::teardown()
{
  if ( !m_tool )
    return;
  QgsMapTool *tool = m_tool;
  m_tool = nullptr;
  m_horizon.clear();
  m_shape.clear();
  m_constraintType.clear();
  m_faciesCode = -1;
  if ( m_canvasCtl )
    m_canvasCtl->setMapTool( nullptr );
  tool->deleteLater();
}
