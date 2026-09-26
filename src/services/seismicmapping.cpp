#include "seismicmapping.h"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace
{
  void setReason( QString *reason, const QString &text )
  {
    if ( reason )
      *reason = text;
  }
} // namespace

// ---- SurveyGridGeometry -----------------------------------------------------

SurveyGridGeometry SurveyGridGeometry::fromHorizonHeader( const HorizonHeader &h )
{
  SurveyGridGeometry g;
  if ( !h.hasP1 || !h.hasP2 || !h.hasP3 )
    return g;
  if ( h.p2Xline == h.p1Xline || h.p3Inline == h.p2Inline )
    return g; // 锚点退化：crossline/inline 方向步数为 0

  const double dxl = static_cast<double>( h.p2Xline - h.p1Xline );
  const double din = static_cast<double>( h.p3Inline - h.p2Inline );
  g.a = ( h.p2x - h.p1x ) / dxl;
  g.c = ( h.p2y - h.p1y ) / dxl;
  g.b = ( h.p3x - h.p2x ) / din;
  g.d = ( h.p3y - h.p2y ) / din;
  g.p1x = h.p1x;
  g.p1y = h.p1y;
  g.p1Inline = h.p1Inline;
  g.p1Xline = h.p1Xline;
  g.inlineMin = std::min( h.p1Inline, h.p3Inline );
  g.inlineMax = std::max( h.p1Inline, h.p3Inline );
  g.xlineMin = std::min( h.p1Xline, h.p2Xline );
  g.xlineMax = std::max( h.p1Xline, h.p2Xline );

  const double det = g.a * g.d - g.b * g.c;
  const bool finite = std::isfinite( g.a ) && std::isfinite( g.b ) &&
                      std::isfinite( g.c ) && std::isfinite( g.d );
  g.valid = finite && std::fabs( det ) > 1e-12;
  return g;
}

void SurveyGridGeometry::inlineXlineToXy( double inlineNo, double xline, double *x, double *y ) const
{
  const double dxl = xline - p1Xline;
  const double din = inlineNo - p1Inline;
  if ( x )
    *x = p1x + a * dxl + b * din;
  if ( y )
    *y = p1y + c * dxl + d * din;
}

void SurveyGridGeometry::xyToInlineXlineContinuous( double x, double y, double *inlineNo,
                                                    double *xline ) const
{
  // 逆矩阵 [a b; c d]⁻¹ = 1/det·[d −b; −c a]：din（inline 步）由 ey 主导。
  const double det = a * d - b * c;
  const double ex = x - p1x;
  const double ey = y - p1y;
  if ( inlineNo )
    *inlineNo = p1Inline + ( a * ey - c * ex ) / det;
  if ( xline )
    *xline = p1Xline + ( d * ex - b * ey ) / det;
}

bool SurveyGridGeometry::inlineXlineInside( int inlineNo, int xline ) const
{
  return inlineNo >= inlineMin && inlineNo <= inlineMax &&
         xline >= xlineMin && xline <= xlineMax;
}

bool SurveyGridGeometry::xyToInlineXline( double x, double y, int *inlineNo, int *xline,
                                          QString *reason ) const
{
  if ( !valid )
  {
    setReason( reason, QStringLiteral( "测网几何不可用（锚点缺失或退化）" ) );
    return false;
  }
  if ( !std::isfinite( x ) || !std::isfinite( y ) )
  {
    setReason( reason, QStringLiteral( "坐标 (%1,%2) 不是有限值" ).arg( x ).arg( y ) );
    return false;
  }
  double ci = 0.0, cx = 0.0;
  xyToInlineXlineContinuous( x, y, &ci, &cx );
  const int inl = static_cast<int>( std::lround( ci ) );
  const int xl = static_cast<int>( std::lround( cx ) );
  if ( !inlineXlineInside( inl, xl ) )
  {
    // 超网明确失败，绝不夹取（§40 依赖契约：调用方拿到的是失败而非边界值）。
    setReason( reason, QStringLiteral( "坐标 (%1,%2) 超出测网 [inline %3–%4 × crossline %5–%6]" )
                             .arg( x, 0, 'f', 2 )
                             .arg( y, 0, 'f', 2 )
                             .arg( inlineMin )
                             .arg( inlineMax )
                             .arg( xlineMin )
                             .arg( xlineMax ) );
    return false;
  }
  if ( inlineNo )
    *inlineNo = inl;
  if ( xline )
    *xline = xl;
  return true;
}

