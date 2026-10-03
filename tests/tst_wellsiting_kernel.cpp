// 层：测试壳
#include <QtTest>

#include "algorithms/wellsiting/wellsiting.h"
#include "algorithms/singlefactor/support.h" // distanceToPolyline（避让断言用）

#include <cmath>
#include <limits>

// 方向34 井网辅助纯核测试（Oracle 1/3）：
//   · 空洞检测——合成规则井网 + 人工去井空洞，面积/位置/连通性断言；
//   · 方案评估——加一口候选井后指标改善方向与幅度（合成夹具可解析验证）；
//   · 候选生成——最大空洞圆心/规则网格/避让/边界内缩/上限；
//   · 预算与退化输入的如实回退。
// 性能纪律：不设绝对毫秒墙钟；大工区空洞检测用「吞吐比率门」
// （大格网吞吐 ≥ 参照小格网吞吐的一半——同机同进程内相对比较）。

using namespace paleo::wellsiting;
namespace sf = paleo::singlefactor;

namespace
{

constexpr double kInf = std::numeric_limits<double>::infinity();

// 夹具口径：4km 井距、井控半径 3500m——井间对角 2828m 被覆盖，只有
// 东北角（缺井侧）出单个空洞（面积 ≈2.33km²，最深点 (9950,9950)）。
SitingOptions holeOptions()
{
  SitingOptions options;
  options.controlRadius = 3500;
  return options;
}

// 合成工区：10km×10km 方形域。
Polygon squareDomain( double size = 10000 )
{
  Polygon domain;
  domain.exterior.points = { Point2{ 0, 0 }, Point2{ size, 0 }, Point2{ size, size },
                             Point2{ 0, size } };
  return domain;
}

// 规则井网：4km 网格 3×3，去掉 (9000,9000) 一口 → 东北角人工空洞。
std::vector<Point2> regularWellsMissingNE()
{
  std::vector<Point2> wells;
  for ( int cx = 1000; cx <= 9000; cx += 4000 )
    for ( int cy = 1000; cy <= 9000; cy += 4000 )
    {
      if ( cx == 9000 && cy == 9000 )
        continue; // 人工挖去的井位
      wells.push_back( Point2{ double( cx ), double( cy ) } );
    }
  return wells;
}

double bruteNearest( double x, double y, const std::vector<Point2> &wells )
{
  double best = kInf;
  for ( const Point2 &w : wells )
    best = std::min( best, std::hypot( w.x - x, w.y - y ) );
  return best;
}

} // namespace

class TestWellSitingKernel : public QObject
{
  Q_OBJECT
  private slots:
    void nearestNeighborStatsRegularGrid();
    void holeDetectionSyntheticHole();
    void holeBoundaryPolygon();
    void boundaryBudgetSkipsOversized();
    void emptyWellsAllHole();
    void budgetExceeded();
    void invalidInputs();
    void candidateDeepestAndGrid();
    void candidateAvoidance();
    void scenarioImprovementDirectionAndMagnitude();
    void contributionMatchesScenarioDelta();
    void largeFieldThroughputRatio();
};

void TestWellSitingKernel::nearestNeighborStatsRegularGrid()
{
  // 3×3 规则网（含 NE 井）：每口井最近邻 = 4km 网格边长。
  std::vector<Point2> wells;
  for ( int cx = 1000; cx <= 9000; cx += 4000 )
    for ( int cy = 1000; cy <= 9000; cy += 4000 )
      wells.push_back( Point2{ double( cx ), double( cy ) } );
  const NearestNeighborStats stats = nearestNeighborStats( wells );
  QVERIFY( stats.valid );
  QCOMPARE( stats.count, std::size_t( 9 ) );
  QCOMPARE( stats.min, 4000.0 );
  QCOMPARE( stats.max, 4000.0 );
  QCOMPARE( stats.mean, 4000.0 );
  QCOMPARE( stats.median, 4000.0 );
  QCOMPARE( stats.p90, 4000.0 );

  const NearestNeighborStats lone = nearestNeighborStats( { Point2{ 5, 5 } } );
  QVERIFY( !lone.valid );
  const NearestNeighborStats none = nearestNeighborStats( {} );
  QVERIFY( !none.valid );
}

