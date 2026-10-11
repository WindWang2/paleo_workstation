// 层：数据
#include "faciesconsistency.h"
#include "faciescatalog.h"

#include <QCoreApplication>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace paleo::facies
{
namespace
{
QString tr( const char *text )
{
  return QCoreApplication::translate( "FaciesConsistency", text );
}
} // namespace

ConsistencyReport computeConsistency( const QVector<ConsistencySample> &samples,
                                      const QVariantList &schema )
{
  ConsistencyReport report;
  QSet<int> codes;
  for ( const QVariant &v : schema )
  {
    bool ok = false;
    const int c = v.toMap().value( QStringLiteral( "code" ) ).toInt( &ok );
    if ( ok )
      codes.insert( c );
  }
  for ( const ConsistencySample &s : samples )
  {
    ConsistencyReport::Row row;
    row.sample = s;
    if ( !s.wellCode )
      row.reason = tr( "测井相未预测" );
    else if ( !s.seismicCode )
      row.reason = tr( "井位不在地震相覆盖范围内" );
    else
    {
      row.compared = true;
      row.agree = *s.wellCode == *s.seismicCode;
      codes.insert( *s.wellCode );
      codes.insert( *s.seismicCode );
    }
    report.rows.append( row );
  }
  report.codes = QVector<int>( codes.cbegin(), codes.cend() );
  std::sort( report.codes.begin(), report.codes.end() );
  const int n = report.codes.size();
  report.confusion = QVector<QVector<int>>( n, QVector<int>( n, 0 ) );
  for ( const auto &row : std::as_const( report.rows ) )
  {
    if ( !row.compared )
      continue;
    const int i = static_cast<int>( report.codes.indexOf( *row.sample.wellCode ) );
    const int j = static_cast<int>( report.codes.indexOf( *row.sample.seismicCode ) );
    ++report.confusion[i][j];
    ++report.compared;
    if ( row.agree )
      ++report.agreed;
  }
  if ( report.compared > 0 )
  {
    const double total = report.compared;
    report.agreement = report.agreed / total;
    double expected = 0.0;
    for ( int k = 0; k < n; ++k )
    {
      double rowSum = 0.0, colSum = 0.0;
      for ( int m = 0; m < n; ++m )
      {
        rowSum += report.confusion[k][m];
        colSum += report.confusion[m][k];
      }
      expected += ( rowSum / total ) * ( colSum / total );
    }
    report.kappa = std::fabs( 1.0 - expected ) < 1e-12
                       ? 1.0
                       : ( report.agreement - expected ) / ( 1.0 - expected );
  }
  return report;
}

QVariantMap ConsistencyReport::toVariant( const QVariantList &schema ) const
{
  const auto nameOf = [&schema]( const std::optional<int> &code ) -> QVariant {
    if ( !code )
      return QVariant();
    return FaciesCatalog::find( schema, *code ).value( QStringLiteral( "name" ) );
  };
  QVariantList rowList;
  for ( const Row &r : rows )
  {
    rowList << QVariantMap{
        { QStringLiteral( "well_id" ), r.sample.wellId },
        { QStringLiteral( "well_name" ), r.sample.wellName },
        { QStringLiteral( "x" ), r.sample.x },
        { QStringLiteral( "y" ), r.sample.y },
        { QStringLiteral( "well_code" ), r.sample.wellCode ? QVariant( *r.sample.wellCode ) : QVariant() },
        { QStringLiteral( "well_facies" ), nameOf( r.sample.wellCode ) },
        { QStringLiteral( "seismic_code" ), r.sample.seismicCode ? QVariant( *r.sample.seismicCode ) : QVariant() },
        { QStringLiteral( "seismic_facies" ), nameOf( r.sample.seismicCode ) },
        { QStringLiteral( "compared" ), r.compared },
        { QStringLiteral( "agree" ), r.agree },
        { QStringLiteral( "reason" ), r.reason } };
  }
  QVariantList codeList, matrix;
  for ( const int c : codes )
    codeList << QVariantMap{ { QStringLiteral( "code" ), c },
                             { QStringLiteral( "name" ), nameOf( c ) },
                             { QStringLiteral( "color" ),
                               FaciesCatalog::find( schema, c ).value( QStringLiteral( "color" ) ) } };
  for ( const auto &line : confusion )
  {
    QVariantList cells;
    for ( const int v : line )
      cells << v;
    matrix << QVariant( cells );
  }
  return { { QStringLiteral( "schema" ), QStringLiteral( "paleo_facies_consistency_v1" ) },
           { QStringLiteral( "rows" ), rowList },
           { QStringLiteral( "codes" ), codeList },
           { QStringLiteral( "confusion" ), matrix },
           { QStringLiteral( "compared" ), compared },
           { QStringLiteral( "agreed" ), agreed },
           { QStringLiteral( "uncompared" ), static_cast<int>( rows.size() ) - compared },
           { QStringLiteral( "agreement" ), agreement },
           { QStringLiteral( "kappa" ), kappa } };
}
} // namespace paleo::facies
