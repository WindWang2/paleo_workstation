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
  const auto *iv = fw::findInterval( scheme, iid );
  if ( !iv || subIndex < 0 || subIndex > kSubIntervalCount )
    return sel;
  sel.schemeId = QString::fromStdString( scheme.id );
  sel.revision = QString::fromStdString( scheme.revision.value );
  sel.intervalId = intervalId;
  // 上下界面从方案边界表派生写入，禁止调用方拆分显示名。
  if ( const auto *top = fw::findBoundary( scheme, iv->top ) )
    sel.topBoundary = QString::fromStdString( top->name );
  if ( const auto *bot = fw::findBoundary( scheme, iv->bottom ) )
    sel.bottomBoundary = QString::fromStdString( bot->name );
  if ( subIndex == 0 )
  {
    sel.aggregate = true;
    sel.subIndex = 0;
    sel.displayName = QString::fromStdString( fw::displayName( scheme, iid ) ) + QStringLiteral( "（综合）" );
    return sel;
  }
  const auto subs = fw::subIntervalsOf( scheme, iid );
  if ( static_cast<int>( subs.size() ) < subIndex )
    return UnitSelection{}; // 方案未派生完整四子层段：不给出半成品选择
  sel.subIntervalId = QString::fromStdString( subs.at( subIndex - 1 )->id.value );
  sel.subIndex = subIndex;
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

QVector<SubIntervalOccurrenceSummary> summarizeSubIntervalOccurrences(
    const StratigraphicScheme &scheme, const QString &intervalId,
    const std::vector<paleo::data::SubIntervalResultBinding> *bindings )
{
  QVector<SubIntervalOccurrenceSummary> out;
  const paleo::data::IntervalId iid{ intervalId.toStdString() };
  const auto pickLight = []( const SubIntervalOccurrenceSummary &r ) -> QString {
    const int wells = r.wellsComplete + r.wellsProvisional + r.wellsAffected + r.wellsPending;
    if ( wells == 0 && r.bindingsCurrent == 0 && r.bindingsStale == 0 )
      return QStringLiteral( "gray" );
    if ( r.bindingsStale > 0 || ( wells > 0 && r.wellsAffected == wells ) )
      return QStringLiteral( "red" );
    if ( r.wellsProvisional > 0 || r.wellsPending > 0 || r.wellsAffected > 0 )
      return QStringLiteral( "yellow" );
    return QStringLiteral( "green" );
  };
  const auto lightTag = []( const QString &light ) -> QString {
    if ( light == QLatin1String( "green" ) )
      return QStringLiteral( "[绿]" );
    if ( light == QLatin1String( "yellow" ) )
      return QStringLiteral( "[黄]" );
    if ( light == QLatin1String( "red" ) )
      return QStringLiteral( "[红]" );
    return QStringLiteral( "[灰]" );
  };
  for ( const auto *sub : fw::subIntervalsOf( scheme, iid ) )
  {
    SubIntervalOccurrenceSummary row;
    row.subIntervalId = QString::fromStdString( sub->id.value );
    row.label = QStringLiteral( "S%1" ).arg( sub->index );
    // #862：每口有父单元记录的井按本子层段自己的顶底判定。
    for ( const auto &o : scheme.wellOccurrences )
    {
      if ( o.interval.value != iid.value )
        continue;
      if ( o.status == "pending" )
      {
        ++row.wellsPending;
        continue;
      }
      const auto r = fw::wellRange( scheme, o.wellId, sub->id );
      if ( !r.resolved )
        ++row.wellsAffected;
      else if ( r.provisional )
        ++row.wellsProvisional;
      else
        ++row.wellsComplete;
    }
    if ( bindings )
    {
      for ( const auto &b : *bindings )
      {
        if ( b.subInterval.value != sub->id.value )
          continue;
        if ( b.status == "current" || b.status == "candidate" )
          ++row.bindingsCurrent;
        else if ( b.status == "stale" || b.status == "review_required" )
          ++row.bindingsStale;
        else if ( b.status == "legacy_unresolved" )
          ++row.bindingsStale;
      }
    }
    row.light = pickLight( row );
    // 完整性 / 可用资料 / 受影响 —— 一行可读中文。
    const int wells =
      row.wellsComplete + row.wellsProvisional + row.wellsAffected + row.wellsPending;
    QString completeness;
    if ( wells == 0 )
      completeness = QStringLiteral( "无井出现记录" );
    else
      completeness = QStringLiteral( "本子层段井 已约束%1/按比例派生%2/受影响%3/待定%4" )
                         .arg( row.wellsComplete )
                         .arg( row.wellsProvisional )
                         .arg( row.wellsAffected )
                         .arg( row.wellsPending );
    const QString materials = row.bindingsCurrent > 0
                                  ? QStringLiteral( "可用资料 %1" ).arg( row.bindingsCurrent )
                                  : QStringLiteral( "暂无登记成果" );
    const QString impact = row.bindingsStale > 0
                               ? QStringLiteral( "受影响 %1" ).arg( row.bindingsStale )
                               : QStringLiteral( "无失效成果" );
    row.line = QStringLiteral( "%1 %2：%3；%4；%5 · ID %6" )
                   .arg( lightTag( row.light ), row.label, completeness, materials, impact, row.subIntervalId );
    out.push_back( row );
  }
  return out;
}


} // namespace paleo::strat
