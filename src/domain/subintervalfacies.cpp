// 层：数据
#include "subintervalfacies.h"

#include <QHash>
#include <QtNumeric>
#include <algorithm>
#include <cmath>

namespace paleo::strat
{

SandRatioClass classifySandRatio( double ratio, const SandRatioBins &bins )
{
  SandRatioClass out;
  if ( !std::isfinite( ratio ) || ratio < 0.0 || ratio > 1.0 + 1e-9 )
    return out;
  const double r = std::min( 1.0, std::max( 0.0, ratio ) );
  if ( r <= bins.mudMax )
  {
    out.code = 1;
    out.id = QStringLiteral( "mud" );
    out.labelZh = QStringLiteral( "泥质" );
  }
  else if ( r <= bins.muddySandMax )
  {
    out.code = 2;
    out.id = QStringLiteral( "muddy_sand" );
    out.labelZh = QStringLiteral( "泥质砂" );
  }
  else if ( r <= bins.sandMax )
  {
    out.code = 3;
    out.id = QStringLiteral( "sand" );
    out.labelZh = QStringLiteral( "砂岩" );
  }
  else
  {
    out.code = 4;
    out.id = QStringLiteral( "clean_sand" );
    out.labelZh = QStringLiteral( "纯砂" );
  }
  return out;
}

SandRatioMetrics makeSandRatioMetrics( double topMd, double bottomMd, double sandThicknessMd,
                                       const SandRatioBins &bins )
{
  SandRatioMetrics m;
  m.topMd = topMd;
  m.bottomMd = bottomMd;
  if ( !std::isfinite( topMd ) || !std::isfinite( bottomMd ) || bottomMd <= topMd )
  {
    m.reason = QStringLiteral( "子层段顶底无效或倒置" );
    return m;
  }
  m.thicknessMd = bottomMd - topMd;
  if ( !std::isfinite( sandThicknessMd ) || sandThicknessMd < 0 )
  {
    m.reason = QStringLiteral( "砂厚缺失或为负" );
    return m;
  }
  if ( sandThicknessMd > m.thicknessMd + 1e-6 )
  {
    m.reason = QStringLiteral( "砂厚大于层厚（砂地比超出 [0,1]）" );
    return m;
  }
  m.sandThicknessMd = sandThicknessMd;
  m.sandRatio = m.thicknessMd > 0 ? sandThicknessMd / m.thicknessMd : 0;
  m.faciesClass = classifySandRatio( m.sandRatio, bins );
  if ( m.faciesClass.code <= 0 )
  {
    m.reason = QStringLiteral( "砂地比无法分类" );
    return m;
  }
  m.ok = true;
  return m;
}

bool provisionalSubRange( double parentTopMd, double parentBottomMd, int subIndex1to4, double *outTop,
                          double *outBottom )
{
  if ( !outTop || !outBottom )
    return false;
  if ( !( subIndex1to4 >= 1 && subIndex1to4 <= 4 ) )
    return false;
  if ( !std::isfinite( parentTopMd ) || !std::isfinite( parentBottomMd ) || parentBottomMd <= parentTopMd )
    return false;
  const double h = parentBottomMd - parentTopMd;
  *outTop = parentTopMd + h * ( subIndex1to4 - 1 ) / 4.0;
  *outBottom = parentTopMd + h * subIndex1to4 / 4.0;
  return true;
}

QString seismicFaciesScopeId( SeismicFaciesScope scope )
{
  return scope == SeismicFaciesScope::Slice ? QStringLiteral( "slice" )
                                            : QStringLiteral( "interval" );
}

QString seismicFaciesScopeLabelZh( SeismicFaciesScope scope )
{
  return scope == SeismicFaciesScope::Slice ? QStringLiteral( "时间切片预测" )
                                            : QStringLiteral( "层段综合结果" );
}

SeismicIntervalAggregate aggregateSeismicSliceVotes( const QVector<SeismicSliceVote> &votes,
                                                     double minCoverage )
{
  SeismicIntervalAggregate out;
  if ( !( minCoverage > 0.0 && minCoverage <= 1.0 ) )
  {
    out.reason = QStringLiteral( "覆盖率门控无效" );
    return out;
  }
  double total = 0;
  double valid = 0;
  QHash<int, double> weightByCode;
  for ( const auto &v : votes )
  {
    if ( !( v.weight > 0 ) )
      continue;
    total += v.weight;
    if ( v.classCode <= 0 )
      continue;
    valid += v.weight;
    weightByCode[v.classCode] += v.weight;
  }
  if ( total <= 0 )
  {
    out.reason = QStringLiteral( "无切片投票权重" );
    return out;
  }
  out.coverage = valid / total;
  if ( out.coverage + 1e-12 < minCoverage )
  {
    out.reason = QStringLiteral( "层段有效覆盖不足（%1 < %2）" )
                     .arg( out.coverage, 0, 'f', 3 )
                     .arg( minCoverage, 0, 'f', 3 );
    return out;
  }
  int best = 0;
  double bestW = -1;
  for ( auto it = weightByCode.cbegin(); it != weightByCode.cend(); ++it )
  {
    if ( it.value() > bestW || ( std::abs( it.value() - bestW ) <= 1e-15 && it.key() < best ) )
    {
      bestW = it.value();
      best = it.key();
    }
  }
  if ( best <= 0 )
  {
    out.reason = QStringLiteral( "无有效相码" );
    return out;
  }
  out.classCode = best;
  out.ok = true;
  return out;
}

QVariantMap evidenceRowForBinding( const QString &kind, const QString &schemeId, const QString &revision,
                                   const QString &intervalId, const QString &subIntervalId, bool aggregate,
                                   qlonglong wellId, const QString &assetRef, const QString &status,
                                   const QVariantMap &detail )
{
  QVariantMap row;
  row.insert( QStringLiteral( "kind" ), kind );
  row.insert( QStringLiteral( "schemeId" ), schemeId );
  row.insert( QStringLiteral( "schemeRevision" ), revision );
  row.insert( QStringLiteral( "intervalId" ), intervalId );
  if ( !aggregate )
    row.insert( QStringLiteral( "subIntervalId" ), subIntervalId );
  row.insert( QStringLiteral( "aggregate" ), aggregate );
  row.insert( QStringLiteral( "wellId" ), wellId );
  row.insert( QStringLiteral( "assetRef" ), assetRef );
  row.insert( QStringLiteral( "status" ), status );
  // 证据联动身份：稳定 ID，禁止 horizon 字符串匹配。
  row.insert( QStringLiteral( "source_horizon" ),
              aggregate ? ( QStringLiteral( "agg-" ) + intervalId ) : subIntervalId );
  if ( !detail.isEmpty() )
    row.insert( QStringLiteral( "detail" ), detail );
  return row;
}

} // namespace paleo::strat
