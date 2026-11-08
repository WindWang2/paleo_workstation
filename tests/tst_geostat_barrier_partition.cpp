// 层：测试壳（tests，链 paleo_algorithms）
#include "../src/algorithms/geostat/sgs.h"
#include "../src/algorithms/geostat/types.h"
#include "../src/algorithms/geostat/variogram.h"
#include "../src/algorithms/singlefactor/sgsbarrier.h"
#include "../src/algorithms/singlefactor/support.h"
#include "../src/algorithms/singlefactor/types.h"

#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

// P3：SGS 硬屏障隔断的行为回归。分量归属经生产同款 labelHardBarriers
//（grid_connectivity_v1：有限端点可绕行的 supercover 栅格化 + 4 连通泛洪）
// 物化，再经 toSgsBarrierInput 稳定重编号进入数值核——不是手绘一张与
// 生产无关的分区图。
using namespace paleo::geostat;

namespace
{

GridSpec makeGrid( int cols, int rows, double x0, double y0, double cell )
{
  GridSpec grid;
  grid.cols = cols;
  grid.rows = rows;
  grid.originX = x0;
  grid.originY = y0;
  grid.pixelWidth = cell;
  grid.pixelHeight = -cell;
  return grid;
}

VariogramModel gaussianModel()
{
  VariogramModel model;
  model.type = VariogramModelType::Gaussian;
  model.nugget = 0.2;
  model.sill = 1.0;
  model.range = 40;
  return model;
}

GridSpec barrierGrid() { return makeGrid( 32, 32, -10.0, 250.0, 8.0 ); }

// 垂直硬屏障线：x=122（列 16 中心线），贯穿全高——有限端点在网格边距外，
// 两侧不连通。列 16 栅格化为屏障格（-2）。
std::vector<paleo::singlefactor::ConstraintLine> verticalBarrierLine( const QString &id )
{
  paleo::singlefactor::ConstraintLine line;
  line.stableId = id.toStdString();
  line.semantic = paleo::singlefactor::Semantic::HardBarrier;
  line.enabled = true;
  line.points = { paleo::singlefactor::Point2{ 122.0, 260.0 },
                  paleo::singlefactor::Point2{ 122.0, 130.0 },
                  paleo::singlefactor::Point2{ 122.0, -20.0 } };
  return { line };
}

// 左右两组井：左值 ~1（对数正态小值），右值 ~1000——两分量的支持分布
// 悬殊，任何跨分量正态得分/条件化泄漏都会让值域混叠。
std::vector<paleo::singlefactor::Sample> leftRightWells( std::uint64_t seed, double rightShift = 0.0 )
{
  std::mt19937_64 rng( seed );
  auto jitter = [&rng]() { return ( static_cast<double>( rng() % 1000 ) / 1000.0 - 0.5 ) * 3.0; };
  std::vector<paleo::singlefactor::Sample> wells;
  auto addWell = [&]( double x, double y, double value ) {
    paleo::singlefactor::Sample sample;
    sample.x = x;
    sample.y = y;
    sample.value = value;
    wells.push_back( sample );
  };
  const GridSpec grid = barrierGrid();
  for ( int column = 2; column <= 13; column += 3 )
    for ( int row = 2; row <= 30; row += 4 )
      addWell( grid.cellCenterX( column ) + jitter(), grid.cellCenterY( row ) + jitter(),
               1.0 + static_cast<double>( rng() % 100 ) / 100.0 * 4.0 );
  for ( int column = 19; column <= 30; column += 3 )
    for ( int row = 1; row <= 29; row += 4 )
      addWell( grid.cellCenterX( column ) + jitter(), grid.cellCenterY( row ) + jitter(),
               1000.0 + static_cast<double>( rng() % 100 ) / 100.0 * 8.0 + rightShift );
  return wells;
}

// 生产同款物化：labelHardBarriers → toSgsBarrierInput。
struct LabeledBarrier
{
  paleo::geostat::SgsBarrierInput input;
  paleo::singlefactor::BarrierGrid raw;
  bool ok = false;
};

LabeledBarrier labelVerticalBarrier( const std::vector<paleo::singlefactor::Sample> &wells,
                                     const QString &lineId = QStringLiteral( "fault-a" ) )
{
  LabeledBarrier out;
  const GridSpec grid = barrierGrid();
  const double span = std::max( grid.cols * grid.pixelWidth, -grid.pixelHeight * grid.rows );
  out.raw = paleo::singlefactor::labelHardBarriers(
      paleo::singlefactor::toSinglefactorGrid( grid ), verticalBarrierLine( lineId ), wells,
      span * 1e-9 );
  if ( !out.raw.error.empty() || !out.raw.ambiguous.empty() )
    return out;
  out.input = paleo::singlefactor::toSgsBarrierInput( out.raw, wells, "grid_connectivity_v1" );
  out.ok = true;
  return out;
}

std::vector<Sample> geostatSamples( const std::vector<paleo::singlefactor::Sample> &wells )
{
  std::vector<Sample> samples;
  samples.reserve( wells.size() );
  for ( const paleo::singlefactor::Sample &well : wells )
    samples.push_back( Sample{ well.x, well.y, well.value } );
  return samples;
}

SgsParams barrierParams( const SgsBarrierInput &barrier )
{
  SgsParams params;
  params.nRealizations = 2;
  params.seed = 20261108;
  params.maxPoints = 12;
  params.barrier = barrier;
  return params;
}

} // namespace

