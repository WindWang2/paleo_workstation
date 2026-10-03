// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/buffertransition.h"
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

std::size_t cell( int col, int row, int cols )
{
  return static_cast<std::size_t>( row ) * cols + col;
}

BufferTransitionSpec spec( std::vector<Point2> points, double half, double shoulder, double floor )
{
  BufferTransitionSpec out;
  out.points = std::move( points );
  out.halfWidth = half;
  out.transitionWidth = shoulder;
  out.floor = floor;
  return out;
}

std::vector<BufferTransitionSpec> specs( std::initializer_list<BufferTransitionSpec> list )
{
  return std::vector<BufferTransitionSpec>( list );
}

} // namespace

class SingleFactorBufferTests : public QObject
{
  Q_OBJECT
  private slots:
    void transitionBandSmoothAndOutsideUntouched();
    void explicitFloorOnly();
    void overlapTakesLowerEnvelope();
    void invalidSpecRejected();
};

void SingleFactorBufferTests::transitionBandSmoothAndOutsideUntouched()
{
  // 竖线 x=5，全高水平场 10，核半宽 1、肩宽 2：核内 floor=2，1<d<3 过渡，带外不动。
  const GridSpec g = grid( 12, 1, 0, 1 );
  std::vector<double> source( 12, 10.0 );
  const BufferTransitionResult result =
      applyBufferTransition( g, source, specs( { spec( { { 5, -10 }, { 5, 10 } }, 1.0, 2.0, 2.0 ) } ), {} );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QCOMPARE( result.records.size(), std::size_t{ 1 } );
  QVERIFY( result.records[0].coreCells > 0 );
  QVERIFY( result.records[0].affectedCells >= result.records[0].coreCells );

  // 过渡带按 |距离| 单调回升（线两侧各有一段过渡带，逐像元走行不单调是正常的）。
  std::vector<std::pair<double, double>> band;
  for ( int col = 0; col < 12; ++col )
  {
    const double d = std::abs( col + 0.5 - 5.0 );
    const double value = result.values[cell( col, 0, 12 )];
    if ( d < 1.0 )
    {
      QCOMPARE( value, 2.0 ); // 核内常值
    }
    else if ( d <= 3.0 )
    {
      QVERIFY( value < 10.0 );
      band.emplace_back( d, value );
    }
    else
    {
      QCOMPARE( value, 10.0 ); // 带外不受影响
    }
  }
  std::sort( band.begin(), band.end() );
  for ( std::size_t k = 1; k < band.size(); ++k )
    QVERIFY( band[k].second > band[k - 1].second - 1e-12 );
  // 过渡带端点连续：blend(0)=floor、blend(1)=原值。
  QVERIFY( result.values[cell( 4, 0, 12 )] < 2.5 );
  QVERIFY( result.values[cell( 7, 0, 12 )] > 9.0 );
}

void SingleFactorBufferTests::explicitFloorOnly()
{
  const GridSpec g = grid( 4, 1, 0, 1 );
  std::vector<double> source( 4, 7.0 );
  // 墙在场外：任何像元不改——本模块绝不推断「断线=零厚度」。
  const BufferTransitionResult untouched =
      applyBufferTransition( g, source, specs( { spec( { { 10, -1 }, { 10, 1 } }, 1.0, 1.0, 0.0 ) } ), {} );
  QCOMPARE( untouched.values, source );
  QCOMPARE( untouched.records.front().affectedCells, 0 );

  // 显式 floor=5 作用在核内非零值 10 上：是压到 5，不是压到 0。
  const BufferTransitionResult floored =
      applyBufferTransition( g, source, specs( { spec( { { 0.5, -1 }, { 0.5, 1 } }, 1.0, 1.0, 5.0 ) } ), {} );
  QVERIFY( floored.values[cell( 0, 0, 4 )] < 5.5 );
  QVERIFY( floored.values[cell( 0, 0, 4 )] > 4.5 );
  QCOMPARE( floored.values[cell( 3, 0, 4 )], 7.0 );
}

void SingleFactorBufferTests::overlapTakesLowerEnvelope()
{
  const GridSpec g = grid( 10, 1, 0, 1 );
  std::vector<double> source( 10, 10.0 );
  // 两条重叠缓冲：x=2 floor=6；x=6 floor=3。重叠候选独立计算取较低包络，
  // 线序不影响结果。
  const BufferTransitionResult a = applyBufferTransition(
      g, source,
      specs( { spec( { { 2, -10 }, { 2, 10 } }, 1.0, 2.0, 6.0 ),
              spec( { { 6, -10 }, { 6, 10 } }, 1.0, 2.0, 3.0 ) } ),
      {} );
  const BufferTransitionResult b = applyBufferTransition(
      g, source,
      specs( { spec( { { 6, -10 }, { 6, 10 } }, 1.0, 2.0, 3.0 ),
              spec( { { 2, -10 }, { 2, 10 } }, 1.0, 2.0, 6.0 ) } ),
      {} );
  QCOMPARE( a.values, b.values );
  // 各自核内取各自 floor。
  QVERIFY( std::abs( a.values[cell( 6, 0, 10 )] - 3.0 ) <= 1e-9 );
  QVERIFY( std::abs( a.values[cell( 2, 0, 10 )] - 6.0 ) <= 1e-9 );
}

void SingleFactorBufferTests::invalidSpecRejected()
{
  const GridSpec g = grid( 4, 1, 0, 1 );
  std::vector<double> source( 4, 1.0 );
  BufferTransitionResult bad =
      applyBufferTransition( g, source, specs( { spec( { { 0, 0 }, { 1, 1 } }, -1.0, 1.0, 0.0 ) } ), {} );
  QCOMPARE( static_cast<int>( bad.status ), static_cast<int>( Status::InvalidInput ) );
  QVERIFY( !bad.message.empty() );
  bad = applyBufferTransition( g, source, specs( { spec( { { 0, 0 }, { 1, 1 } }, 1.0, 0.0, 0.0 ) } ), {} );
  QCOMPARE( static_cast<int>( bad.status ), static_cast<int>( Status::InvalidInput ) );
  // 网格尺寸不符。
  bad = applyBufferTransition( g, { 1.0, 2.0 }, {}, {} );
  QCOMPARE( static_cast<int>( bad.status ), static_cast<int>( Status::InvalidInput ) );
}

QTEST_MAIN( SingleFactorBufferTests )
#include "tst_singlefactor_buffer.moc"
