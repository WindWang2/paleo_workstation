// 层：数据（测试壳位于 tests/，被测对象为方向23 完成内核与冻结参考夹具）
#include <QtTest/QtTest>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "algorithms/singlefactor/corridor.h"
#include "algorithms/singlefactor/faultpath.h"
#include "algorithms/singlefactor/partition.h"

#include <cmath>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

QString fixturePath()
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) ).dir().filePath(
      QStringLiteral( "fixtures/singlefactor/kernel_parity_completion.json" ) );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/singlefactor/kernel_parity_completion.json" );
}

std::vector<Point2> pointsOf( const QJsonArray &rows )
{
  std::vector<Point2> points;
  for ( const QJsonValue &value : rows )
  {
    const QJsonArray row = value.toArray();
    points.push_back( Point2{ row.at( 0 ).toDouble(), row.at( 1 ).toDouble() } );
  }
  return points;
}

GridSpec centerGrid( int cols, int rows )
{
  // 北置网格（originY 顶、pixelHeight<0）；像元中心与夹具的升序 y 网格
  // 经行翻转 (r_up = rows-1-r_north) 对齐。
  GridSpec grid;
  grid.cols = cols;
  grid.rows = rows;
  grid.originX = 0;
  grid.originY = rows;
  grid.pixelWidth = 1;
  grid.pixelHeight = -1;
  return grid;
}

} // namespace

class SingleFactorCompletionParityTests : public QObject
{
  Q_OBJECT
private slots:
  void matchesFrozenCompletionReference();
};

