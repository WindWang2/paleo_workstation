// 层：测试壳（集合逐像元统计核：均值/总体标准差/P10/P90 收敛与口径）
#include <QtTest>
#include <QElapsedTimer>

#include "../src/algorithms/ensemblestats.h"

#include <cmath>
#include <limits>

// 方向 47 Oracle 2：合成 N 成员（已知均值/方差场）→ 派生面逐像素收敛真值。
// 口径钉死：stddev 是总体口径（÷n）；分位数 = 升序线性插值 type-7。
// NaN 语义：n=0 → 全输出 NaN；n=1 → mean=值、stddev=0、分位=值。

using namespace paleo::ensemble;

namespace
{
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// 成员 k 的像元 i 值 = base(i) + offset(k)。offset 构造成均值/方差已知的
// 对称序列，解析真值随手可算。
std::vector<double> memberField( std::size_t cells, double baseSlope,
                                 const std::vector<double> &offsets, std::size_t k )
{
  std::vector<double> field( cells );
  for ( std::size_t i = 0; i < cells; ++i )
    field[i] = 100.0 + baseSlope * static_cast<double>( i % 97 ) + offsets[k];
  return field;
}
} // namespace

class EnsembleStatsTests : public QObject
{
  Q_OBJECT

  private slots:
    void syntheticTruthConverges();
    void quantilesMatchDefinition();
    void nanCellsAreHonest();
    void streamingMatchesWholeField();
    void bandedMatchesWholeField();
    void degenerateSets();
    void scalesLinearlyWithCells();
};

void EnsembleStatsTests::syntheticTruthConverges()
{
  const std::size_t cells = 511;
  const std::vector<double> offsets = { -3.0, -2.0, -1.0, 0.0, 0.0, 1.0, 2.0, 3.0 };
  const double offsetMean = 0.0;         // (-3-2-1+0+0+1+2+3)/8
  double sq = 0.0;
  for ( const double o : offsets )
    sq += ( o - offsetMean ) * ( o - offsetMean );
  const double offsetStd = std::sqrt( sq / offsets.size() ); // 总体口径

  std::vector<std::vector<double>> storage;
  std::vector<const double *> members;
  for ( std::size_t k = 0; k < offsets.size(); ++k )
  {
    storage.push_back( memberField( cells, 0.5, offsets, k ) );
    members.push_back( storage.back().data() );
  }
  const StatsResult r = compute( members, cells, StatsRequest{ true, true, true, true } );
  QCOMPARE( r.mean.size(), cells );
  QCOMPARE( r.stddev.size(), cells );
  QCOMPARE( r.validCount.size(), cells );
  for ( std::size_t i = 0; i < cells; ++i )
  {
    const double truth = 100.0 + 0.5 * static_cast<double>( i % 97 ) + offsetMean;
    QVERIFY2( std::fabs( r.mean[i] - truth ) < 1e-9,
              qPrintable( QStringLiteral( "mean[%1]=%2 truth %3" ).arg( i ).arg( r.mean[i] ).arg( truth ) ) );
    QVERIFY2( std::fabs( r.stddev[i] - offsetStd ) < 1e-9,
              qPrintable( QStringLiteral( "stddev[%1]=%2 truth %3" ).arg( i ).arg( r.stddev[i] ).arg( offsetStd ) ) );
    QCOMPARE( r.validCount[i], 8 );
    // 均值面就是逐像元成员均值——与直算一致（不再经第二通道）。
    double direct = 0.0;
    for ( const double *m : members )
      direct += m[i];
    QCOMPARE( r.mean[i], direct / members.size() );
  }
}

