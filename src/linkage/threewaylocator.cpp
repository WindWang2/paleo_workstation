// 层：功能
#include "threewaylocator.h"

#include "../qgis/qgiscanvascontroller.h"

#include <QRegularExpression>

#include <cmath>

ThreeWayLocator::ThreeWayLocator( QgisCanvasController *canvas, QObject *parent )
  : QObject( parent )
  , m_canvas( canvas )
{
}

void ThreeWayLocator::locate( const QString &layerId, const QString &wktLocation,
                              const QVariantMap &payload )
{
  // ① 地图：井点优先 —— payload 的 well_x/well_y 是工程网格局部米，
  // wktLocation「POINT(x y)」兜底；都没有才退回问题图层缩放（非残差问题）。
  // 缺字段时 QVariant().toDouble() 给 0.0——必须用转换结果判断有无，
  // 不能把 (0,0) 当合法井点飞过去。
  bool okx = false, oky = false;
  double px = payload.value( QStringLiteral( "well_x" ) ).toDouble( &okx );
  double py = payload.value( QStringLiteral( "well_y" ) ).toDouble( &oky );
  bool havePoint = okx && oky && std::isfinite( px ) && std::isfinite( py );
  if ( !havePoint )
  {
    static const QRegularExpression kPointRe(
        QStringLiteral( "^\\s*POINT\\s*\\(\\s*(-?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?)\\s+(-?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?)\\s*\\)\\s*$" ),
        QRegularExpression::CaseInsensitiveOption );
    const QRegularExpressionMatch m = kPointRe.match( wktLocation );
    if ( m.hasMatch() )
    {
      px = m.captured( 1 ).toDouble();
      py = m.captured( 2 ).toDouble();
      havePoint = std::isfinite( px ) && std::isfinite( py );
    }
  }
  if ( havePoint )
  {
    m_hasMapPoint = true;
    m_lastMapPoint = QPointF( px, py );
    if ( m_canvas )
      m_canvas->zoomToPoint( px, py ); // 井点居中，保持当前比例尺
  }
  else if ( m_canvas && !layerId.isEmpty() )
    m_canvas->zoomToLayer( layerId );

  // ② 连井剖面：先发底栏切页意图（停靠里的面板要先可见），再发滚动意图。
  // inline/time_ms 不再走地震 gotoLine —— 底栏地震标签已随预览壳重排移除，
  // 测线跳转由验证页「在数据页看这条剖面」负责。
  const QString wellId = payload.value( QStringLiteral( "wellId" ) ).toString();
  if ( !wellId.isEmpty() )
  {
    emit bottomTabFocusRequested( QStringLiteral( "correlation" ) );
    emit correlationFocusRequested(
        wellId, payload.value( QStringLiteral( "horizon" ) ).toString() );
  }
}