class GeostatBarrierPartitionTests : public QObject
{
  Q_OBJECT
private slots:
  // 合法性检查在分配 realizations 大数组之前完成——错误输入不留半成品。
  void invalidBarrierInputsRejected();
  // 屏障格与域外格保持 nodata，不跨线补邻居、不把缺测填成零。
  void barrierCellsStayNodata();
  // 两分量支持分布悬殊时，值域严格分离（正态得分/条件化均不跨分量）。
  void twoPartitionsStrictlySeparated();
  // 反事实：固定显式变差模型，只改另一分量样本值——本分量实现逐位不变
  //（同时抓静态井越界与已模拟格越界两类泄漏）。
  void counterfactualIsolationStaticAndDynamic();
  // 镜像反事实：改左（先模拟）分量样本值，断言右（后模拟）分量逐位不变
  //——动态邻域过滤（已模拟格跨分量不可见）失效时，右分量会看见左分量
  // draw 的变化而红；静态向泄漏由 counterfactualIsolationStaticAndDynamic 覆盖。
  void counterfactualReverseIsolationCatchesDynamicLeak();
  // 随机流按「实现 × 稳定分量号」派生：并行实现与逐个串行实现逐位一致。
  void parallelRealizationsMatchSequentialWithBarrier();
  // 单样本分量按既有退化语义恒回该值（如实退化，不发明地质阈值）。
  void singleSampleComponentDegenerates();
  // 同 seed 同输入两次运行逐位一致。
  void sameSeedReproducibleWithBarrier();
  // 屏障上井（sampleComponent < 0）与 localidw 对 ambiguous 井同口径：拒绝。
  void sampleOnBarrierRejected();
  // 全场自动拟合（无显式模型）在隔断路径仍可运行——QC 由工作流层注明共享。
  void autoFitModelStillRunsPartitioned();
};