void TestWellSitingKernel::holeDetectionSyntheticHole()
{
  const SitingOptions options = holeOptions();
  const CoverageReport report =
      diagnoseCoverage( { squareDomain() }, regularWellsMissingNE(), options );

  QCOMPARE( report.status, Status::Ok );
  QVERIFY( report.note.find( "欧氏" ) != std::string::npos ); // 口径注记如实随报告

  // 独立暴力参照：细一倍的格网直接算距最近井，空洞面积应在 5% 内一致。
  double bruteArea = 0;
  const double fine = 50;
  const double R = options.controlRadius;
  for ( double y = fine / 2; y < 10000; y += fine )
    for ( double x = fine / 2; x < 10000; x += fine )
      if ( bruteNearest( x, y, regularWellsMissingNE() ) > R )
        bruteArea += fine * fine;
  QVERIFY( bruteArea > 1e6 ); // 夹具自检：空洞确实存在（≈ 数 km²）
  QVERIFY( report.holeAreaTotal > 1e6 );
  const double rel = std::abs( report.holeAreaTotal - bruteArea ) / bruteArea;
  QVERIFY2( rel < 0.05, qPrintable( QStringLiteral( "holeArea=%1 brute=%2 rel=%3" )
                                        .arg( report.holeAreaTotal )
                                        .arg( bruteArea )
                                        .arg( rel ) ) );

  // 只有一个空洞连通域，且在东北角：最深点贴近 (10000,10000) 角。
  QCOMPARE( report.holeCount, 1 );
  QCOMPARE( report.regions.size(), std::size_t( 1 ) );
  const HoleRegion &hole = report.regions.front();
  QVERIFY( hole.deepest.x > 9800 );
  QVERIFY( hole.deepest.y > 9800 );
  QVERIFY( hole.maxDistance > 5000 ); // 角点距最近井 ≈ 5.1km
  // 单调口径自洽：空洞面积 = 格数 × 格面积。
  QCOMPARE( hole.area, double( hole.cellCount ) * 100.0 * 100.0 );
  // 覆盖率 = 1 − 空洞/域（同栅格口径）。
  const double expectedRatio = 1.0 - report.holeAreaTotal / ( 100.0 * 100.0 * 10000 );
  QVERIFY( std::abs( report.coverageRatio - expectedRatio ) < 1e-9 );
  QVERIFY( report.coverageRatio > 0.9 );
}

void TestWellSitingKernel::holeBoundaryPolygon()
{
  const SitingOptions options = holeOptions();
  const CoverageReport report =
      diagnoseCoverage( { squareDomain() }, regularWellsMissingNE(), options );
  QCOMPARE( report.status, Status::Ok );
  const HoleRegion &hole = report.regions.front();
  // GEOS 边界：外环非空、闭合环至少 4 点、包住空洞主体（东北角）。
  QVERIFY( !hole.boundaryParts.empty() );
  double minX = kInf, minY = kInf, maxX = -kInf, maxY = -kInf;
  std::size_t totalPts = 0;
  for ( const std::vector<Point2> &ring : hole.boundaryParts )
  {
    totalPts += ring.size();
    for ( const Point2 &p : ring )
    {
      minX = std::min( minX, p.x );
      minY = std::min( minY, p.y );
      maxX = std::max( maxX, p.x );
      maxY = std::max( maxY, p.y );
    }
  }
  QVERIFY( totalPts >= 4 );
  QVERIFY2( maxX > 9900 && maxY > 9900, "空洞边界应抵达东北角" );
  // 阶梯口径的面积注记：显示边界围出的面积与格计数面积同量级（不超 2 倍）。
  double shoelace = 0;
  for ( const std::vector<Point2> &ring : hole.boundaryParts )
    for ( std::size_t i = 0; i + 1 < ring.size(); ++i )
      shoelace += ring[i].x * ring[i + 1].y - ring[i + 1].x * ring[i].y;
  const double polyArea = std::abs( shoelace ) / 2;
  QVERIFY( polyArea > hole.area * 0.5 );
  QVERIFY( polyArea < hole.area * 2.0 );
}

