// 层：数据
#include "frameworkdiagnostics.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>

// 数据层的用户可见串（诊断标题/说明）同样走翻译机制：源语言=中文
//（DESIGN.md 2026-09-29 决策），用 QCoreApplication::translate 带显式上下文，
// 与 domain/welllogfacies.cpp 同款写法（本层不是 QObject，无 tr()）。
namespace
{

  QString sfText( const char *source )
  {
    return QCoreApplication::translate( "SequenceFramework", source );
  }

  int kindRank( SequenceFramework::DiagnosticKind kind )
  {
    return static_cast<int>( kind ); // 枚举序即报告序，稳定可复现
  }
} // namespace

namespace SequenceFramework
{

QString diagnosticCode( DiagnosticKind kind )
{
  switch ( kind )
  {
    case DiagnosticKind::MarkerDanglingLayer:
      return QStringLiteral( "SF-MARKER-DANGLING-LAYER" );
    case DiagnosticKind::MarkerDanglingUnit:
      return QStringLiteral( "SF-MARKER-DANGLING-UNIT" );
    case DiagnosticKind::BoundaryOutsideSet:
      return QStringLiteral( "SF-BOUNDARY-OUTSIDE-SET" );
    case DiagnosticKind::AmbiguousHorizonUnit:
      return QStringLiteral( "SF-AMBIGUOUS-HORIZON-UNIT" );
    case DiagnosticKind::HorizonWithoutUnit:
      return QStringLiteral( "SF-HORIZON-WITHOUT-UNIT" );
    case DiagnosticKind::UnitWithoutMappingData:
      return QStringLiteral( "SF-UNIT-WITHOUT-MAPPING-DATA" );
    case DiagnosticKind::FrameworkGap:
      return QStringLiteral( "SF-FRAMEWORK-GAP" );
    case DiagnosticKind::DuplicateLayerName:
      return QStringLiteral( "SF-DUPLICATE-LAYER-NAME" );
    case DiagnosticKind::OrphanWellLayer:
      return QStringLiteral( "SF-ORPHAN-WELL-LAYER" );
  }
  return QStringLiteral( "SF-UNKNOWN" );
}

QString diagnosticTitle( DiagnosticKind kind )
{
  switch ( kind )
  {
    case DiagnosticKind::MarkerDanglingLayer:
      return sfText( "标志层悬空：引用了不存在的井分层" );
    case DiagnosticKind::MarkerDanglingUnit:
      return sfText( "标志层悬空：指向了不存在的格架单元" );
    case DiagnosticKind::BoundaryOutsideSet:
      return sfText( "格架界面不在层位集合内" );
    case DiagnosticKind::AmbiguousHorizonUnit:
      return sfText( "层位归属歧义：一个层位被多个单元覆盖" );
    case DiagnosticKind::HorizonWithoutUnit:
      return sfText( "层位无格架单元归属" );
    case DiagnosticKind::UnitWithoutMappingData:
      return sfText( "格架单元无编图数据" );
    case DiagnosticKind::FrameworkGap:
      return sfText( "格架空洞：单元全工区无井覆盖" );
    case DiagnosticKind::DuplicateLayerName:
      return sfText( "跨单元重名层" );
    case DiagnosticKind::OrphanWellLayer:
      return sfText( "孤立井分层：无格架归属" );
  }
  return sfText( "未知诊断项" );
}

QString severityToken( Severity severity )
{
  switch ( severity )
  {
    case Severity::High:
      return QStringLiteral( "high" );
    case Severity::Medium:
      return QStringLiteral( "medium" );
    case Severity::Low:
      return QStringLiteral( "low" );
  }
  return QStringLiteral( "low" );
}

namespace
{

  Severity severityOf( DiagnosticKind kind )
  {
    switch ( kind )
    {
      case DiagnosticKind::MarkerDanglingLayer:
      case DiagnosticKind::MarkerDanglingUnit:
      case DiagnosticKind::BoundaryOutsideSet:
      case DiagnosticKind::AmbiguousHorizonUnit:
        return Severity::High;
      case DiagnosticKind::HorizonWithoutUnit:
      case DiagnosticKind::UnitWithoutMappingData:
      case DiagnosticKind::FrameworkGap:
        return Severity::Medium;
      case DiagnosticKind::DuplicateLayerName:
      case DiagnosticKind::OrphanWellLayer:
        return Severity::Low;
    }
    return Severity::Low;
  }