void SingleFactorCompletionParityTests::matchesFrozenCompletionReference()
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
  QCOMPARE( cases.size(), 3 );

  for ( const QJsonValue &item : cases )
  {
    const QJsonObject spec = item.toObject();
    const QString kind = spec.value( QStringLiteral( "kind" ) ).toString();
    const QString name = spec.value( QStringLiteral( "name" ) ).toString();

    if ( kind == QLatin1String( "corridor_projection" ) )
    {
      const QJsonObject lineSpec = spec.value( QStringLiteral( "line" ) ).toObject();
      DirectionLineSpec dspec;
      dspec.lineId = "bend";
      dspec.points = pointsOf( lineSpec.value( QStringLiteral( "points" ) ).toArray() );
      dspec.ratio = lineSpec.value( QStringLiteral( "ratio" ) ).toDouble();
      dspec.influenceRadius = lineSpec.value( QStringLiteral( "influence_radius" ) ).toDouble();
      dspec.coreRadius = lineSpec.value( QStringLiteral( "core_radius" ) ).toDouble();
      dspec.extendMode = lineSpec.value( QStringLiteral( "extend_mode" ) ).toString().toStdString();
      const PolylineGeometry geom = buildPolylineGeometry( dspec, 0, 0.0 );
      const std::vector<Point2> queries = pointsOf( spec.value( QStringLiteral( "queries" ) ).toArray() );
      const QJsonArray expected = spec.value( QStringLiteral( "expected" ) ).toArray();
      QCOMPARE( queries.size(), static_cast<std::size_t>( expected.size() ) );
      for ( int i = 0; i < expected.size(); ++i )
      {
        const QJsonObject want = expected.at( i ).toObject();
        const PolylineProjection got = projectPointToPolyline( queries[static_cast<std::size_t>( i )], geom );
        const double g = combinedInfluence( got.distance, got.s, geom );
        const double scale = std::max(
            { 1.0, std::abs( want.value( QStringLiteral( "s" ) ).toDouble() ),
              std::abs( want.value( QStringLiteral( "n" ) ).toDouble() ),
              std::abs( want.value( QStringLiteral( "distance" ) ).toDouble() ), g } );
        QVERIFY2( std::abs( got.s - want.value( QStringLiteral( "s" ) ).toDouble() ) <= 1e-9 * scale,
                  qPrintable( QStringLiteral( "%1 q%2 s %3 vs %4" )
                                  .arg( name )
                                  .arg( i )
                                  .arg( got.s )
                                  .arg( want.value( QStringLiteral( "s" ) ).toDouble() ) ) );
        QVERIFY2( std::abs( got.n - want.value( QStringLiteral( "n" ) ).toDouble() ) <= 1e-9 * scale,
                  qPrintable( QStringLiteral( "%1 q%2 n" ).arg( name ).arg( i ) ) );
        QVERIFY2( std::abs( got.tx - want.value( QStringLiteral( "tx" ) ).toDouble() ) <= 1e-9, qPrintable( name ) );
        QVERIFY2( std::abs( got.ty - want.value( QStringLiteral( "ty" ) ).toDouble() ) <= 1e-9, qPrintable( name ) );
        QVERIFY2( std::abs( got.distance - want.value( QStringLiteral( "distance" ) ).toDouble() ) <=
                      1e-9 * scale,
                  qPrintable( QStringLiteral( "%1 q%2 d %3 vs %4" )
                                  .arg( name )
                                  .arg( i )
                                  .arg( got.distance )
                                  .arg( want.value( QStringLiteral( "distance" ) ).toDouble() ) ) );
        QVERIFY2( std::abs( g - want.value( QStringLiteral( "g" ) ).toDouble() ) <= 1e-9,
                  qPrintable( QStringLiteral( "%1 q%2 g %3 vs %4" )
                                  .arg( name )
                                  .arg( i )
                                  .arg( g )
                                  .arg( want.value( QStringLiteral( "g" ) ).toDouble() ) ) );
      }
      continue;
    }

    if ( kind == QLatin1String( "faultpath_distance" ) )
    {
      const std::vector<Point2> wells = pointsOf( spec.value( QStringLiteral( "wells" ) ).toArray() );
      FaultLine wall;
      wall.points = pointsOf( spec.value( QStringLiteral( "barrier" ) ).toArray() );
      const std::vector<FaultLine> barriers{ wall };
      const std::vector<Point2> queries = pointsOf( spec.value( QStringLiteral( "queries" ) ).toArray() );
      const QJsonArray distances = spec.value( QStringLiteral( "distances" ) ).toArray();
      const FaultPathMetric metric( wells, barriers );
      const std::vector<double> got = metric.distances( queries );
      QCOMPARE( got.size(), queries.size() * wells.size() );
      double scale = 1;
      double maxAbs = 0;
      for ( const QJsonValue &rowValue : distances )
      {
        const QJsonArray row = rowValue.toArray();
        for ( const QJsonValue &value : row )
          scale = std::max( scale, std::abs( value.toDouble() ) );
      }
      int finitePairs = 0;
      for ( int q = 0; q < distances.size(); ++q )
      {
        const QJsonArray row = distances.at( q ).toArray();
        for ( int w = 0; w < row.size(); ++w )
        {
          const double expected = row.at( w ).toDouble();
          const double actual = got[static_cast<std::size_t>( q ) * wells.size() + static_cast<std::size_t>( w )];
          // 参考侧 inf（隔离）与有限值：掩码必须先一致。
          QVERIFY2( std::isfinite( expected ) == std::isfinite( actual ),
                    qPrintable( QStringLiteral( "%1 q%2 w%3 finiteness" ).arg( name ).arg( q ).arg( w ) ) );
          if ( !std::isfinite( expected ) )
            continue;
          maxAbs = std::max( maxAbs, std::abs( actual - expected ) );
          ++finitePairs;
        }
      }
      QVERIFY2( finitePairs > 0, qPrintable( name ) );
      QVERIFY2( maxAbs <= 1e-9 * scale,
                qPrintable( QStringLiteral( "%1 maxAbs=%2 scale=%3（解析绕行节点 vs shapely "
                                            "缓冲外环，实测 4.2e-13）" )
                                .arg( name )
                                .arg( maxAbs, 0, 'e', 3 )
                                .arg( scale ) ) );
      continue;
    }

    if ( kind == QLatin1String( "partition_labels" ) )
    {
      const int cols = spec.value( QStringLiteral( "cols" ) ).toInt();
      const int rows = spec.value( QStringLiteral( "rows" ) ).toInt();
      const GridSpec grid = centerGrid( cols, rows );
      const std::vector<Point2> wells = pointsOf( spec.value( QStringLiteral( "wells" ) ).toArray() );
      BarrierSpec wall;
      wall.lineId = "w";
      wall.points = pointsOf( spec.value( QStringLiteral( "barrier" ) ).toArray() );
      const std::vector<BarrierSpec> barriers{ wall };
      Polygon boundary;
      boundary.exterior.points = pointsOf( spec.value( QStringLiteral( "boundary" ) ).toArray() );
      const std::vector<Polygon> boundaries{ boundary };
      // 夹具探针 (col, r_up)：升序 y 网格行号；本侧北置网格行号 rows-1-r_up。
      std::vector<std::pair<int, int>> probes;
      for ( const QJsonValue &value : spec.value( QStringLiteral( "probes" ) ).toArray() )
      {
        const QJsonArray row = value.toArray();
        probes.emplace_back( row.at( 0 ).toInt(), rows - 1 - row.at( 1 ).toInt() );
      }

      const QJsonObject resultSpec = spec.value( QStringLiteral( "result" ) ).toObject();
      for ( const char *mode : { "local", "interpretation" } )
      {
        const bool interpretation = qstrcmp( mode, "interpretation" ) == 0;
        const QJsonObject want = resultSpec.value( QLatin1String( mode ) ).toObject();
        const PartitionResult part =
            buildPartition( grid, std::vector<std::uint8_t>( static_cast<std::size_t>( cols ) * rows,
                                                             std::uint8_t{ 1 } ),
                            barriers, wells, boundaries, interpretation );
        QCOMPARE( part.regionCount, want.value( QStringLiteral( "region_count" ) ).toInt() );
        QCOMPARE( static_cast<int>( part.extensions.size() ),
                  want.value( QStringLiteral( "extension_count" ) ).toInt() );
        // 探针格同区矩阵（标签 id 的置换不变量）。
        const QJsonArray probeSame = want.value( QStringLiteral( "probe_same_region" ) ).toArray();
        QCOMPARE( probes.size(), static_cast<std::size_t>( probeSame.size() ) );
        for ( std::size_t i = 0; i < probes.size(); ++i )
        {
          const QJsonArray row = probeSame.at( static_cast<int>( i ) ).toArray();
          const int li = part.regionIds[static_cast<std::size_t>( probes[i].second ) * cols +
                                        probes[i].first];
          for ( std::size_t j = 0; j < probes.size(); ++j )
          {
            const int lj = part.regionIds[static_cast<std::size_t>( probes[j].second ) * cols +
                                          probes[j].first];
            const bool same = li >= 0 && li == lj;
            QVERIFY2( same == row.at( static_cast<int>( j ) ).toBool(),
                      qPrintable( QStringLiteral( "%1 %2 probe %3/%4 same=%5 want=%6" )
                                      .arg( name )
                                      .arg( QLatin1String( mode ) )
                                      .arg( i )
                                      .arg( j )
                                      .arg( same )
                                      .arg( row.at( static_cast<int>( j ) ).toBool() ) ) );
          }
        }
        // 井同区矩阵。
        const QJsonArray wellSame = want.value( QStringLiteral( "well_same_region" ) ).toArray();
        QCOMPARE( wells.size(), static_cast<std::size_t>( wellSame.size() ) );
        for ( std::size_t i = 0; i < wells.size(); ++i )
        {
          const QJsonArray row = wellSame.at( static_cast<int>( i ) ).toArray();
          for ( std::size_t j = 0; j < wells.size(); ++j )
          {
            const bool same = part.wellRegionIds[i] >= 0 && part.wellRegionIds[i] == part.wellRegionIds[j];
            QVERIFY2( same == row.at( static_cast<int>( j ) ).toBool(),
                      qPrintable( QStringLiteral( "%1 %2 well %3/%4" )
                                      .arg( name )
                                      .arg( QLatin1String( mode ) )
                                      .arg( i )
                                      .arg( j ) ) );
          }
        }
      }
      continue;
    }

    QVERIFY2( false, qPrintable( QStringLiteral( "未知夹具 kind：%1" ).arg( kind ) ) );
  }
}

QTEST_MAIN( SingleFactorCompletionParityTests )
#include "tst_singlefactor_parity_completion.moc"