void TestWellSitingKernel::boundaryBudgetSkipsOversized()
{
  // 边界提取预算：超预算的空洞不画轮廓（量算不受影响），note 如实注记。
  SitingOptions options = holeOptions();
  options.maxBoundaryCells = 10; // 夹具空洞 233 格 → 必超
  const CoverageReport report =
      diagnoseCoverage( { squareDomain() }, regularWellsMissingNE(), options );
  QCOMPARE( report.status, Status::Ok );
  QCOMPARE( report.holeCount, 1 );
  QVERIFY( report.holeAreaTotal > 2.0e6 ); // 量算不受预算影响
  QVERIFY( report.regions.front().boundaryParts.empty() );
  QVERIFY( report.note.find( "超边界预算" ) != std::string::npos );

  // describeField(withBoundaries=false)（评估路径）零 GEOS 消费也零边界。
  const SitingOptions normal = holeOptions();
  const SitingField field = sampleField( { squareDomain() }, regularWellsMissingNE(), normal );
  const CoverageReport lean = describeField( field, false );
  QVERIFY( lean.regions.front().boundaryParts.empty() );
  QCOMPARE( lean.holeAreaTotal, report.holeAreaTotal ); // 同格同参——量算一致
}

void TestWellSitingKernel::emptyWellsAllHole()
{
  const SitingOptions options = holeOptions();
  const CoverageReport report = diagnoseCoverage( { squareDomain() }, {}, options );
  QCOMPARE( report.status, Status::Ok );
  QCOMPARE( report.holeCount, 1 );
  // 无井退化：全域是空洞；域均值距最近井 = +inf（如实，不虚造 0）。
  QVERIFY( std::isinf( report.domainMeanDistance ) );
  QVERIFY( report.coverageRatio < 1e-9 );
  QVERIFY( !report.spacing.valid );
  const double expected = 100.0 * 100.0 * 10000; // 域内全部格
  QVERIFY( std::abs( report.holeAreaTotal - expected ) < 0.02 * expected );
}

void TestWellSitingKernel::budgetExceeded()
{
  SitingOptions options = holeOptions();
  options.cellSize = 5; // 10km 域 → 2000×2000 = 4M 格
  options.maxCells = 1000;
  const SitingField field = sampleField( { squareDomain() }, regularWellsMissingNE(), options );
  QCOMPARE( field.status, Status::BudgetExceeded );
  QVERIFY( !field.message.empty() );
  const CoverageReport report = describeField( field );
  QCOMPARE( report.status, Status::BudgetExceeded );
}

void TestWellSitingKernel::invalidInputs()
{
  const SitingOptions options;
  QCOMPARE( sampleField( {}, {}, options ).status, Status::InvalidInput );
  Polygon bad; // 外环不足 3 点
  bad.exterior.points = { Point2{ 0, 0 }, Point2{ 1, 1 } };
  QCOMPARE( sampleField( { bad }, {}, options ).status, Status::InvalidInput );
  SitingOptions zero;
  zero.controlRadius = 0;
  QCOMPARE( sampleField( { squareDomain() }, {}, zero ).status, Status::InvalidInput );
}

void TestWellSitingKernel::candidateDeepestAndGrid()
{
  const SitingOptions options = holeOptions();
  const SitingField field = sampleField( { squareDomain() }, regularWellsMissingNE(), options );
  QCOMPARE( field.status, Status::Ok );

  // 间距 500：1 个 deepest + 9 个规则网格候选（合成夹具可解析的口径）。
  AvoidSurfaces noAvoid;
  CandidateOptions cand;
  cand.gridSpacing = 500;
  const std::vector<CandidatePoint> cands =
      generateCandidates( field, noAvoid, cand, options );
  QCOMPARE( int( cands.size() ), 10 );
  int deepestCount = 0, gridCount = 0;
  for ( const CandidatePoint &c : cands )
  {
    QCOMPARE( c.holeId, 1 );
    if ( c.strategy == "deepest" )
      ++deepestCount;
    else if ( c.strategy == "grid" )
      ++gridCount;
    else
      QFAIL( "unknown strategy" );
    // 候选都在空洞内：基础场距离 > 井控半径。
    QVERIFY( c.holeDistance > options.controlRadius );
  }
  QCOMPARE( deepestCount, 1 );
  QCOMPARE( gridCount, 9 );
  const CandidatePoint &deepest =
      *std::find_if( cands.begin(), cands.end(),
                     []( const CandidatePoint &c ) { return c.strategy == "deepest"; } );
  QVERIFY( deepest.pos.x > 9800 && deepest.pos.y > 9800 );

  // includeDeepest=false + perHoleLimit=3 → 恰好 3 个网格候选（按空洞深度
  // 降序截断——前三是 (9550,9950)/(9550,9450)/(9050,9950)）。
  CandidateOptions gridOnly;
  gridOnly.includeDeepest = false;
  gridOnly.gridSpacing = 500;
  gridOnly.perHoleLimit = 3;
  const std::vector<CandidatePoint> limited =
      generateCandidates( field, noAvoid, gridOnly, options );
  QCOMPARE( int( limited.size() ), 3 );
  for ( const CandidatePoint &c : limited )
    QCOMPARE( c.strategy.c_str(), "grid" );
}