void EnsembleStatsTests::quantilesMatchDefinition()
{
  // 0..9 十分位：p10 = 0.9、p90 = 8.1（h = q·(n-1) 线性插值）。
  const std::vector<double> v = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  QCOMPARE( quantileOf( v.data(), v.size(), 0.10 ), 0.9 );
  QCOMPARE( quantileOf( v.data(), v.size(), 0.90 ), 8.1 );
  QCOMPARE( quantileOf( v.data(), v.size(), 0.50 ), 4.5 );
  // 单值集合：任意分位 = 该值。
  const double one = 7.25;
  QCOMPARE( quantileOf( &one, 1, 0.10 ), 7.25 );
  // 全 NaN → NaN；空 → NaN。
  const double nanv = kNaN;
  QVERIFY( std::isnan( quantileOf( &nanv, 1, 0.5 ) ) );
  QVERIFY( std::isnan( quantileOf( nullptr, 0, 0.5 ) ) );
}

void EnsembleStatsTests::nanCellsAreHonest()
{
  // 3 成员 × 4 像元：像元0 全在场；像元1 一个 NaN；像元2 只剩一个在场；
  // 像元3 全 NaN。
  std::vector<double> a = { 10, 20, 30, kNaN };
  std::vector<double> b = { 12, kNaN, kNaN, kNaN };
  std::vector<double> c = { 14, 24, kNaN, kNaN };
  const StatsResult r = compute( { a.data(), b.data(), c.data() }, 4,
                                 StatsRequest{ true, true, true, true } );
  QCOMPARE( r.validCount[0], 3 );
  QCOMPARE( r.validCount[1], 2 );
  QCOMPARE( r.validCount[2], 1 );
  QCOMPARE( r.validCount[3], 0 );
  QCOMPARE( r.mean[0], 12.0 );
  QCOMPARE( r.mean[1], 22.0 );     // NaN 成员不参与——(20+24)/2
  QCOMPARE( r.mean[2], 30.0 );
  QCOMPARE( r.stddev[2], 0.0 );    // 单成员零离散（如实，不外推）
  QVERIFY( std::isnan( r.mean[3] ) );
  QVERIFY( std::isnan( r.stddev[3] ) );
  QVERIFY( std::isnan( r.p10[3] ) );
  QVERIFY( std::isnan( r.p90[3] ) );
}

void EnsembleStatsTests::streamingMatchesWholeField()
{
  const std::size_t cells = 997;
  std::vector<std::vector<double>> storage;
  std::vector<const double *> members;
  for ( int k = 0; k < 6; ++k )
  {
    storage.push_back( memberField( cells, 0.25, { -2, -1, 0, 1, 2, 3 }, 0 ) );
    for ( auto &v : storage.back() )
      v += k; // 成员间差 1.0——方差已知
    members.push_back( storage.back().data() );
  }
  const StatsResult whole = compute( members, cells, StatsRequest{ true, true } );
  StreamingMoments stream( cells );
  for ( const double *m : members )
    stream.addField( m );
  QCOMPARE( stream.fieldsAdded(), 6 );
  const std::vector<double> sm = stream.mean();
  const std::vector<double> ss = stream.stddevPopulation();
  for ( std::size_t i = 0; i < cells; ++i )
  {
    QCOMPARE( sm[i], whole.mean[i] );
    QCOMPARE( ss[i], whole.stddev[i] );
  }
}

