// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/localidw.h"
#include "algorithms/singlefactor/partition.h"
#include "algorithms/singlefactor/regionalcontours.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

GridSpec grid( int cols, int rows, double originX, double originY )
{
  GridSpec out;
  out.cols = cols;
  out.rows = rows;
  out.originX = originX;
  out.originY = originY;
  out.pixelWidth = 1;
  out.pixelHeight = -1;
  return out;
}

Polygon rect( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

Sample well( const char *id, double x, double y, double value )
{
  Sample sample;
  sample.stableRowId = id;
  sample.wellId = id;
  sample.x = x;
  sample.y = y;
  sample.value = value;
  return sample;
}

PreparedInput inputWithWells( std::vector<Sample> samples )
{
  PreparedInput input;
  input.samples = std::move( samples );
  input.domain = { rect( 0, 0, 10, 10 ) };
  input.validCount = static_cast<int>( input.samples.size() );
  input.originalCount = input.validCount;
  return input;
}

ResolvedParameters params()
{
  ResolvedParameters out;
  out.autosApplied = true;
  out.power = 2;
  out.coverage = CoverageMode::DomainExtrapolation;
  out.minPoints = 1;
  out.maxPoints = 0;
  out.supportedMinPoints = 1;
  out.supportedRadius = 1e9;
  out.tolerance = 1e-9;
  return out;
}

// 像元所属分区：x<5 → 0，x>=5 → 1。
std::vector<int> twoRegionLabels( const GridSpec &g )
{
  std::vector<int> labels( static_cast<std::size_t>( g.cols ) * g.rows, 0 );
  for ( int row = 0; row < g.rows; ++row )
    for ( int col = 0; col < g.cols; ++col )
      labels[static_cast<std::size_t>( row ) * g.cols + col] =
          cellCenter( g, col, row ).x < 5.0 ? 0 : 1;
  return labels;
}

int regionAt( const GridSpec &g, Point2 p, const std::vector<int> &labels )
{
  const int col = static_cast<int>( std::floor( ( p.x - g.originX ) / g.pixelWidth ) );
  const int row = static_cast<int>( std::floor( ( p.y - g.originY ) / g.pixelHeight ) );
  if ( col < 0 || col >= g.cols || row < 0 || row >= g.rows )
    return -1;
  return labels[static_cast<std::size_t>( row ) * g.cols + col];
}

} // namespace

class SingleFactorRegionalTests : public QObject
{
  Q_OBJECT
  private slots:
    void regionsYieldIndependentContours();
    void emptyRegionRefusesBorrowing();
    void mismatchedInputsRejected();
};

void SingleFactorRegionalTests::regionsYieldIndependentContours()
{
  const GridSpec g = grid( 10, 10, 0, 10 );
  PartitionResult partition;
  partition.regionIds = twoRegionLabels( g );
  partition.regionCount = 2;
  partition.mode = "interpretation";
  // 左区两口井（低值），右区两口井（高值）——各区井控互相独立。
  PreparedInput input = inputWithWells( { well( "a", 1, 5, 0.0 ), well( "b", 4, 4.4, 1.0 ),
                                          well( "c", 6, 5, 8.0 ), well( "d", 9, 5, 9.0 ) } );
  partition.wellRegionIds = { 0, 0, 1, 1 };

  const RegionalContourResult result =
      regionalContours( input, g, params(), { 0.4, 8.4 }, partition );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QVERIFY( !result.contours.empty() );
  QCOMPARE( result.contours.size(), result.regionOfContour.size() );

  bool sawLow = false;
  bool sawHigh = false;
  for ( std::size_t i = 0; i < result.contours.size(); ++i )
  {
    const ContourPolyline &poly = result.contours[i];
    QCOMPARE( poly.points.size() >= 2, true );
    // 每条线整体落在单一分区内，不跨区。
    int region = regionAt( g, poly.points.front(), partition.regionIds );
    QVERIFY( region >= 0 );
    QCOMPARE( result.regionOfContour[i], region );
    for ( const Point2 &p : poly.points )
      QCOMPARE( regionAt( g, p, partition.regionIds ), region );
    if ( poly.level == 0.4 && region == 0 )
      sawLow = true;
    if ( poly.level == 8.4 && region == 1 )
      sawHigh = true;
  }
  // 低级别线只在左区出现，高级别线只在右区出现。
  QVERIFY( sawLow );
  QVERIFY( sawHigh );
  for ( std::size_t i = 0; i < result.contours.size(); ++i )
  {
    if ( result.contours[i].level == 0.4 )
      QCOMPARE( result.regionOfContour[i], 0 );
    if ( result.contours[i].level == 8.4 )
      QCOMPARE( result.regionOfContour[i], 1 );
  }
}

void SingleFactorRegionalTests::emptyRegionRefusesBorrowing()
{
  const GridSpec g = grid( 10, 10, 0, 10 );
  PartitionResult partition;
  partition.regionIds = twoRegionLabels( g );
  partition.regionCount = 2;
  partition.mode = "interpretation";
  // 右区无井：拒绝，不跨隔断外借左区数值。
  PreparedInput input = inputWithWells( { well( "a", 1, 5, 0.0 ), well( "b", 4, 5, 1.0 ) } );
  partition.wellRegionIds = { 0, 0 };
  const RegionalContourResult result =
      regionalContours( input, g, params(), { 0.4 }, partition );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::InvalidInput ) );
  QVERIFY( result.message.find( "不能跨隔断借用" ) != std::string::npos );
}

void SingleFactorRegionalTests::mismatchedInputsRejected()
{
  const GridSpec g = grid( 10, 10, 0, 10 );
  PreparedInput input = inputWithWells( { well( "a", 1, 5, 0.0 ) } );
  PartitionResult partition;
  partition.regionIds = twoRegionLabels( g );
  partition.regionCount = 2;
  RegionalContourResult result = regionalContours( input, g, params(), { 0.4 }, partition );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::InvalidInput ) );
  QVERIFY( !result.message.empty() );

  partition.wellRegionIds = { 0, 0 };
  result = regionalContours( input, g, params(), {}, partition );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::InvalidInput ) );

  partition.wellRegionIds = { 0 };
  result = regionalContours( input, g, params(), { 0.4, std::numeric_limits<double>::quiet_NaN() },
                             partition );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::InvalidInput ) );
}

QTEST_MAIN( SingleFactorRegionalTests )
#include "tst_singlefactor_regional.moc"