void GeostatBarrierPartitionTests::invalidBarrierInputsRejected()
{
  const GridSpec grid = barrierGrid();
  const std::vector<Sample> samples = geostatSamples( leftRightWells( 7 ) );
  const LabeledBarrier labeled = labelVerticalBarrier( leftRightWells( 7 ) );
  QVERIFY( labeled.ok );
  const SgsBarrierInput &good = labeled.input;
  QCOMPARE( good.componentCount, 2 );

  const auto expectReject = [&]( const SgsBarrierInput &bad, const char *what ) {
    SgsParams params = barrierParams( bad );
    const SgsResult result = sgs( samples, grid, gaussianModel(), params );
    QVERIFY2( result.status == Status::InvalidInput, what );
    QVERIFY2( !result.message.empty(), what );
    QVERIFY2( result.realizations.empty(), what ); // 大数组未分配
  };

  SgsBarrierInput bad = good;
  bad.cellComponent.resize( bad.cellComponent.size() - 1 ); // 尺寸错
  expectReject( bad, "cellComponent size mismatch must reject" );

  bad = good;
  bad.cellComponent[5] = 99; // 分量号越界
  expectReject( bad, "cell component label out of range must reject" );

  bad = good;
  bad.cellComponent[5] = -3; // 非法负值
  expectReject( bad, "cell component label < -2 must reject" );

  bad = good;
  bad.sampleComponent.resize( bad.sampleComponent.size() - 1 );
  expectReject( bad, "sampleComponent size mismatch must reject" );

  bad = good;
  bad.sampleComponent[0] = -1; // 屏障上/未归属井
  expectReject( bad, "unassigned sample must reject" );

  bad = good;
  bad.sampleComponent[0] = 7; // 分量号越界
  expectReject( bad, "sample component out of range must reject" );

  bad = good;
  bad.componentCount = 0;
  expectReject( bad, "componentCount must be >= 1" );

  bad = good;
  bad.componentCount = 3; // 分量 2 无样本（无井闭合区，同 localidw 口径拒绝）
  bad.cellComponent.assign( grid.rows * grid.cols, 0 );
  for ( int row = 0; row < grid.rows; ++row )
    bad.cellComponent[static_cast<std::size_t>( row ) * grid.cols + 31] = 2;
  for ( int &label : bad.cellComponent )
    if ( label == 1 )
      label = 0;
  for ( int &label : bad.sampleComponent )
    label = 0;
  expectReject( bad, "component without samples must reject" );

  // 重合样本跨分量归属矛盾。
  SgsBarrierInput conflict = good;
  conflict.sampleComponent.assign( samples.size(), 0 );
  conflict.sampleComponent[0] = 0;
  std::vector<Sample> coincident = samples;
  coincident.push_back( Sample{ samples[0].x, samples[0].y, 12345.0 } );
  conflict.sampleComponent.push_back( 1 );
  expectReject( conflict, "coincident samples in different components must reject" );
  QCOMPARE( static_cast<int>( conflict.sampleComponent.size() ),
            static_cast<int>( coincident.size() ) );
  SgsParams conflictParams = barrierParams( conflict );
  QVERIFY( sgs( coincident, grid, gaussianModel(), conflictParams ).status == Status::InvalidInput );
}

void GeostatBarrierPartitionTests::barrierCellsStayNodata()
{
  const std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 7 );
  const LabeledBarrier labeled = labelVerticalBarrier( wells );
  QVERIFY( labeled.ok );
  const GridSpec grid = barrierGrid();
  const SgsResult result = sgs( geostatSamples( wells ), grid, gaussianModel(), barrierParams( labeled.input ) );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.barrierCells, 32 ); // 屏障列 32 格
  QCOMPARE( result.componentCellCount.size(), std::size_t( 2 ) );
  QCOMPARE( result.componentCellCount[0] + result.componentCellCount[1] + result.barrierCells, 32 * 32 );
  QVERIFY( result.componentSampleCount[0] > 0 && result.componentSampleCount[1] > 0 );
  QCOMPARE( result.consumedBarrierIds, std::vector<std::string>{ std::string( "fault-a" ) } );
  QCOMPARE( QString::fromStdString( result.barrierModel ), QStringLiteral( "grid_connectivity_v1" ) );
  for ( const std::vector<double> &field : result.realizations )
    for ( int row = 0; row < grid.rows; ++row )
    {
      const double value = field[static_cast<std::size_t>( row ) * grid.cols + 16];
      QVERIFY2( !std::isfinite( value ), "barrier column must stay nodata" );
    }
  QCOMPARE( result.nodataCells, 32 );
}

void GeostatBarrierPartitionTests::twoPartitionsStrictlySeparated()
{
  const std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 7 );
  const LabeledBarrier labeled = labelVerticalBarrier( wells );
  QVERIFY( labeled.ok );
  const GridSpec grid = barrierGrid();
  const SgsResult result = sgs( geostatSamples( wells ), grid, gaussianModel(), barrierParams( labeled.input ) );
  QCOMPARE( result.status, Status::Ok );
  for ( const std::vector<double> &field : result.realizations )
  {
    double leftMax = -std::numeric_limits<double>::infinity();
    double rightMin = std::numeric_limits<double>::infinity();
    for ( int row = 0; row < grid.rows; ++row )
      for ( int column = 0; column < grid.cols; ++column )
      {
        const double value = field[static_cast<std::size_t>( row ) * grid.cols + column];
        if ( !std::isfinite( value ) )
          continue;
        if ( column < 16 )
          leftMax = std::max( leftMax, value );
        else if ( column > 16 )
          rightMin = std::min( rightMin, value );
      }
    // 左支持 ~[1,5]+尾部 10%，右支持 ~[1000,1008]-尾部——中间隔着 3 个数量级，
    // 大规模混叠（末端掩膜式假隔断、表用错分量）会让两侧值域交叠。静态/
    // 动态邻域的逐位泄漏由两个反事实测试分别钉住（本测试的值域断言对小
    // 泄漏不敏感——正态得分端部限幅会掩护零星越界点）。
    QVERIFY2( leftMax < rightMin, "left/right partitions must stay value-separated" );
    QVERIFY( leftMax < 20.0 );
    QVERIFY( rightMin > 900.0 );
  }
}

