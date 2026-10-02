// 层：数据（测试壳位于 tests/，被测对象为局部方向核与冻结参考夹具）
#include <QtTest/QtTest>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "algorithms/singlefactor/localidw.h"

#include <cmath>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

QString fixturePath()
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) ).dir().filePath(
      QStringLiteral( "fixtures/singlefactor/kernel_parity.json" ) );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/singlefactor/kernel_parity.json" );
}

Sample wellAt( int index, double x, double y, double value )
{
  Sample sample;
  sample.stableRowId = std::to_string( index );
  sample.wellId = sample.stableRowId;
  sample.x = x;
  sample.y = y;
  sample.value = value;
  return sample;
}

} // namespace

class SingleFactorParityTests : public QObject
{
  Q_OBJECT
private slots:
  void matchesFrozenReference();
};

void SingleFactorParityTests::matchesFrozenReference()
{
  QFile file( fixturePath() );
  QVERIFY2( file.open( QIODevice::ReadOnly ), qPrintable( file.fileName() ) );
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson( file.readAll(), &error );
  QCOMPARE( error.error, QJsonParseError::NoError );
  const QJsonObject root = document.object();
  QCOMPARE( root.value( QStringLiteral( "referenceRevision" ) ).toString(),
            QStringLiteral( "27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f" ) );
  const QJsonArray cases = root.value( QStringLiteral( "cases" ) ).toArray();
  QVERIFY( cases.size() >= 13 );

  for ( const QJsonValue &item : cases )
  {
    const QJsonObject spec = item.toObject();
    const QString name = spec.value( QStringLiteral( "name" ) ).toString();
    PreparedInput input;
    const QJsonArray wells = spec.value( QStringLiteral( "wells" ) ).toArray();
    for ( int i = 0; i < wells.size(); ++i )
    {
      const QJsonArray row = wells.at( i ).toArray();
      input.samples.push_back( wellAt( i, row.at( 0 ).toDouble(), row.at( 1 ).toDouble(), row.at( 2 ).toDouble() ) );
    }
    input.originalCount = static_cast<int>( input.samples.size() );
    input.validCount = input.originalCount;
    std::vector<Point2> queries;
    for ( const QJsonValue &query : spec.value( QStringLiteral( "queries" ) ).toArray() )
    {
      const QJsonArray row = query.toArray();
      queries.push_back( Point2{ row.at( 0 ).toDouble(), row.at( 1 ).toDouble() } );
    }
    ResolvedParameters params;
    params.autosApplied = true;
    params.power = spec.value( QStringLiteral( "power" ) ).toDouble();
    params.minPoints = spec.value( QStringLiteral( "min_points" ) ).toInt();
    params.maxPoints = spec.value( QStringLiteral( "max_points" ) ).toInt();
    params.clusterSpan = spec.value( QStringLiteral( "cluster_span" ) ).toDouble();
    params.coverage = CoverageMode::DomainExtrapolation;
    if ( !spec.value( QStringLiteral( "search_radius" ) ).isNull() )
      params.searchRadius = spec.value( QStringLiteral( "search_radius" ) ).toDouble();
    for ( const QJsonValue &directionValue : spec.value( QStringLiteral( "directions" ) ).toArray() )
    {
      const QJsonObject direction = directionValue.toObject();
      ResolvedDirection resolved;
      resolved.id = direction.value( QStringLiteral( "id" ) ).toString().toStdString();
      resolved.ratio = direction.value( QStringLiteral( "ratio" ) ).toDouble();
      resolved.influence = direction.value( QStringLiteral( "influence_radius" ) ).toDouble();
      resolved.core = direction.value( QStringLiteral( "core_radius" ) ).toDouble();
      for ( const QJsonValue &point : direction.value( QStringLiteral( "points" ) ).toArray() )
      {
        const QJsonArray row = point.toArray();
        resolved.points.push_back( Point2{ row.at( 0 ).toDouble(), row.at( 1 ).toDouble() } );
      }
      params.directions.push_back( std::move( resolved ) );
    }
    for ( const QJsonValue &softValue : spec.value( QStringLiteral( "soft" ) ).toArray() )
    {
      const QJsonObject soft = softValue.toObject();
      ResolvedSoft resolved;
      resolved.radius = soft.value( QStringLiteral( "radius" ) ).toDouble();
      resolved.strength = soft.value( QStringLiteral( "strength" ) ).toDouble();
      for ( const QJsonValue &point : soft.value( QStringLiteral( "points" ) ).toArray() )
      {
        const QJsonArray row = point.toArray();
        resolved.points.push_back( Point2{ row.at( 0 ).toDouble(), row.at( 1 ).toDouble() } );
      }
      params.soft.push_back( std::move( resolved ) );
    }

    const QueryResult result = evaluateAt( input, queries, params, {} );
    QVERIFY2( result.status == Status::Ok, qPrintable( name + QStringLiteral( ": " ) +
                                                        QString::fromStdString( result.message ) ) );
    const QJsonArray expected = spec.value( QStringLiteral( "values" ) ).toArray();
    const QJsonArray finite = spec.value( QStringLiteral( "finite" ) ).toArray();
    QCOMPARE( result.values.size(), static_cast<std::size_t>( expected.size() ) );
    double scale = 1;
    for ( const QJsonValue &value : expected )
    {
      if ( !value.isNull() )
        scale = std::max( scale, std::abs( value.toDouble() ) );
    }
    double maxAbs = 0;
    double sumSq = 0;
    int counted = 0;
    for ( int i = 0; i < expected.size(); ++i )
    {
      const bool expectFinite = finite.at( i ).toBool();
      const bool gotFinite = std::isfinite( result.values[static_cast<std::size_t>( i )] );
      QVERIFY2( expectFinite == gotFinite, qPrintable( name ) );
      if ( !expectFinite )
        continue;
      const double error = std::abs( result.values[static_cast<std::size_t>( i )] - expected.at( i ).toDouble() );
      maxAbs = std::max( maxAbs, error );
      sumSq += error * error;
      ++counted;
    }
    const double rmse = counted > 0 ? std::sqrt( sumSq / counted ) : 0;
    QVERIFY2( maxAbs <= 1e-9 * scale, qPrintable( QStringLiteral( "%1 maxAbs=%2 scale=%3" ).arg( name ).arg( maxAbs, 0, 'e', 3 ).arg( scale ) ) );
    QVERIFY2( rmse <= 1e-10 * scale, qPrintable( QStringLiteral( "%1 rmse=%2 scale=%3" ).arg( name ).arg( rmse, 0, 'e', 3 ).arg( scale ) ) );
  }
}

QTEST_MAIN( SingleFactorParityTests )
#include "tst_singlefactor_parity.moc"
