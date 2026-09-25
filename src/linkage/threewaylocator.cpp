#include "threewaylocator.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../ui/correlationpanel.h"
#include "../ui/pages/pagepanels.h"
#include "../ui/seismicpreviewpanel.h"

ThreeWayLocator::ThreeWayLocator( QgisCanvasController *canvas, WellCorrelationPanel *wellPanel,
                                  SeismicPreviewPanel *seismicPanel, QObject *parent )
  : QObject( parent )
  , m_canvas( canvas )
  , m_wellPanel( wellPanel )
  , m_seismicPanel( seismicPanel )
{
}

void ThreeWayLocator::attach( ValidatePage *page )
{
  if ( !page )
    return;
  connect( page, &ValidatePage::locateRequested, this,
           [this]( const QString &layerId, const QString &wkt, const QVariantMap &payload ) {
             locate( layerId, wkt, payload );
           } );
}

void ThreeWayLocator::locate( const QString &layerId, const QString &wktLocation,
                              const QVariantMap &payload )
{
  // ① 地图：问题图层缩放（残差问题的 layerId = 层位栅格声明）。
  if ( m_canvas && !layerId.isEmpty() )
    m_canvas->zoomToLayer( layerId );

  // ② 连井剖面：滚到该井该分层。
  const QString wellId = payload.value( QStringLiteral( "wellId" ) ).toString();
  if ( m_wellPanel && !wellId.isEmpty() )
    m_wellPanel->scrollToWellTop( wellId, payload.value( QStringLiteral( "horizon" ) ).toString() );

  // ③ 地震剖面：滚到目标测线与目标时间（inline 缺省/-1 = 未知，跳过）。
  const QVariant inlineVar = payload.value( QStringLiteral( "inline" ) );
  const int inlineNo = inlineVar.isValid() ? inlineVar.toInt() : -1;
  if ( m_seismicPanel && inlineNo >= 0 )
  {
    const double timeMs = payload.value( QStringLiteral( "time_ms" ) ).toDouble();
    m_seismicPanel->gotoLine( inlineNo, qIsNaN( timeMs ) ? 0.0 : timeMs );
  }
  Q_UNUSED( wktLocation ); // 井位 WKT 已编码进 payload 的 well_x/well_y
}