void GeostatBarrierPartitionTests::counterfactualIsolationStaticAndDynamic()
{
  const std::vector<paleo::singlefactor::Sample> baseWells = leftRightWells( 7 );
  const std::vector<paleo::singlefactor::Sample> shiftedWells = leftRightWells( 7, 500.0 );
  const LabeledBarrier base = labelVerticalBarrier( baseWells );
  const LabeledBarrier shifted = labelVerticalBarrier( shiftedWells );
  QVERIFY( base.ok && shifted.ok );
  const GridSpec grid = barrierGrid();
  const SgsResult baseRun =
      sgs( geostatSamples( baseWells ), grid, gaussianModel(), barrierParams( base.input ) );
  const SgsResult shiftedRun =
      sgs( geostatSamples( shiftedWells ), grid, gaussianModel(), barrierParams( shifted.input ) );
  QCOMPARE( baseRun.status, Status::Ok );
  QCOMPARE( shiftedRun.status, Status::Ok );
  QCOMPARE( baseRun.realizations.size(), shiftedRun.realizations.size() );
  for ( std::size_t r = 0; r < baseRun.realizations.size(); ++r )
  {
    for ( int row = 0; row < grid.rows; ++row )
      for ( int column = 0; column < 16; ++column )
      {
        const std::size_t cell = static_cast<std::size_t>( row ) * grid.cols + column;
        const double a = baseRun.realizations[r][cell];
        const double b = shiftedRun.realizations[r][cell];
        QVERIFY2( std::isfinite( a ) && a == b,
                  "left partition must be bitwise unchanged when only right samples change" );
      }
    // 右分量确实变了（500 平移可见）——防止两侧都冻结的假阳性。
    bool rightChanged = false;
    for ( int row = 0; row < grid.rows && !rightChanged; ++row )
      for ( int column = 17; column < grid.cols && !rightChanged; ++column )
      {
        const std::size_t cell = static_cast<std::size_t>( row ) * grid.cols + column;
        if ( baseRun.realizations[r][cell] != shiftedRun.realizations[r][cell] )
          rightChanged = true;
      }
    QVERIFY2( rightChanged, "right partition must respond to its own sample change" );
  }
}

void GeostatBarrierPartitionTests::counterfactualReverseIsolationCatchesDynamicLeak()
{
  // 左值整体 +50：左分量（row-major 首现 = 分量 0，先模拟）的模拟场随之
  // 改变；右分量（分量 1，后模拟）的条件点（井）与正态得分表都不含左值
  // ——唯一可能被污染的通道是「借用左分量已模拟格」，而 queryNearestInComponent
  // 的分量过滤把它挡死。断言右分量逐位不变。
  const std::vector<paleo::singlefactor::Sample> baseWells = leftRightWells( 7 );
  std::vector<paleo::singlefactor::Sample> shiftedWells = leftRightWells( 7 );
  for ( paleo::singlefactor::Sample &well : shiftedWells )
    if ( well.x < 122.0 )
      well.value += 50.0;
  const LabeledBarrier base = labelVerticalBarrier( baseWells );
  const LabeledBarrier shifted = labelVerticalBarrier( shiftedWells );
  QVERIFY( base.ok && shifted.ok );
  const GridSpec grid = barrierGrid();
  const SgsResult baseRun =
      sgs( geostatSamples( baseWells ), grid, gaussianModel(), barrierParams( base.input ) );
  const SgsResult shiftedRun =
      sgs( geostatSamples( shiftedWells ), grid, gaussianModel(), barrierParams( shifted.input ) );
  QCOMPARE( baseRun.status, Status::Ok );
  QCOMPARE( shiftedRun.status, Status::Ok );
  for ( std::size_t r = 0; r < baseRun.realizations.size(); ++r )
  {
    for ( int row = 0; row < grid.rows; ++row )
      for ( int column = 17; column < grid.cols; ++column )
      {
        const std::size_t cell = static_cast<std::size_t>( row ) * grid.cols + column;
        const double a = baseRun.realizations[r][cell];
        const double b = shiftedRun.realizations[r][cell];
        QVERIFY2( ( std::isnan( a ) && std::isnan( b ) ) || a == b,
                  "right (later-simulated) partition must not see left draws" );
      }
    // 左分量确实响应了自己的样本变化——防两侧冻结的假阳性。
    bool leftChanged = false;
    for ( int row = 0; row < grid.rows && !leftChanged; ++row )
      for ( int column = 0; column < 16 && !leftChanged; ++column )
      {
        const std::size_t cell = static_cast<std::size_t>( row ) * grid.cols + column;
        if ( baseRun.realizations[r][cell] != shiftedRun.realizations[r][cell] )
          leftChanged = true;
      }
    QVERIFY2( leftChanged, "left partition must respond to its own sample change" );
  }
}