  void append( DiagnosticReport &report, DiagnosticKind kind, const QString &detail,
               const QStringList &locations )
  {
    Diagnostic d;
    d.kind = kind;
    d.severity = severityOf( kind );
    d.code = diagnosticCode( kind );
    d.title = diagnosticTitle( kind );
    d.detail = detail;
    d.locations = locations;
    report.items.append( d );
  }

  // 井分层名 → 出现该分层的井名列表（报告定位用）。
  QHash<QString, QStringList> wellsByLayerName( const QVector<WellTopRecord> &tops )
  {
    QHash<QString, QStringList> out;
    for ( const WellTopRecord &t : tops )
    {
      if ( t.topName.isEmpty() )
        continue;
      QStringList &wells = out[ t.topName ];
      if ( !wells.contains( t.wellName ) )
        wells.append( t.wellName );
    }
    for ( auto it = out.begin(); it != out.end(); ++it )
      it.value().sort();
    return out;
  }
} // namespace

DiagnosticReport diagnose( const DiagnosticInput &input )
{
  DiagnosticReport report;
  const Framework &fw = input.framework;
  const QStringList &horizons = input.horizons;

  // ---- 1. 单元界面是否在层位集合内 ----
  for ( const FrameworkUnit &u : fw.units )
  {
    QStringList outside;
    if ( !u.topBoundary.isEmpty() && !horizons.contains( u.topBoundary ) )
      outside.append( u.topBoundary );
    if ( !u.baseBoundary.isEmpty() && !horizons.contains( u.baseBoundary ) )
      outside.append( u.baseBoundary );
    if ( !outside.isEmpty() )
    {
      append( report, DiagnosticKind::BoundaryOutsideSet,
              sfText( "单元 %1 的界面 %2 不在层位集合内，覆盖区间无从定义" )
                  .arg( unitPath( fw, u.id ), outside.join( QStringLiteral( "、" ) ) ),
              QStringList{ unitPath( fw, u.id ) } + outside );
    }
  }

  // ---- 2. horizon ↔ 格架单元映射检查 ----
  const QHash<QString, QStringList> mapping = horizonUnitMap( fw, horizons );
  for ( const QString &h : horizons )
  {
    const QStringList owners = mapping.value( h );
    if ( owners.isEmpty() )
    {
      append( report, DiagnosticKind::HorizonWithoutUnit,
              sfText( "层位 %1 没有任何格架单元覆盖" ).arg( h ), QStringList{ h } );
      continue;
    }
    if ( owners.size() > 1 )
    {
      QStringList paths;
      for ( const QString &id : owners )
        paths.append( unitPath( fw, id ) );
      paths.sort();
      append( report, DiagnosticKind::AmbiguousHorizonUnit,
              sfText( "层位 %1 同时被 %2 覆盖（归属歧义）" )
                  .arg( h, paths.join( QStringLiteral( "、" ) ) ),
              QStringList{ h } + paths );
    }
  }

  // ---- 3. 格架单元是否有编图数据 ----
  const QSet<QString> mapped( input.mappedHorizons.begin(), input.mappedHorizons.end() );
  for ( const FrameworkUnit &u : leavesOf( fw ) )
  {
    const QStringList covered = horizonsCoveredBy( fw, u.id, horizons );
    if ( covered.isEmpty() )
      continue; // 区间无从定义 → 已由 BoundaryOutsideSet 报告，不重复计
    bool hasData = false;
    for ( const QString &h : covered )
      if ( mapped.contains( h ) )
      {
        hasData = true;
        break;
      }
    if ( !hasData )
    {
      append( report, DiagnosticKind::UnitWithoutMappingData,
              sfText( "单元 %1 的层位区间（%2）内没有任何已入库的编图层位" )
                  .arg( unitPath( fw, u.id ), covered.join( QStringLiteral( "、" ) ) ),
              QStringList{ unitPath( fw, u.id ) } );
    }
  }

  // ---- 4. 标志层引用完整性 ----
  const QSet<QString> known( input.knownLayerNames.begin(), input.knownLayerNames.end() );
  QHash<QString, QStringList> layersByUnit; // 分层名 → 所属单元 id（重名检测）
  for ( const MarkerBed &m : fw.markers )
  {
    if ( unitById( fw, m.unitId ) == nullptr )
    {
      append( report, DiagnosticKind::MarkerDanglingUnit,
              sfText( "标志层 %1 指向不存在的格架单元 %2" )
                  .arg( m.name, m.unitId.isEmpty() ? sfText( "（空）" ) : m.unitId ),
              QStringList{ m.name, m.unitId.isEmpty() ? QStringLiteral( "（空）" ) : m.unitId } );
    }
    for ( const QString &layer : m.layerNames )
    {
      if ( layer.isEmpty() || known.contains( layer ) )
        continue;
      append( report, DiagnosticKind::MarkerDanglingLayer,
              sfText( "标志层 %1 引用了井分层里不存在的分层名 %2" ).arg( m.name, layer ),
              QStringList{ m.name, layer } );
    }
    for ( const QString &layer : m.layerNames )
    {
      if ( layer.isEmpty() )
        continue;
      QStringList &units = layersByUnit[ layer ];
      if ( !units.contains( m.unitId ) )
        units.append( m.unitId );
    }
  }

  // ---- 5. 跨单元重名层 ----
  QStringList dupLayers;
  for ( auto it = layersByUnit.constBegin(); it != layersByUnit.constEnd(); ++it )
    if ( it.value().size() > 1 )
      dupLayers.append( it.key() );
  dupLayers.sort();
  for ( const QString &layer : dupLayers )
  {
    QStringList paths;
    for ( const QString &id : layersByUnit.value( layer ) )
      paths.append( unitPath( fw, id ) );
    paths.sort();
    append( report, DiagnosticKind::DuplicateLayerName,
            sfText( "分层名 %1 同时归属 %2" ).arg( layer, paths.join( QStringLiteral( "、" ) ) ),
            QStringList{ layer } + paths );
  }

  // ---- 6. 格架空洞：单元覆盖层位在任何井分层里都没有落点 ----
  if ( !input.tops.isEmpty() )
  {
    QSet<QString> topped;
    for ( const WellTopRecord &t : input.tops )
      if ( !t.topName.isEmpty() )
        topped.insert( t.topName );
    for ( const FrameworkUnit &u : leavesOf( fw ) )
    {
      const QStringList covered = horizonsCoveredBy( fw, u.id, horizons );
      if ( covered.isEmpty() )
        continue;
      bool covered2 = false;
      for ( const QString &h : covered )
        if ( topped.contains( h ) )
        {
          covered2 = true;
          break;
        }
      if ( !covered2 )
      {
        append( report, DiagnosticKind::FrameworkGap,
                sfText( "单元 %1 的层位区间（%2）在全部 %3 条井分层里都没有落点" )
                    .arg( unitPath( fw, u.id ), covered.join( QStringLiteral( "、" ) ),
                          QString::number( input.tops.size() ) ),
                QStringList{ unitPath( fw, u.id ) } );
      }
    }
  }

  // ---- 7. 孤立井分层 ----
  {
    QSet<QString> coveredByUnits;
    for ( const FrameworkUnit &u : leavesOf( fw ) )
      for ( const QString &h : horizonsCoveredBy( fw, u.id, horizons ) )
        coveredByUnits.insert( h );
    QSet<QString> referenced;
    for ( const MarkerBed &m : fw.markers )
      for ( const QString &layer : m.layerNames )
        if ( !layer.isEmpty() )
          referenced.insert( layer );
    const QHash<QString, QStringList> wellsOf = wellsByLayerName( input.tops );
    QStringList orphan;
    for ( auto it = wellsOf.constBegin(); it != wellsOf.constEnd(); ++it )
      if ( !coveredByUnits.contains( it.key() ) && !referenced.contains( it.key() ) )
        orphan.append( it.key() );
    // knownLayerNames 里出现、井里没出现的名字不是「井分层孤立」，不在此计。
    orphan.sort();
    for ( const QString &layer : orphan )
    {
      const QStringList wells = wellsOf.value( layer );
      append( report, DiagnosticKind::OrphanWellLayer,
              sfText( "井分层 %1 既不落在任何格架单元内，也未被任何标志层引用（见于 %2）" )
                  .arg( layer, wells.join( QStringLiteral( "、" ) ) ),
              QStringList{ layer } + wells );
    }
  }

  // 报告序稳定：kind → 首定位 → 明细。
  std::stable_sort( report.items.begin(), report.items.end(),
                    []( const Diagnostic &a, const Diagnostic &b ) {
                      if ( kindRank( a.kind ) != kindRank( b.kind ) )
                        return kindRank( a.kind ) < kindRank( b.kind );
                      const QString ka = a.locations.isEmpty() ? QString() : a.locations.first();
                      const QString kb = b.locations.isEmpty() ? QString() : b.locations.first();
                      if ( ka != kb )
                        return ka < kb;
                      return a.detail < b.detail;
                    } );
  return report;
}

int DiagnosticReport::countOf( DiagnosticKind kind ) const
{
  int n = 0;
  for ( const Diagnostic &d : items )
    if ( d.kind == kind )
      ++n;
  return n;
}

int DiagnosticReport::countOf( Severity severity ) const
{
  int n = 0;
  for ( const Diagnostic &d : items )
    if ( d.severity == severity )
      ++n;
  return n;
}

QString DiagnosticReport::toText() const
{
  QStringList lines;
  lines.append( sfText( "层序格架一致性诊断：%1 项（high %2 / medium %3 / low %4）" )
                    .arg( QString::number( items.size() ),
                          QString::number( countOf( Severity::High ) ),
                          QString::number( countOf( Severity::Medium ) ),
                          QString::number( countOf( Severity::Low ) ) ) );
  for ( const Diagnostic &d : items )
    lines.append( QStringLiteral( "[%1] %2 — %3｜%4｜%5" )
                      .arg( severityToken( d.severity ), d.code, d.title, d.detail,
                            d.locations.join( QStringLiteral( ", " ) ) ) );
  return lines.join( QStringLiteral( "\n" ) );
}

QString DiagnosticReport::toMarkdown() const
{
  QStringList lines;
  lines.append( QStringLiteral( "# %1" ).arg( sfText( "层序格架一致性诊断报告" ) ) );
  lines.append( QString() );
  lines.append( QStringLiteral( "- %1：%2" ).arg( sfText( "诊断项数" ),
                                                  QString::number( items.size() ) ) );
  lines.append( QStringLiteral( "- %1：high %2 / medium %3 / low %4" )
                    .arg( sfText( "按严重度" ), QString::number( countOf( Severity::High ) ),
                          QString::number( countOf( Severity::Medium ) ),
                          QString::number( countOf( Severity::Low ) ) ) );
  lines.append( QString() );
  lines.append( QStringLiteral( "| %1 | %2 | %3 | %4 |" )
                    .arg( sfText( "严重度" ), sfText( "代码" ), sfText( "标题" ),
                          sfText( "定位" ) ) );
  lines.append( QStringLiteral( "|---|---|---|---|" ) );
  for ( const Diagnostic &d : items )
    lines.append( QStringLiteral( "| %1 | %2 | %3 | %4 |" )
                      .arg( severityToken( d.severity ), d.code, d.title,
                            d.locations.join( QStringLiteral( "、" ) ) ) );
  lines.append( QString() );
  lines.append( QStringLiteral( "## %1" ).arg( sfText( "明细" ) ) );
  for ( const Diagnostic &d : items )
    lines.append( QStringLiteral( "- **%1** %2：%3" ).arg( d.code, d.title, d.detail ) );
  return lines.join( QStringLiteral( "\n" ) );
}

} // namespace SequenceFramework
