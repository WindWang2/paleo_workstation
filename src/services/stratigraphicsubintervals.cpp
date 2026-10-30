// 层：数据
#include "stratigraphicsubintervals.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fw = paleo::data::framework;
using paleo::data::StratigraphicScheme;

namespace paleo::strat
{

StratigraphicScheme draftScheme( const QString &schemeId, const QString &name, const QStringList &orderedBoundaries,
                                 const WellTopTable &tops, const QString &depthType )
{
  StratigraphicScheme s;
  s.id = schemeId.toStdString();
  s.name = name.toStdString();
  int order = 0;
  for ( const QString &b : orderedBoundaries )
  {
    paleo::data::StratigraphicBoundary boundary;
    boundary.name = b.toStdString();
    boundary.order = ++order;
    boundary.kind = "horizon";
    boundary.source = "project_boundaries";
    s.boundaries.push_back( boundary );
  }
  fw::deriveUnits( s );
  QList<qlonglong> wells = tops.keys();
  std::sort( wells.begin(), wells.end() );
  for ( std::size_t i = 0; i < s.intervals.size(); ++i )
  {
    const auto &iv = s.intervals[i];
    const QString top = QString::fromStdString( fw::findBoundary( s, iv.top )->name );
    const QString bottom = QString::fromStdString( fw::findBoundary( s, iv.bottom )->name );
    for ( qlonglong w : wells )
    {
      const auto &t = tops.value( w );
      if ( !t.contains( top ) && !t.contains( bottom ) )
        continue; // 该井与此父单元无任何证据，不写出现记录
      paleo::data::WellIntervalOccurrence o;
      o.wellId = w;
      o.interval = iv.id;
      o.depthType = depthType.toStdString();
      o.depthUnit = "m";
      o.top = t.value( top, std::numeric_limits<double>::quiet_NaN() );
      o.bottom = t.value( bottom, std::numeric_limits<double>::quiet_NaN() );
      if ( !t.contains( top ) )
        o.status = "top_missing";
      else if ( !t.contains( bottom ) )
        o.status = "not_penetrated";
      else if ( o.top == o.bottom )
        o.status = "pinched_out";
      s.wellOccurrences.push_back( o );
    }
  }
  return s;
}

QVector<SelectorParent> selectorOptions( const StratigraphicScheme &scheme )
{
  QVector<SelectorParent> out;
  for ( const auto &iv : scheme.intervals )
  {
    SelectorParent p;
    p.intervalId = QString::fromStdString( iv.id.value );
    p.label = QString::fromStdString( fw::displayName( scheme, iv.id ) );
    for ( const auto *sub : fw::subIntervalsOf( scheme, iv.id ) )
    {
      p.subIntervalIds << QString::fromStdString( sub->id.value );
      p.subLabels << QStringLiteral( "S%1" ).arg( sub->index );
    }
    out.push_back( p );
  }
  return out;
}

UnitSelection makeSelection( const StratigraphicScheme &scheme, const QString &intervalId, int subIndex )
{
  UnitSelection sel;
  const paleo::data::IntervalId iid{ intervalId.toStdString() };
  if ( !fw::findInterval( scheme, iid ) || subIndex < 0 || subIndex > kSubIntervalCount )
    return sel;
  sel.schemeId = QString::fromStdString( scheme.id );
  sel.revision = QString::fromStdString( scheme.revision.value );
  sel.intervalId = intervalId;
  if ( subIndex == 0 )
  {
    sel.aggregate = true;
    sel.displayName = QString::fromStdString( fw::displayName( scheme, iid ) ) + QStringLiteral( "（综合）" );
    return sel;
  }
  const auto subs = fw::subIntervalsOf( scheme, iid );
  sel.subIntervalId = QString::fromStdString( subs.at( subIndex - 1 )->id.value );
  sel.displayName = QString::fromStdString( fw::displayName( scheme, subs.at( subIndex - 1 )->id ) );
  return sel;
}

QVector<char> wellSampleMask( const StratigraphicScheme &scheme, qlonglong wellId, const QString &subIntervalId,
                              const QVector<double> &depths, QString *reason )
{
  QVector<char> mask( depths.size(), 0 );
  const paleo::data::SubIntervalId sid{ subIntervalId.toStdString() };
  const auto range = fw::wellRange( scheme, wellId, sid );
  if ( !range.resolved )
  {
    if ( reason )
      *reason = QString::fromStdString( range.reason.empty() ? std::string( "unresolved" ) : range.reason );
    return mask;
  }
  const auto *sub = fw::findSubInterval( scheme, sid );
  const bool last = sub && sub->index == kSubIntervalCount;
  for ( int i = 0; i < depths.size(); ++i )
  {
    const double d = depths.at( i );
    mask[i] = ( d >= range.top && ( d < range.bottom || ( last && d == range.bottom ) ) ) ? 1 : 0;
  }
  return mask;
}

double wellSubIntervalMean( const StratigraphicScheme &scheme, qlonglong wellId, const QString &subIntervalId,
                            const QVector<double> &depths, const QVector<double> &values, int minSamples )
{
  const auto mask = wellSampleMask( scheme, wellId, subIntervalId, depths );
  double sum = 0;
  int n = 0;
  for ( int i = 0; i < mask.size() && i < values.size(); ++i )
    if ( mask[i] && std::isfinite( values[i] ) )
    {
      sum += values[i];
      ++n;
    }
  return n >= std::max( 1, minSamples ) ? sum / n : std::numeric_limits<double>::quiet_NaN();
}

double aggregateInterval( const StratigraphicScheme &scheme, qlonglong wellId, const QString &intervalId,
                          const QHash<QString, double> &perSubValue, QStringList *missing )
{
  const paleo::data::IntervalId iid{ intervalId.toStdString() };
  double weighted = 0, total = 0;
  bool ok = fw::findInterval( scheme, iid ) != nullptr;
  for ( const auto *sub : fw::subIntervalsOf( scheme, iid ) )
  {
    const QString id = QString::fromStdString( sub->id.value );
    const auto r = fw::wellRange( scheme, wellId, sub->id );
    const double v = perSubValue.value( id, std::numeric_limits<double>::quiet_NaN() );
    if ( !r.resolved || !std::isfinite( v ) )
    {
      ok = false;
      if ( missing )
        *missing << QStringLiteral( "S%1" ).arg( sub->index );
      continue;
    }
    weighted += v * ( r.bottom - r.top );
    total += r.bottom - r.top;
  }
  return ( ok && total > 0 ) ? weighted / total : std::numeric_limits<double>::quiet_NaN();
}

} // namespace paleo::strat
