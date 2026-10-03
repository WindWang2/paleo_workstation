// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/contouravoidance.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

ContourPolyline line( double level, std::vector<Point2> points )
{
  ContourPolyline poly;
  poly.level = level;
  poly.points = std::move( points );
  return poly;
}

double distToWall( Point2 p, const std::vector<Point2> &wall )
{
  return distanceToPolyline( p, wall );
}

bool samePoints( const std::vector<Point2> &a, const std::vector<Point2> &b )
{
  if ( a.size() != b.size() )
    return false;
  for ( std::size_t i = 0; i < a.size(); ++i )
  {
    if ( a[i].x != b[i].x || a[i].y != b[i].y )
      return false;
  }
  return true;
}

bool sameContours( const std::vector<ContourPolyline> &a, const std::vector<ContourPolyline> &b )
{
  if ( a.size() != b.size() )
    return false;
  for ( std::size_t i = 0; i < a.size(); ++i )
  {
    if ( a[i].level != b[i].level || !samePoints( a[i].points, b[i].points ) )
      return false;
  }
  return true;
}

} // namespace

class SingleFactorAvoidanceTests : public QObject
{
  Q_OBJECT
  private slots:
    void crossingContourRoutesAroundBand();
    void midWallCrossingTruncates();
    void touchingContourTruncatesAtBand();
    void farContoursUntouchedAndValuesKept();
    void degenerateInputsPassThrough();
};

void SingleFactorAvoidanceTests::crossingContourRoutesAroundBand()
{
  // 短墙尖端贴近穿越点（tip 在 y=0.5）：沿缓冲外环绕过尖端，整线保持一条。
  const std::vector<Point2> wall{ { 5, 0.5 }, { 5, 10 } };
  const double width = 1.0;
  const ContourAvoidResult result = avoidContourBuffers(
      { line( 2.0, { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 }, { 8, 0 }, { 10, 0 } } ) }, wall,
      width, 1.0 );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QVERIFY2( result.rerouted >= 1, qPrintable( QStringLiteral( "rerouted=%1 truncated=%2" )
                                                  .arg( result.rerouted ).arg( result.truncated ) ) );
  QCOMPARE( result.contours.size(), std::size_t{ 1 } ); // 绕行保住整线，不分裂
  QCOMPARE( result.contours.front().level, 2.0 );       // 改线不改值（级别原样）
  for ( const Point2 &p : result.contours.front().points )
  {
    QVERIFY2( distToWall( p, wall ) >= width * 0.95,
              qPrintable( QStringLiteral( "%1,%2 d=%3" ).arg( p.x ).arg( p.y ).arg( distToWall( p, wall ) ) ) );
  }
  // 绕行点数不少于原线（绕道必然更长的链）。
  QVERIFY( result.contours.front().points.size() >= 5 );
}

void SingleFactorAvoidanceTests::midWallCrossingTruncates()
{
  // 长墙（y ∈ [-10,10]）中段穿越：绕行弧超长（> max(width*6, 弦*3)），
  // 原线在缓冲处截断，两段都在带外。
  const std::vector<Point2> wall{ { 5, -10 }, { 5, 10 } };
  const double width = 1.0;
  const ContourAvoidResult result = avoidContourBuffers(
      { line( 2.0, { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 }, { 8, 0 }, { 10, 0 } } ) }, wall,
      width, 1.0 );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QCOMPARE( result.rerouted, 0 );
  QVERIFY( result.truncated >= 1 );
  QVERIFY( result.contours.size() >= 2 ); // 截断成带外两段
  for ( const ContourPolyline &poly : result.contours )
  {
    QCOMPARE( poly.level, 2.0 );
    for ( const Point2 &p : poly.points )
      QVERIFY( distToWall( p, wall ) >= width * 0.95 );
  }
}

void SingleFactorAvoidanceTests::touchingContourTruncatesAtBand()
{
  // 等值线在缓冲内终止（右端在带内）：截断为带外片段，不做无中生有的连线。
  const std::vector<Point2> wall{ { 5, -10 }, { 5, 10 } };
  const double width = 1.0;
  const ContourAvoidResult result = avoidContourBuffers(
      { line( 2.0, { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 5.5, 0 } } ) }, wall, width, 1.0 );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QVERIFY( result.truncated >= 1 );
  QVERIFY( !result.contours.empty() );
  for ( const ContourPolyline &poly : result.contours )
  {
    QCOMPARE( poly.level, 2.0 );
    for ( const Point2 &p : poly.points )
      QVERIFY( distToWall( p, wall ) >= width * 0.95 );
  }
  // 截断后保留的是带外前段：最右点接近缓冲边界 x=4。
  double maxX = -std::numeric_limits<double>::infinity();
  for ( const ContourPolyline &poly : result.contours )
    for ( const Point2 &p : poly.points )
      maxX = std::max( maxX, p.x );
  QVERIFY( maxX < 4.5 );
}

void SingleFactorAvoidanceTests::farContoursUntouchedAndValuesKept()
{
  const std::vector<Point2> wall{ { 5, -10 }, { 5, 10 } };
  // 两条不穿缓冲的线（一条停在墙前，一条完全在墙右侧带外）。
  const std::vector<ContourPolyline> far{
    line( 1.0, { { 0, 6 }, { 2, 6 }, { 4, 6 } } ),
    line( 3.0, { { 6, -6 }, { 7, -6 }, { 9, -6 } } ),
  };
  const ContourAvoidResult result = avoidContourBuffers( far, wall, 1.0, 1.0 );
  QCOMPARE( result.rerouted, 0 );
  QCOMPARE( result.truncated, 0 );
  QCOMPARE( result.contours.size(), std::size_t{ 2 } );
  QVERIFY( samePoints( result.contours[0].points, far[0].points ) );
  QVERIFY( samePoints( result.contours[1].points, far[1].points ) );
}

void SingleFactorAvoidanceTests::degenerateInputsPassThrough()
{
  const std::vector<ContourPolyline> contours{ line( 1.0, { { 0, 0 }, { 1, 1 } } ) };
  // 墙退化 / 宽度非正：原样返回。
  ContourAvoidResult result = avoidContourBuffers( contours, { { 0, 0 } }, 1.0, 1.0 );
  QVERIFY( sameContours( result.contours, contours ) );
  result = avoidContourBuffers( contours, { { 0, 0 }, { 0, 1 } }, 0.0, 1.0 );
  QVERIFY( sameContours( result.contours, contours ) );
}

QTEST_MAIN( SingleFactorAvoidanceTests )
#include "tst_singlefactor_avoidance.moc"