void TestWellSitingKernel::candidateAvoidance()
{
  const SitingOptions options = holeOptions();
  const SitingField field = sampleField( { squareDomain() }, regularWellsMissingNE(), options );

  // 避让线沿对角穿过空洞腹地，缓冲 700m：最深点距线 ≈1343m 存活，
  // 更近的网格候选被否。
  AvoidSurfaces avoid;
  avoid.lines.push_back( { Point2{ 8000, 10000 }, Point2{ 10000, 8000 } } );
  avoid.lineBuffer = 700;
  CandidateOptions cand;
  cand.gridSpacing = 500;
  const std::vector<CandidatePoint> cands =
      generateCandidates( field, avoid, cand, options );
  QVERIFY( !cands.empty() );
  for ( const CandidatePoint &c : cands )
    QVERIFY2( sf::distanceToPolyline( c.pos, avoid.lines.front() ) >= avoid.lineBuffer,
              "候选点落在避让缓冲内" );

  // 缓冲加到 1500m：所有候选（含最深点 1343m）都被否——避让强度单调。
  AvoidSurfaces tighter = avoid;
  tighter.lineBuffer = 1500;
  const std::vector<CandidatePoint> none =
      generateCandidates( field, tighter, cand, options );
  QVERIFY( none.empty() );

  // 边界内缩 500m（不叠避让线，单测内缩语义）：所有候选距域边界 ≥ 500
  //（最深点在角上必被否掉）。
  CandidateOptions margin;
  margin.gridSpacing = 500;
  margin.boundaryMargin = 500;
  AvoidSurfaces noAvoid;
  const std::vector<CandidatePoint> margined =
      generateCandidates( field, noAvoid, margin, options );
  QVERIFY( !margined.empty() );
  for ( const CandidatePoint &c : margined )
  {
    QVERIFY( c.pos.x >= 500 && c.pos.x <= 9500 );
    QVERIFY( c.pos.y >= 500 && c.pos.y <= 9500 );
  }

  // 禁钻多边形盖住空洞全部格（6000+ 象限）→ 候选清空。
  AvoidSurfaces forbid;
  Polygon cover;
  cover.exterior.points = { Point2{ 6000, 6000 }, Point2{ 10000, 6000 },
                            Point2{ 10000, 10000 }, Point2{ 6000, 10000 } };
  forbid.polygons.push_back( cover );
  const std::vector<CandidatePoint> forbidden =
      generateCandidates( field, forbid, cand, options );
  QVERIFY( forbidden.empty() );

  // 距既有井最小距离：正约束只会删不会增——存活的都满足距离门。
  CandidateOptions withMin;
  withMin.gridSpacing = 500;
  withMin.minWellDistance = 3500; // 与井控半径同值：候选天然在远处
  const std::vector<CandidatePoint> unrestricted =
      generateCandidates( field, avoid, withMin, options );
  for ( const CandidatePoint &c : unrestricted )
    QVERIFY( bruteNearest( c.pos.x, c.pos.y, regularWellsMissingNE() ) >= 3500.0 );
}