void EnsembleStatsTests::bandedMatchesWholeField()
{
  const int cols = 64, rows = 48;
  const std::size_t cells = static_cast<std::size_t>( cols ) * rows;
  const std::vector<double> offsets = { -2.0, -1.0, 0.0, 1.0, 2.0 };
  std::vector<std::vector<double>> storage;
  std::vector<const double *> members;
  for ( std::size_t k = 0; k < offsets.size(); ++k )
  {
    storage.push_back( memberField( cells, 1.0, offsets, k ) );
    members.push_back( storage.back().data() );
  }
  const StatsResult whole = compute( members, cells, StatsRequest{ true, true, true, true } );

  const auto readBand = [&members, cols]( std::size_t member, int row0, int nRows, double *out ) {
    const double *src = members.at( member ) + static_cast<std::size_t>( row0 ) * cols;
    std::copy( src, src + static_cast<std::size_t>( nRows ) * cols, out );
    return true;
  };
  // 带高强制为 7 行（多带路径）：maxBandValues = 5 成员 × 64 列 × 7 行。
  const auto banded = quantilesBanded( members.size(), cols, rows, { 0.10, 0.90 },
                                       5 * 64 * 7, readBand );
  QCOMPARE( banded.size(), std::size_t( 2 ) );
  for ( std::size_t i = 0; i < cells; ++i )
  {
    QCOMPARE( banded[0][i], whole.p10[i] );
    QCOMPARE( banded[1][i], whole.p90[i] );
  }

  // 取消诚实：第二带起取消 → 空表。
  int calls = 0;
  const auto cancelAfterFirstBand = [&calls] { return ++calls > 1; };
  QVERIFY( quantilesBanded( members.size(), cols, rows, { 0.5 },
                            5 * 64 * 7, readBand, cancelAfterFirstBand ).empty() );
  // readBand 失败 → 空表（坏成员不静默）。
  const auto failBand = []( std::size_t, int, int, double * ) { return false; };
  QVERIFY( quantilesBanded( members.size(), cols, rows, { 0.5 }, 5 * 64, failBand ).empty() );
}

void EnsembleStatsTests::degenerateSets()
{
  // 零成员：不 crash、不产假面——空结果。
  QVERIFY( compute( {}, 64, StatsRequest{ true, true, true, true } ).mean.empty() );
  QVERIFY( quantilesBanded( 0, 8, 8, { 0.5 }, 1024,
                            []( std::size_t, int, int, double * ) { return true; } ).empty() );

  // 单成员：mean=自身、stddev=0、分位=自身。
  std::vector<double> single = { 3.0, 5.0, kNaN };
  const StatsResult r = compute( { single.data() }, 3, StatsRequest{ true, true, true, true } );
  QCOMPARE( r.mean[0], 3.0 );
  QCOMPARE( r.stddev[0], 0.0 );
  QCOMPARE( r.p10[1], 5.0 );
  QCOMPARE( r.p90[1], 5.0 );
  QVERIFY( std::isnan( r.mean[2] ) );
}

void EnsembleStatsTests::scalesLinearlyWithCells()
{
  // 比率门（TEST-02：禁绝对毫秒墙钟）：4× 像元 → 耗时 ≤ 4×1.5。
  const auto run = []( std::size_t cells ) {
    const std::vector<double> offsets = { -2, -1, 0, 1, 2, 3, -3, 0.5 };
    std::vector<std::vector<double>> storage;
    std::vector<const double *> members;
    storage.reserve( offsets.size() );
    for ( std::size_t k = 0; k < offsets.size(); ++k )
    {
      storage.push_back( memberField( cells, 0.125, offsets, k ) );
      members.push_back( storage.back().data() );
    }
    return compute( members, cells, StatsRequest{ true, true, true, true } );
  };
  QElapsedTimer timer;
  timer.start();
  const StatsResult small = run( 64 * 1024 );
  const qint64 smallMs = timer.elapsed();
  QCOMPARE( small.validCount[0], 8 );
  timer.restart();
  const StatsResult big = run( 256 * 1024 );
  const qint64 bigMs = timer.elapsed();
  QCOMPARE( big.validCount[0], 8 );
  const double ratio =
      static_cast<double>( bigMs ) / static_cast<double>( std::max<qint64>( smallMs, 1 ) );
  qWarning( "%s",
            qPrintable( QStringLiteral( "BASELINE ensemble_stats_scale_ratio = %1" ).arg( ratio ) ) );
  QVERIFY2( ratio <= 4.0 * 1.5,
            qPrintable( QStringLiteral( "ensemble stats scale ratio %1 exceeds bound 6" ).arg( ratio ) ) );
}

QTEST_MAIN( EnsembleStatsTests )
#include "tst_ensemblestats.moc"