void GeostatBarrierPartitionTests::parallelRealizationsMatchSequentialWithBarrier()
{
  const std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 11 );
  const LabeledBarrier labeled = labelVerticalBarrier( wells );
  QVERIFY( labeled.ok );
  const GridSpec grid = barrierGrid();

  SgsParams batch = barrierParams( labeled.input );
  batch.nRealizations = 4; // 无回调 → 实现级并行档
  const SgsResult parallel = sgs( geostatSamples( wells ), grid, gaussianModel(), batch );
  QCOMPARE( parallel.status, Status::Ok );

  // 串行参考：同参数带 progress 回调（有回调 → 串行档逐实现执行），与
  // parallelRealizationsMatchSequential（无屏障版）同口径——每实现流按
  // 「实现号」派生，与调度方式无关。
  SgsParams serialParams = barrierParams( labeled.input );
  serialParams.nRealizations = 4;
  Control serialControl;
  serialControl.progress = []( double ) {};
  const SgsResult serial = sgs( geostatSamples( wells ), grid, gaussianModel(), serialParams,
                                serialControl );
  QCOMPARE( serial.status, Status::Ok );
  QCOMPARE( serial.realizations.size(), std::size_t( 4 ) );
  for ( int r = 0; r < 4; ++r )
    for ( std::size_t cell = 0; cell < parallel.realizations[static_cast<std::size_t>( r )].size(); ++cell )
    {
      const double a = parallel.realizations[static_cast<std::size_t>( r )][cell];
      const double b = serial.realizations[static_cast<std::size_t>( r )][cell];
      QVERIFY2( a == b || ( !std::isfinite( a ) && !std::isfinite( b ) ),
                "parallel realization must match serial bit-for-bit" );
    }
}

void GeostatBarrierPartitionTests::singleSampleComponentDegenerates()
{
  // 手工分量：左分量仅 1 口井（列 2），右分量 32 口——单样本分量的正态得分
  // 表 z/y 各 1 组，反变换恒回该值（singleSampleDegeneratesHonestly 同语义）。
  const GridSpec grid = barrierGrid();
  std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 7 );
  std::vector<paleo::singlefactor::Sample> reduced;
  paleo::singlefactor::Sample only;
  only.x = grid.cellCenterX( 2 );
  only.y = grid.cellCenterY( 16 );
  only.value = 42.0;
  reduced.push_back( only );
  for ( const paleo::singlefactor::Sample &well : wells )
    if ( well.x > 122.0 )
      reduced.push_back( well );
  const LabeledBarrier labeled = labelVerticalBarrier( reduced );
  QVERIFY( labeled.ok );
  QCOMPARE( labeled.input.componentCount, 2 );
  const SgsResult result = sgs( geostatSamples( reduced ), grid, gaussianModel(), barrierParams( labeled.input ) );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.componentSampleCount[0], 1 );
  for ( const std::vector<double> &field : result.realizations )
    for ( int row = 0; row < grid.rows; ++row )
      for ( int column = 0; column < 16; ++column )
      {
        const double value = field[static_cast<std::size_t>( row ) * grid.cols + column];
        QCOMPARE( value, 42.0 ); // 唯一支持值如实退化
      }
}

