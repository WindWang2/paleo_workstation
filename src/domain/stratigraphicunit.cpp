// 层：数据
#include "stratigraphicunit.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QVariantMap>

namespace paleo::strat
{

QString cacheKey( const UnitSelection &selection, const QString &unitFingerprint, const QString &parameterSha256 )
{
  if ( !selection.isValid() )
    return {};
  const QString unit = selection.aggregate ? QStringLiteral( "agg:" ) + selection.intervalId : selection.subIntervalId;
  const QByteArray text = QStringList{ selection.schemeId, selection.revision, unit, unitFingerprint, parameterSha256 }
                            .join( QChar( 0x1f ) )
                            .toUtf8();
  return QString::fromLatin1( QCryptographicHash::hash( text, QCryptographicHash::Sha256 ).toHex() );
}

QString layerToken( const UnitSelection &selection )
{
  if ( !selection.isValid() )
    return {};
  return selection.aggregate ? QStringLiteral( "agg-" ) + selection.intervalId : selection.subIntervalId;
}

LabelClassification classifyLabel( const QString &label, const QStringList &orderedBoundaries )
{
  LabelClassification out;
  const QString text = label.trimmed();
  if ( text.isEmpty() )
    return out;
  if ( orderedBoundaries.contains( text ) )
  {
    out.kind = LabelKind::Boundary;
    out.top = text;
    return out;
  }
  static const QRegularExpression re( QStringLiteral( R"(^(.+?)\s*[-–—~]\s*(.+?)(?:\s*/\s*S([1-4]))?$)" ) );
  const auto m = re.match( text );
  if ( !m.hasMatch() )
    return out;
  const int a = orderedBoundaries.indexOf( m.captured( 1 ) );
  const int b = orderedBoundaries.indexOf( m.captured( 2 ) );
  if ( a < 0 || b < 0 )
    return out;
  out.top = m.captured( 1 );
  out.bottom = m.captured( 2 );
  if ( b != a + 1 )
  {
    out.kind = LabelKind::NonAdjacent;
    return out;
  }
  if ( !m.captured( 3 ).isEmpty() )
  {
    out.kind = LabelKind::SubInterval;
    out.subIndex = m.captured( 3 ).toInt();
  }
  else
    out.kind = LabelKind::Interval;
  return out;
}

bool requiresSubInterval( const QString &resultKind )
{
  static const QStringList kinds{ QStringLiteral( "well_facies" ), QStringLiteral( "seismic_facies" ),
                                  QStringLiteral( "single_factor" ), QStringLiteral( "map" ),
                                  QStringLiteral( "thickness" ), QStringLiteral( "well_tie" ) };
  return kinds.contains( resultKind );
}

QVector<QPair<QString, QString>> adjacentIntervals( const QStringList &orderedBoundaries )
{
  QVector<QPair<QString, QString>> out;
  for ( int i = 0; i + 1 < orderedBoundaries.size(); ++i )
    out.push_back( { orderedBoundaries.at( i ), orderedBoundaries.at( i + 1 ) } );
  return out;
}

QVariantMap toMap( const UnitSelection &selection )
{
  QVariantMap m;
  if ( !selection.isValid() )
    return m;
  m.insert( QStringLiteral( "schemeId" ), selection.schemeId );
  m.insert( QStringLiteral( "revision" ), selection.revision );
  m.insert( QStringLiteral( "intervalId" ), selection.intervalId );
  if ( !selection.aggregate )
    m.insert( QStringLiteral( "subIntervalId" ), selection.subIntervalId );
  m.insert( QStringLiteral( "aggregate" ), selection.aggregate );
  if ( !selection.displayName.isEmpty() )
    m.insert( QStringLiteral( "displayName" ), selection.displayName );
  if ( !selection.topBoundary.isEmpty() )
    m.insert( QStringLiteral( "topBoundary" ), selection.topBoundary );
  if ( !selection.bottomBoundary.isEmpty() )
    m.insert( QStringLiteral( "bottomBoundary" ), selection.bottomBoundary );
  return m;
}

UnitSelection fromMap( const QVariantMap &map )
{
  UnitSelection s;
  s.schemeId = map.value( QStringLiteral( "schemeId" ) ).toString();
  s.revision = map.value( QStringLiteral( "revision" ) ).toString();
  s.intervalId = map.value( QStringLiteral( "intervalId" ) ).toString();
  s.subIntervalId = map.value( QStringLiteral( "subIntervalId" ) ).toString();
  s.aggregate = map.value( QStringLiteral( "aggregate" ) ).toBool();
  s.displayName = map.value( QStringLiteral( "displayName" ) ).toString();
  s.topBoundary = map.value( QStringLiteral( "topBoundary" ) ).toString();
  s.bottomBoundary = map.value( QStringLiteral( "bottomBoundary" ) ).toString();
  if ( !s.isValid() )
    return {};
  return s;
}

QString analysisLayerKey( const UnitSelection &selection, const QString &legacyHorizon )
{
  if ( selection.isValid() )
  {
    const QString tok = layerToken( selection );
    if ( !tok.isEmpty() )
      return tok;
  }
  return legacyHorizon.trimmed();
}

} // namespace paleo::strat