// ---- LineCdpMap -------------------------------------------------------------

LineCdpMap LineCdpMap::fromObservations( const QVector<QVector<qint64>> &observations )
{
  LineCdpMap map;
  map.valid = false;
  // 按 inline 分桶，桶内按 xline 排序后拟合仿射。
  QHash<int, QVector<QPair<qint64, qint64>>> byLine; // inline -> (xline, cdp)
  for ( const QVector<qint64> &o : observations )
  {
    if ( o.size() < 3 )
      continue;
    byLine[static_cast<int>( o[0] )].append( { o[1], o[2] } );
  }
  for ( auto it = byLine.begin(); it != byLine.end(); ++it )
  {
    QVector<QPair<qint64, qint64>> pts = it.value();
    std::sort( pts.begin(), pts.end(),
               []( const QPair<qint64, qint64> &l, const QPair<qint64, qint64> &r ) {
                 return l.first < r.first;
               } );
    pts.erase( std::unique( pts.begin(), pts.end(),
                            []( const QPair<qint64, qint64> &l, const QPair<qint64, qint64> &r ) {
                              return l.first == r.first;
                            } ),
               pts.end() );
    if ( pts.isEmpty() )
      continue;
    LineAffine affine;
    affine.firstXline = static_cast<int>( pts.first().first );
    affine.baseCdp = pts.first().second;
    if ( pts.size() >= 2 )
    {
      const double span = static_cast<double>( pts.last().first - pts.first().first );
      affine.cdpPerXline = static_cast<double>( pts.last().second - pts.first().second ) / span;
      // 全部观测须落在首末连线上（仿射可复现，容差半道）——否则整线拒绝。
      bool affineOk = true;
      for ( const auto &p : pts )
      {
        const double expected = affine.baseCdp +
                                affine.cdpPerXline *
                                    static_cast<double>( p.first - pts.first().first );
        if ( std::fabs( expected - static_cast<double>( p.second ) ) > 0.5 )
        {
          affineOk = false;
          break;
        }
      }
      if ( !affineOk )
        continue; // 该线不可信：不进映射（查询走「测线无 CDP 映射」原因）
    }
    else
      affine.cdpPerXline = 0.0; // 单点线：只能报出该点 CDP
    map.lines.insert( it.key(), affine );
    map.valid = true;
  }
  return map;
}

bool LineCdpMap::cdpFor( int inlineNo, int xline, qint64 *cdp, QString *reason ) const
{
  const auto it = lines.constFind( inlineNo );
  if ( it == lines.constEnd() )
  {
    setReason( reason, QStringLiteral( "测线 %1 没有 CDP 映射（未观测或道头非仿射被拒）" )
                             .arg( inlineNo ) );
    return false;
  }
  if ( cdp )
    *cdp = it->baseCdp + static_cast<qint64>( std::llround(
                             it->cdpPerXline * static_cast<double>( xline - it->firstXline ) ) );
  return true;
}