void TestWellSitingKernel::scenarioImprovementDirectionAndMagnitude()
{
  const SitingOptions options = holeOptions();
  const std::vector<Point2> wells = regularWellsMissingNE();
  const std::vector<Point2> candidate{ Point2{ 9000, 9000 } }; // 补回挖去的井位

  const ScenarioMetrics base = baselineMetrics( { squareDomain() }, wells, {}, options );
  const ScenarioMetrics withOne =
      scenarioMetrics( { squareDomain() }, wells, candidate, {}, options );

  QCOMPARE( base.holeCount, 1 );
  QVERIFY( base.holeAreaTotal > 1e6 );
  // 方向：加井后空洞清零、覆盖率升、域均值距井降。
  QCOMPARE( withOne.holeCount, 0 );
  QCOMPARE( withOne.holeAreaTotal, 0.0 );
  QVERIFY( withOne.coverageRatio > base.coverageRatio );
  QVERIFY( withOne.domainMeanDistance < base.domainMeanDistance );
  QVERIFY( withOne.note.find( "候选井" ) != std::string::npos ); // 部署假设入注记

  // 幅度（解析）：补井后空洞面积恰好归零——同栅格口径下的精确断言。
  QCOMPARE( base.holeAreaTotal - withOne.holeAreaTotal, base.holeAreaTotal );

  // 层位加权井控密度：单层 8 实井 → +1 候选 = 9/100km² = 0.09 口/km²（权重归一）。
  std::vector<LayerWells> layers;
  LayerWells layer;
  layer.horizon = "D61";
  layer.points = wells;
  layers.push_back( layer );
  const ScenarioMetrics withOneLayered =
      scenarioMetrics( { squareDomain() }, wells, candidate, layers, options );
  QCOMPARE( withOneLayered.weightedDensity, 9.0 / 100.0 );
  const ScenarioMetrics baseLayered =
      baselineMetrics( { squareDomain() }, wells, layers, options );
  QCOMPARE( baseLayered.weightedDensity, 8.0 / 100.0 );
  // 双层等权：一层有井一层无井 → 密度 = (9+1)/2/100。
  LayerWells emptyLayer;
  emptyLayer.horizon = "C3";
  layers.push_back( emptyLayer );
  const ScenarioMetrics twoLayer =
      scenarioMetrics( { squareDomain() }, wells, candidate, layers, options );
  QCOMPARE( twoLayer.weightedDensity, ( 9.0 + 1.0 ) / 2.0 / 100.0 );
}

void TestWellSitingKernel::contributionMatchesScenarioDelta()
{
  const SitingOptions options = holeOptions();
  const std::vector<Point2> wells = regularWellsMissingNE();

  const SitingField field = sampleField( { squareDomain() }, wells, options );
  const ScenarioMetrics base = baselineMetrics( { squareDomain() }, wells, {}, options );

  // 贡献分与「基线 − 单候选方案」的同栅格口径精确一致（三处不同井位）。
  const std::vector<Point2> probes{ Point2{ 9000, 9000 }, Point2{ 9500, 9500 },
                                    Point2{ 8000, 9000 } };
  for ( const Point2 &probe : probes )
  {
    const CandidateContribution contribution = contributionOf( field, probe, options );
    const ScenarioMetrics scenario =
        scenarioMetrics( { squareDomain() }, wells, { probe }, {}, options );
    QCOMPARE( contribution.holeAreaReduction, base.holeAreaTotal - scenario.holeAreaTotal );
    QVERIFY( std::abs( contribution.meanDistanceReduction -
                       ( base.domainMeanDistance - scenario.domainMeanDistance ) ) < 1e-9 );
    QVERIFY( contribution.holeAreaReduction >= 0 );
    QVERIFY( contribution.meanDistanceReduction >= 0 );
  }
}

void TestWellSitingKernel::largeFieldThroughputRatio()
{
  // 抽样式比率门：同一核在大格网（500×500）与参照小格网（100×100）上的
  // 吞吐比 ≥ 0.5——桶格索引应让单格成本近似常数；比率塌方说明索引退化
  // （如环形扩张未收敛）。不设绝对毫秒阈值。
  std::vector<Point2> wells;
  for ( int i = 0; i < 200; ++i )
    wells.push_back( Point2{ double( i % 20 ) * 950.0 + 100.0,
                             double( i / 20 ) * 950.0 + 100.0 } );

  const auto throughput = [&]( double cellSize ) -> double {
    SitingOptions options;
    options.controlRadius = 2500;
    options.cellSize = cellSize;
    QElapsedTimer timer;
    timer.start();
    const SitingField field = sampleField( { squareDomain() }, wells, options );
    const qint64 elapsed = timer.elapsed();
    // QCOMPARE/QVERIFY 在 lambda 内展开为 return;（void）——状态断言放外面。
    Q_ASSERT( field.status == Status::Ok );
    return double( field.inDomain.size() ) / double( std::max<qint64>( elapsed, 1 ) );
  };

  const double reference = throughput( 100 ); // 100×100
  const double large = throughput( 20 );      // 500×500
  SitingOptions sanity;
  sanity.controlRadius = 2500;
  sanity.cellSize = 20;
  QCOMPARE( sampleField( { squareDomain() }, wells, sanity ).status, Status::Ok );
  QVERIFY2( large >= 0.5 * reference,
            qPrintable( QStringLiteral( "throughput ratio %1/%2 = %3" )
                            .arg( large )
                            .arg( reference )
                            .arg( large / reference ) ) );
}

QTEST_MAIN( TestWellSitingKernel )
#include "tst_wellsiting_kernel.moc"
