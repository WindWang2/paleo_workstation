// 层：数据
#include "frameworksuggester.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace SequenceFramework
{

namespace
{

  double meanOf( const QVector<double> &v )
  {
    if ( v.isEmpty() )
      return 0.0;
    double acc = 0.0;
    for ( double x : v )
      acc += x;
    return acc / static_cast<double>( v.size() );
  }

  double sdOf( const QVector<double> &v, double mean )
  {
    if ( v.size() < 2 )
      return 0.0;
    double acc = 0.0;
    for ( double x : v )
    {
      const double d = x - mean;
      acc += d * d;
    }
    return std::sqrt( acc / static_cast<double>( v.size() - 1 ) );
  }

  // 归一化尺度：实测离散度 / 相对下限 / 1.0 三者取大——绝不除零，也不让
  // 全同样本把尺度压到 0 而把正常差异放大成超阈值。
  double scaleOf( double spread, double magnitude, double relativeFloor )
  {
    double s = spread;
    if ( s < std::fabs( magnitude ) * relativeFloor )
      s = std::fabs( magnitude ) * relativeFloor;
    return s > 1e-9 ? s : 1.0;
  }

  // 到闭区间 [top, base] 的距离（区间内 = 0）。
  double gapToInterval( double value, double top, double base )
  {
    if ( value < top )
      return top - value;
    if ( value > base )
      return value - base;
    return 0.0;
  }
} // namespace

QVector<WellLayerObservation> buildObservations( const QVector<WellTopRecord> &tops )
{
  QHash<QString, QVector<WellTopRecord>> byWell;
  for ( const WellTopRecord &t : tops )
  {
    if ( t.topName.isEmpty() || t.wellName.isEmpty() )
      continue;
    if ( !t.hasMd )
      continue; // 无深度的分层不参与建议（诚实跳过，不外推）
    byWell[ t.wellName ].append( t );
  }

  QVector<WellLayerObservation> out;
  QStringList wells;
  for ( auto it = byWell.constBegin(); it != byWell.constEnd(); ++it )
    wells.append( it.key() );
  wells.sort();
  for ( const QString &well : wells )
  {
    QVector<WellTopRecord> rows = byWell.value( well );
    std::stable_sort( rows.begin(), rows.end(),
                      []( const WellTopRecord &a, const WellTopRecord &b ) {
                        return a.md < b.md;
                      } );
    for ( int i = 0; i < rows.size(); ++i )
    {
      const WellTopRecord &row = rows.at( i );
      WellLayerObservation o;
      o.wellName = row.wellName;
      o.layerName = row.topName;
      o.topMd = row.md;
      o.hasTop = true;
      if ( i + 1 < rows.size() )
      {
        o.thickness = rows.at( i + 1 ).md - row.md;
        o.hasThickness = true;
      }
      out.append( o );
    }
  }
  return out;
}

QHash<QString, QString> confirmedOwners( const Framework &fw )
{
  QHash<QString, QString> owner;
  for ( const MarkerBed &m : fw.markers )
    for ( const QString &layer : m.layerNames )
      if ( !layer.isEmpty() && !owner.contains( layer ) )
        owner.insert( layer, m.unitId );
  return owner;
}

WellIntervalTable buildWellIntervals( const Framework &fw,
                                      const QVector<WellLayerObservation> &obs )
{
  const QHash<QString, QString> owner = confirmedOwners( fw );
  WellIntervalTable table;
  // 第一遍：累计每井每单元的最浅顶深 / 最深底深。
  for ( const WellLayerObservation &o : obs )
  {
    const QString unit = owner.value( o.layerName );
    if ( unit.isEmpty() || !o.hasTop )
      continue;
    const double bottom = o.hasThickness ? o.topMd + o.thickness : o.topMd;
    auto &byUnit = table[ o.wellName ];
    auto it = byUnit.find( unit );
    if ( it == byUnit.end() )
    {
      UnitInterval iv;
      iv.top = o.topMd;
      iv.base = bottom;
      iv.valid = true;
      byUnit.insert( unit, iv );
      continue;
    }
    if ( o.topMd < it.value().top )
      it.value().top = o.topMd;
    if ( bottom > it.value().base )
      it.value().base = bottom;
  }
  return table;
}

QVector<UnitSignature> buildSignatures( const Framework &fw,
                                        const QVector<WellLayerObservation> &obs )
{
  const QHash<QString, QString> owner = confirmedOwners( fw );
  const WellIntervalTable table = buildWellIntervals( fw, obs );

  // 每单元的逐井区间 → 取均值；成员厚度单独累计。
  QHash<QString, QVector<double>> topVals, baseVals, thickVals;
  for ( const WellLayerObservation &o : obs )
  {
    const QString unit = owner.value( o.layerName );
    if ( unit.isEmpty() )
      continue;
    if ( o.hasTop )
    {
      const UnitInterval iv = table.value( o.wellName ).value( unit );
      if ( iv.valid )
      {
        topVals[ unit ].append( iv.top );
        baseVals[ unit ].append( iv.base );
      }
    }
    if ( o.hasThickness )
      thickVals[ unit ].append( o.thickness );
  }

  QVector<UnitSignature> out;
  for ( const FrameworkUnit &u : fw.units )
  {
    UnitSignature sig;
    sig.unitId = u.id;
    const QVector<double> tv = topVals.value( u.id );
    const QVector<double> bv = baseVals.value( u.id );
    const QVector<double> hv = thickVals.value( u.id );
    sig.sampleCount = tv.size();
    if ( sig.sampleCount > 0 )
    {
      sig.topRef = meanOf( tv );
      sig.baseRef = meanOf( bv );
      sig.thicknessRef = meanOf( hv );
      sig.spanScale = scaleOf( std::fabs( sig.baseRef - sig.topRef ),
                               std::fabs( sig.baseRef - sig.topRef ), 0.5 );
      sig.thicknessScale = scaleOf( sdOf( hv, sig.thicknessRef ), sig.thicknessRef, 0.5 );
    }
    out.append( sig );
  }
  return out;
}

QVector<SuggestionCandidate> suggestUnitAssignments(
    const Framework &fw, const QVector<WellLayerObservation> &obs, const SuggestOptions &opt )
{
  QVector<SuggestionCandidate> out;
  if ( fw.units.isEmpty() )
    return out;

  const QHash<QString, QString> owner = confirmedOwners( fw );
  const WellIntervalTable table = buildWellIntervals( fw, obs );
  const QVector<UnitSignature> sigs = buildSignatures( fw, obs );

  // 已确认归属的分层名集合（skipAlreadyAssigned 时跳过整条观测）。
  QSet<QString> assigned;
  for ( auto it = owner.constBegin(); it != owner.constEnd(); ++it )
    assigned.insert( it.key() );

  for ( const WellLayerObservation &o : obs )
  {
    if ( !o.hasTop )
      continue;
    if ( opt.skipAlreadyAssigned && assigned.contains( o.layerName ) )
      continue;
    if ( opt.requireThickness && !o.hasThickness )
      continue;

    QString bestUnit;
    double bestDistance = 0.0, bestGap = 0.0, bestThick = 0.0;
    bool bestWellLocal = false;
    bool found = false;
    for ( const UnitSignature &sig : sigs )
    {
      if ( !sig.valid() )
        continue;
      UnitInterval iv = table.value( o.wellName ).value( sig.unitId );
      const bool wellLocal = iv.valid;
      if ( !wellLocal )
      {
        iv.top = sig.topRef;
        iv.base = sig.baseRef;
        iv.valid = true;
      }
      const double gap = gapToInterval( o.topMd, iv.top, iv.base );
      // 区间跨度尺度：本井区间优先用本井跨度，退回时用全工区均值跨度。
      const double span = scaleOf( std::fabs( iv.span() ), std::fabs( iv.span() ), 0.5 );
      double acc = opt.depthWeight * ( gap / span ) * ( gap / span );
      double wsum = opt.depthWeight;
      double thickDelta = 0.0;
      if ( o.hasThickness && sig.thicknessRef != 0.0 )
      {
        thickDelta = o.thickness - sig.thicknessRef;
        const double z = thickDelta / sig.thicknessScale;
        acc += opt.thicknessWeight * z * z;
        wsum += opt.thicknessWeight;
      }
      if ( wsum <= 0.0 )
        continue;
      const double distance = std::sqrt( acc / wsum );
      if ( !found || distance < bestDistance )
      {
        found = true;
        bestDistance = distance;
        bestUnit = sig.unitId;
        bestGap = gap;
        bestThick = thickDelta;
        bestWellLocal = wellLocal;
      }
    }
    if ( !found || bestUnit.isEmpty() )
      continue;
    if ( bestDistance > opt.maxDistance )
      continue; // 不够像就别猜（诚实失败，不出候选）

    SuggestionCandidate c;
    c.wellName = o.wellName;
    c.layerName = o.layerName;
    c.unitId = bestUnit;
    c.distance = bestDistance;
    c.depthGap = bestGap;
    c.thicknessDelta = bestThick;
    c.usedWellInterval = bestWellLocal;
    out.append( c );
  }

  // 稳定序：距离 → 层名 → 井名（同输入必同输出）。
  std::stable_sort( out.begin(), out.end(),
                    []( const SuggestionCandidate &a, const SuggestionCandidate &b ) {
                      if ( a.distance != b.distance )
                        return a.distance < b.distance;
                      if ( a.layerName != b.layerName )
                        return a.layerName < b.layerName;
                      return a.wellName < b.wellName;
                    } );
  return out;
}

QVector<SuggestionCandidate> acceptedCandidates( const QVector<SuggestionCandidate> &all )
{
  QVector<SuggestionCandidate> out;
  for ( const SuggestionCandidate &c : all )
    if ( c.accepted )
      out.append( c );
  return out;
}

int applyAccepted( Framework &fw, const QVector<SuggestionCandidate> &all )
{
  int changed = 0;
  for ( const SuggestionCandidate &c : all )
  {
    if ( !c.accepted || c.unitId.isEmpty() || c.layerName.isEmpty() )
      continue;
    if ( SequenceFramework::unitById( fw, c.unitId ) == nullptr )
      continue; // 悬空单元不落库（诊断面另报）
    MarkerBed *target = nullptr;
    for ( MarkerBed &m : fw.markers )
      if ( m.unitId == c.unitId && m.name == c.layerName )
      {
        target = &m;
        break;
      }
    if ( target == nullptr )
    {
      MarkerBed m;
      m.id = SequenceFramework::nextMarkerId( fw );
      m.name = c.layerName;
      m.unitId = c.unitId;
      m.layerNames.append( c.layerName );
      fw.markers.append( m );
      ++changed;
      continue;
    }
    if ( target->layerNames.contains( c.layerName ) )
      continue; // 已存在 → 幂等，不计入变更
    target->layerNames.append( c.layerName );
    ++changed;
  }
  return changed;
}

} // namespace SequenceFramework