bool LineCdpMap::xlineFor( int inlineNo, qint64 cdp, int *xline, QString *reason ) const
{
  const auto it = lines.constFind( inlineNo );
  if ( it == lines.constEnd() )
  {
    setReason( reason, QStringLiteral( "测线 %1 没有 CDP 映射（未观测或道头非仿射被拒）" )
                             .arg( inlineNo ) );
    return false;
  }
  if ( std::fabs( it->cdpPerXline ) < 1e-12 )
  {
    setReason( reason, QStringLiteral( "测线 %1 的 CDP 编号不随 crossline 变化，无法反解" )
                             .arg( inlineNo ) );
    return false;
  }
  const double xl = it->firstXline +
                    static_cast<double>( cdp - it->baseCdp ) / it->cdpPerXline;
  const int rounded = static_cast<int>( std::lround( xl ) );
  // 反解必须可逆：roundtrip 对不上说明该 CDP 不落在本线格点上。
  qint64 back = 0;
  if ( !cdpFor( inlineNo, rounded, &back ) || back != cdp )
  {
    setReason( reason, QStringLiteral( "CDP %1 不在测线 %2 的格点序列上" ).arg( cdp ).arg( inlineNo ) );
    return false;
  }
  if ( xline )
    *xline = rounded;
  return true;
}

// ---- VelocityModel ----------------------------------------------------------

void VelocityModel::setWellTable( const QString &wellId, const TimeDepthTable &table )
{
  m_wells.insert( wellId, table );
}

bool VelocityModel::hasWell( const QString &wellId ) const
{
  return m_wells.contains( wellId );
}

void VelocityModel::clear()
{
  m_wells.clear();
}

TimeDepthTool::TdResult VelocityModel::twtForDepth( const QString &wellId, double depth,
                                                    bool useMd ) const
{
  const auto it = m_wells.constFind( wellId );
  if ( it == m_wells.constEnd() )
    return TimeDepthTool::TdResult(); // NoTable
  return TimeDepthTool::interpolateTimeMs( it.value(), depth, useMd );
}

TimeDepthTool::TdResult VelocityModel::depthForTwt( const QString &wellId, double timeMs ) const
{
  const auto it = m_wells.constFind( wellId );
  if ( it == m_wells.constEnd() )
    return TimeDepthTool::TdResult(); // NoTable

  // 反向契约（镜像 interpolateTimeMs）：文件顺序、time 列严格递增、
  // 可用样点 = time 与 TVD 都有限的行、范围外不外推。
  const QVector<TdRow> &rows = it.value().rows;
  int firstUsable = -1, lastUsable = -1;
  for ( int i = 0; i < rows.size(); ++i )
  {
    const TdRow &r = rows[i];
    if ( !std::isfinite( r.timeMs ) || !r.hasTvd || !std::isfinite( r.tvd ) )
      continue;
    if ( lastUsable >= 0 && !( r.timeMs > rows[lastUsable].timeMs ) )
      return { qQNaN(), TimeDepthTool::TdStatus::NonMonotonic };
    if ( firstUsable < 0 )
      firstUsable = i;
    lastUsable = i;
  }
  if ( firstUsable < 0 || lastUsable - firstUsable + 1 < 2 )
    return { qQNaN(), TimeDepthTool::TdStatus::NoTable };

  const double tFirst = rows[firstUsable].timeMs;
  const double tLast = rows[lastUsable].timeMs;
  if ( !( timeMs >= tFirst && timeMs <= tLast ) )
    return { qQNaN(), TimeDepthTool::TdStatus::OutOfRange };

  for ( int i = firstUsable; i < lastUsable; ++i )
  {
    const TdRow &r1 = rows[i];
    const TdRow &r2 = rows[i + 1];
    if ( !std::isfinite( r2.timeMs ) || !r2.hasTvd || !std::isfinite( r2.tvd ) )
      continue; // 不可用行不构成区间
    if ( timeMs >= r1.timeMs && timeMs <= r2.timeMs )
    {
      const double f = ( timeMs - r1.timeMs ) / ( r2.timeMs - r1.timeMs );
      return { r1.tvd + f * ( r2.tvd - r1.tvd ), TimeDepthTool::TdStatus::Ok };
    }
  }
  // tLast 采样点本身：上面区间循环在最后一段含端点，理论到不了这里。
  return { rows[lastUsable].tvd, TimeDepthTool::TdStatus::Ok };
}
