// 层：视图
#include "constraintdrawcontroller.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/constraintcrs.h"
#include "../workflow/workflows.h"
#include "../metadata/paleoprojectstore.h"
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
    emit captureFailed( tr( "没有活动层位——无法绘制约束" ) );
    return;
  }
  if ( !m_canvasCtl || !m_wf )
  {
    emit captureFailed( tr( "约束绘制尚未完成接线" ) );
    return;
  }

  if (auto *store = m_wf->projectStore(); store && store->isReadOnly())
  {
    emit captureFailed(tr("工程为只读，不能绘制约束"));
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
    emit captureFailed( tr( "未知的约束形状「%1」" ).arg( shape ) );
    return;
  }

  m_horizon = horizon;
  m_shape = shape;
  m_faciesCode = faciesCode;

  connect( m_tool, SIGNAL( constraintDrawn( QString ) ),
           this, SLOT( onDrawn( QString ) ) );
  connect( m_tool, SIGNAL( drawAborted() ),
           this, SLOT( onAborted() ) );

  // 画布换工具哨子：本工具被外部换走（原生控件 / 编辑工具条抢装、Esc 停止
  // 按钮走 deactivateTool）时，控制器经画布 mapToolSet 的 oldTool 认出自己
  // 被卸，走同一条收尾路径——否则 m_tool 陈旧：active() 谎报绘制中，
  // teardown 还会把画布上那个外来工具当成自己的 unset 掉（踢用户工具）。
  // 自走 teardown 时 m_tool 已先清零，oldTool 对不上即不会误触。
  disconnect( m_toolWatch );
  m_toolWatch = connect( canvas, &QgsMapCanvas::mapToolSet, this,
                         [this]( QgsMapTool *newTool, QgsMapTool *oldTool ) {
                           if ( oldTool == m_tool && newTool != m_tool )
                             onAborted(); // 外部卸载：等同 Esc 中止的收尾
                         } );

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
  // P1-C：工具输出画布 CRS 坐标；入库前显式变换到约束库 CRS（本地工程
  // 网格），配准工程走工程转换上下文里的显式坐标操作。失败如实报错不入库。
  QString storeWkt;
  QgsMapCanvas *canvas = m_canvasCtl ? m_canvasCtl->canvas() : nullptr;
  if ( !paleo::constraintcrs::toStoreWkt(
           wkt, canvas ? canvas->mapSettings().destinationCrs() : QgsCoordinateReferenceSystem(),
           canvas ? canvas->mapSettings().transformContext() : QgsCoordinateTransformContext(), &storeWkt,
           &err ) )
  {
    teardown();
    emit captureFailed( err );
    return;
  }
  const bool ok = m_wf->drawConstraint( m_horizon, storeWkt, m_shape, m_faciesCode,
                                       &err, &constraintId );
  const QString horizon = m_horizon; // capture before teardown clears state
  teardown();
  if ( ok )
    emit captureFinished( horizon, constraintId );
  else
    emit captureFailed( err.isEmpty() ? tr( "约束提交失败" ) : err );
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
  // 只在画布当前工具仍是本控制器的工具时才走正式 unset（deactivate）路径——
  // 工具已被外部换走时，画布上那个工具是别人的，unset 它会踢掉用户正在用的
  // 工具（外部换走后的陈旧 m_tool 经 mapToolSet 哨子已同步清零，见
  // startCapture）。
  if ( m_canvasCtl && m_canvasCtl->activeTool() == tool )
    m_canvasCtl->setMapTool( nullptr ); // proper unset path (deactivate)
  tool->deleteLater();
}