void GeostatBarrierPartitionTests::sameSeedReproducibleWithBarrier()
{
  const std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 7 );
  const LabeledBarrier labeled = labelVerticalBarrier( wells );
  QVERIFY( labeled.ok );
  const GridSpec grid = barrierGrid();

  // 串行档（R=1）：路径本身必须确定。
  SgsParams serialParams = barrierParams( labeled.input );
  serialParams.nRealizations = 1;
  const SgsResult s1 = sgs( geostatSamples( wells ), grid, gaussianModel(), serialParams );
  const SgsResult s2 = sgs( geostatSamples( wells ), grid, gaussianModel(), serialParams );
  QCOMPARE( s1.status, Status::Ok );
  QCOMPARE( s2.status, Status::Ok );
  for ( std::size_t cell = 0; cell < s1.realizations[0].size(); ++cell )
  {
    const double a = s1.realizations[0][cell];
    const double b = s2.realizations[0][cell];
    QVERIFY2( a == b || ( std::isnan( a ) && std::isnan( b ) ),
              "serial partitioned path must be deterministic" );
  }

  // 并行档（R=2，无回调）：与串行档同 seed 一致。
  SgsParams parallelParams = barrierParams( labeled.input );
  parallelParams.nRealizations = 2;
  const SgsResult first = sgs( geostatSamples( wells ), grid, gaussianModel(), parallelParams );
  const SgsResult second = sgs( geostatSamples( wells ), grid, gaussianModel(), parallelParams );
  QCOMPARE( first.status, Status::Ok );
  QCOMPARE( second.status, Status::Ok );
  QCOMPARE( first.realizations.size(), second.realizations.size() );
  for ( std::size_t r = 0; r < first.realizations.size(); ++r )
    for ( std::size_t cell = 0; cell < first.realizations[r].size(); ++cell )
    {
      const double a = first.realizations[r][cell];
      const double b = second.realizations[r][cell];
      QVERIFY2( a == b || ( std::isnan( a ) && std::isnan( b ) ),
                "parallel partitioned path must be deterministic" );
    }
  QVERIFY2( first.realizations[0][0] == s1.realizations[0][0],
            "realization 0 must not depend on parallel/serial scheduling" );
}

void GeostatBarrierPartitionTests::sampleOnBarrierRejected()
{
  // 井恰落屏障列 → labelHardBarriers 记 ambiguous、归属 -1；数值核对
  // sampleComponent < 0 一律拒绝（与 localidw「硬屏障上的井点归属不明确」
  // 同口径——屏障上的井不静默分到某一侧）。
  const GridSpec grid = barrierGrid();
  std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 7 );
  paleo::singlefactor::Sample onBarrier;
  onBarrier.x = 122.0;
  onBarrier.y = grid.cellCenterY( 16 );
  onBarrier.value = 7.0;
  wells.push_back( onBarrier );
  const LabeledBarrier labeled = labelVerticalBarrier( wells );
  QVERIFY( !labeled.ok ); // ambiguous 非空 → 物化层先行拒绝
  QVERIFY( !labeled.raw.ambiguous.empty() );

  // 数值核兜底：即使物化层漏检，sampleComponent<0 仍拒绝。
  SgsBarrierInput input = labelVerticalBarrier( leftRightWells( 7 ) ).input;
  std::vector<Sample> samples = geostatSamples( leftRightWells( 7 ) );
  samples.push_back( Sample{ onBarrier.x, onBarrier.y, onBarrier.value } );
  input.sampleComponent.push_back( -1 );
  const SgsResult result = sgs( samples, grid, gaussianModel(), barrierParams( input ) );
  QCOMPARE( result.status, Status::InvalidInput );
}

void GeostatBarrierPartitionTests::autoFitModelStillRunsPartitioned()
{
  // 全场自动拟合（refitVariogramOnNormalScores 缺省 false）：模型全场共享、
  // 按分量实测得分方差标定——数值可运行；「模型仍共享、非每断块独立拟合」
  // 的事实由 QC 层声明（工作流测试断言），此处只验证执行与隔离不破。
  const std::vector<paleo::singlefactor::Sample> wells = leftRightWells( 7 );
  const LabeledBarrier labeled = labelVerticalBarrier( wells );
  QVERIFY( labeled.ok );
  const GridSpec grid = barrierGrid();
  SgsParams params = barrierParams( labeled.input );
  params.refitVariogramOnNormalScores = false;
  const SgsResult result = sgs( geostatSamples( wells ), grid, gaussianModel(), params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( static_cast<int>( result.realizations.size() ), params.nRealizations );
  double leftMax = 0, rightMin = 1e9;
  for ( int row = 0; row < grid.rows; ++row )
  {
    const double l = result.realizations[0][static_cast<std::size_t>( row ) * grid.cols + 3];
    const double rr = result.realizations[0][static_cast<std::size_t>( row ) * grid.cols + 28];
    leftMax = std::max( leftMax, l );
    rightMin = std::min( rightMin, rr );
  }
  QVERIFY( leftMax < rightMin );
}

QTEST_MAIN( GeostatBarrierPartitionTests )
#include "tst_geostat_barrier_partition.moc"
