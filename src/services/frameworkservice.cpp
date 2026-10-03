// 层：数据
#include "frameworkservice.h"

#include <QFile>
#include <QSet>

#include "../domain/mappinghorizons.h"
#include "../io/wellfileparsers.h"

namespace SequenceFramework
{

FrameworkService::FrameworkService( const QString &projectDir, QObject *parent )
  : QObject( parent ), m_projectDir( projectDir )
{
}

void FrameworkService::attachCatalog( DataCatalog *catalog )
{
  m_catalog = catalog;
}

bool FrameworkService::reload( QString *error )
{
  if ( m_catalog == nullptr )
  {
    if ( error )
      *error = QStringLiteral( "catalog 未接线" );
    return false;
  }
  FrameworkStore store( m_catalog, m_projectDir );
  Framework fw;
  if ( !store.load( &fw, error ) )
    return false;
  m_fw = fw;
  emit frameworkChanged();
  if ( error )
    error->clear();
  return true;
}

bool FrameworkService::save( const Framework &fw, QString *error )
{
  if ( m_catalog == nullptr )
  {
    if ( error )
      *error = QStringLiteral( "catalog 未接线" );
    return false;
  }
  FrameworkStore store( m_catalog, m_projectDir );
  if ( !store.save( fw, error ) )
  {
    emit catalogWriteRejected( error ? *error : QString() );
    return false;
  }
  m_fw = fw;
  emit frameworkChanged();
  if ( error )
    error->clear();
  return true;
}

QVector<WellTopRecord> FrameworkService::loadWellTops( QString *error ) const
{
  QVector<WellTopRecord> out;
  if ( m_catalog == nullptr )
  {
    if ( error )
      *error = QStringLiteral( "catalog 未接线" );
    return out;
  }
  for ( const CatalogAsset &a : m_catalog->assets() )
  {
    if ( a.type != QLatin1String( "well_stratification" ) )
      continue;
    const CatalogVersion ver = m_catalog->currentVersion( a.id );
    if ( ver.id.isEmpty() )
      continue;
    const QString abs = DataCatalog::resolvedVersionPath( m_projectDir, ver );
    if ( abs.isEmpty() )
      continue;
    QFile f( abs );
    if ( !f.open( QIODevice::ReadOnly ) )
      continue; // 单个资产读不出不致命——其余井照常参与诊断
    out.append( parseWellTopsText( f.readAll() ) );
  }
  if ( error )
    error->clear();
  return out;
}

QVector<SuggestionCandidate> FrameworkService::suggest( const QVector<WellTopRecord> &tops,
                                                        const SuggestOptions &opt ) const
{
  // 纯读：不碰 catalog、不落盘（建议只出候选，人工确认后才写库）。
  const QVector<WellLayerObservation> obs = buildObservations( tops );
  return suggestUnitAssignments( m_fw, obs, opt );
}

bool FrameworkService::commitAccepted( const QVector<SuggestionCandidate> &candidates,
                                       QString *error )
{
  const QVector<SuggestionCandidate> accepted = acceptedCandidates( candidates );
  if ( accepted.isEmpty() )
  {
    if ( error )
      error->clear();
    return true; // 没有确认项 → 不落盘，不空涨 revision
  }
  Framework fw = m_fw;
  const int changed = applyAccepted( fw, accepted );
  if ( changed <= 0 )
  {
    if ( error )
      error->clear();
    return true; // 全部已存在（幂等）→ 同样不落盘
  }
  return save( fw, error );
}

QStringList FrameworkService::mappedHorizons() const
{
  QStringList out;
  if ( m_catalog == nullptr )
    return out;
  for ( const CatalogEntity &e : m_catalog->entities( QStringLiteral( "sequence_boundary" ) ) )
  {
    // 未决层位（名单外、extra["pending"]）不算有编图数据。
    if ( e.extra.value( QStringLiteral( "pending" ) ).toBool() )
      continue;
    if ( e.name.isEmpty() )
      continue;
    out.append( e.name );
  }
  out.sort();
  return out;
}

QStringList FrameworkService::layerNamesOf( const QVector<WellTopRecord> &tops )
{
  QSet<QString> set;
  for ( const WellTopRecord &t : tops )
    if ( !t.topName.isEmpty() )
      set.insert( t.topName );
  QStringList out;
  for ( const QString &name : set )
    out.append( name );
  out.sort();
  return out;
}

DiagnosticInput FrameworkService::buildDiagnosticInput( const QVector<WellTopRecord> &tops ) const
{
  DiagnosticInput in;
  in.framework = m_fw;
  in.horizons = mappingHorizons();       // 层位序唯一权威（浅→深）
  in.mappedHorizons = mappedHorizons();
  in.tops = tops;
  in.knownLayerNames = layerNamesOf( tops );
  return in;
}

DiagnosticReport FrameworkService::diagnose( const QVector<WellTopRecord> &tops ) const
{
  return SequenceFramework::diagnose( buildDiagnosticInput( tops ) );
}

} // namespace SequenceFramework
