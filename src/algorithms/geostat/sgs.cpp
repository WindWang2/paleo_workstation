// 层：数据
#include "sgs.h"

#include "linsolve.h"
#include "neighborhood.h"
#include "sgs_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

// 层：数据
namespace paleo::geostat
{

namespace
{

using detail::CondPoint;

// 已模拟格的格点邻域查询（模拟点恰在格心，格上环扫 + 堆剪枝）。
// 访问序是 2D 逐位结果的组成部分（并列距离靠访问序打破）——不动。
class SimulatedLattice
{
public:
  void reset( int cols, int rows )
  {
    m_cols = cols;
    m_rows = rows;
    m_values.assign( static_cast<std::size_t>( cols ) * rows,
                     std::numeric_limits<double>::quiet_NaN() );
  }

  void setValue( int column, int row, double value )
  {
    m_values[static_cast<std::size_t>( row ) * m_cols + column] = value;
  }

  // 最近 K 个已模拟格（按距离升序）。
  void queryNearest( int column, int row, const GridSpec &grid, int k,
                     std::vector<CondPoint> *out ) const
  {
    queryNearestImpl( column, row, grid, k, nullptr, 0, out );
  }

  // P3：分量内最近 K 个已模拟格——cellComponent[cell] == component 的格才
  // 可见（静态与已模拟两类邻域同时隔断的「动态」一侧）。扫描/剪枝/并列
  // 打破与无屏障版同一实现（queryNearestImpl），仅 visitCell 多一道归属过滤。
  void queryNearestInComponent( int column, int row, const GridSpec &grid, int k,
                                const std::vector<int> &cellComponent, int component,
                                std::vector<CondPoint> *out ) const
  {
    queryNearestImpl( column, row, grid, k, &cellComponent, component, out );
  }

private:
  void queryNearestImpl( int column, int row, const GridSpec &grid, int k,
                         const std::vector<int> *cellComponent, int component,
                         std::vector<CondPoint> *out ) const
  {
    out->clear();
    const double x0 = grid.cellCenterX( column );
    const double y0 = grid.cellCenterY( row );
    const double minCell = std::min( grid.pixelWidth, -grid.pixelHeight );
    const int maxRing = std::max( { column, m_cols - 1 - column, row, m_rows - 1 - row } );
    std::priority_queue<std::pair<double, std::size_t>> heap; // max-heap on d²
    for ( int ring = 1; ring <= maxRing; ++ring )
    {
      const double ringMin = std::max( 0, ring - 1 ) * minCell;
      if ( static_cast<int>( heap.size() ) == k && ringMin * ringMin >= heap.top().first )
        break;
      for ( int cx = column - ring; cx <= column + ring; ++cx )
      {
        visitCell( cx, row - ring, x0, y0, grid, &heap, k, cellComponent, component );
        visitCell( cx, row + ring, x0, y0, grid, &heap, k, cellComponent, component );
      }
      for ( int cy = row - ring + 1; cy <= row + ring - 1; ++cy )
      {
        visitCell( column - ring, cy, x0, y0, grid, &heap, k, cellComponent, component );
        visitCell( column + ring, cy, x0, y0, grid, &heap, k, cellComponent, component );
      }
    }
    std::vector<std::pair<double, std::size_t>> collected;
    collected.reserve( heap.size() );
    while ( !heap.empty() )
    {
      collected.push_back( heap.top() );
      heap.pop();
    }
    std::reverse( collected.begin(), collected.end() ); // 升序
    out->reserve( collected.size() );
    for ( const auto &entry : collected )
    {
      const std::size_t cell = entry.second;
      const int cellRow = static_cast<int>( cell / m_cols );
      const int cellColumn = static_cast<int>( cell % m_cols );
      out->push_back( CondPoint{ grid.cellCenterX( cellColumn ), grid.cellCenterY( cellRow ),
                                 0.0, m_values[cell] } );
    }
  }

  void visitCell( int cx, int cy, double x0, double y0, const GridSpec &grid,
                  std::priority_queue<std::pair<double, std::size_t>> *heap, int k,
                  const std::vector<int> *cellComponent = nullptr, int component = 0 ) const
  {
    if ( cx < 0 || cy < 0 || cx >= m_cols || cy >= m_rows )
      return;
    const std::size_t cell = static_cast<std::size_t>( cy ) * m_cols + cx;
    if ( cellComponent && ( *cellComponent )[cell] != component )
      return; // P3：跨分量已模拟格不可见——不为凑邻居跨屏障借点
    const double value = m_values[cell];
    if ( !std::isfinite( value ) )
      return;
    const double dx = grid.cellCenterX( cx ) - x0;
    const double dy = grid.cellCenterY( cy ) - y0;
    const double d2 = dx * dx + dy * dy;
    if ( static_cast<int>( heap->size() ) < k )
      heap->push( { d2, cell } );
    else if ( d2 < heap->top().first )
    {
      heap->pop();
      heap->push( { d2, cell } );
    }
  }

  int m_cols = 0;
  int m_rows = 0;
  std::vector<double> m_values;
};

// P3：SGS 硬屏障隔断的就绪结构——合法性检查（分配 realizations 大数组之前）
// 通过后一次性物化：每分量自己的去重样本下标、正态得分表、标定到该分量
// 高斯域的共享变差模型、分量内静态邻域索引、分量格子集。变差模型家族/
// 参数全场共享（不实现每断块独立变差拟合），仅按分量实测得分方差标定；
// QC 由调用方注明共享范围。
struct SgsPartition
{
  int components = 0;
  int barrierCellCount = 0; // cellComponent < 0 的格数（屏障/域外，nodata）
  std::vector<std::vector<std::size_t>> cells;        // 分量 c 的格索引（升序）
  std::vector<std::vector<std::uint32_t>> sampleIdx;  // 分量 c 的 deduped 样本下标
  std::vector<detail::NormalScoreTable> tables;       // 分量内正态得分表
  std::vector<VariogramModel> models;                 // 共享模型按分量方差标定
  std::vector<detail::NeighborIndex> indexes;         // 分量内静态邻域索引
};

// 返回空 = 就绪；非空 = 拒绝原因（result.message 由调用方带回）。
std::string prepareSgsPartition( const SgsBarrierInput &barrier, const GridSpec &grid,
                                 const std::vector<Sample> &samples,
                                 const std::vector<Sample> &deduped,
                                 const std::vector<std::uint32_t> &inputToDeduped,
                                 const VariogramModel &model, const SgsParams &clamped,
                                 SgsPartition *out )
{
  const std::size_t cells = static_cast<std::size_t>( grid.rows ) * grid.cols;
  if ( barrier.cellComponent.size() != cells )
    return "barrier cellComponent size does not match grid (expected rows*cols)";
  if ( barrier.sampleComponent.size() != samples.size() )
    return "barrier sampleComponent size does not match samples";
  if ( barrier.componentCount < 1 )
    return "barrier componentCount must be >= 1";
  for ( int label : barrier.cellComponent )
    if ( label < -2 || label >= barrier.componentCount )
      return "barrier cell component label out of range";
  for ( int label : barrier.sampleComponent )
    if ( label < 0 || label >= barrier.componentCount )
      return "barrier sample component unassigned or out of range";

  // dedupe 合并组分量一致性：同一位置的输入样本归属必须一致，否则归属矛盾。
  std::vector<int> dedupedComponent( deduped.size(), -1 );
  for ( std::size_t inputIndex = 0; inputIndex < inputToDeduped.size(); ++inputIndex )
  {
    const std::uint32_t merged = inputToDeduped[inputIndex];
    if ( merged == std::numeric_limits<std::uint32_t>::max() )
      continue; // 非有限样本（dedupe 已丢）
    const int component = barrier.sampleComponent[inputIndex];
    if ( dedupedComponent[merged] == -1 )
      dedupedComponent[merged] = component;
    else if ( dedupedComponent[merged] != component )
      return "coincident samples assigned to different barrier components";
  }

  out->components = barrier.componentCount;
  out->cells.assign( static_cast<std::size_t>( barrier.componentCount ), {} );
  out->sampleIdx.assign( static_cast<std::size_t>( barrier.componentCount ), {} );
  out->barrierCellCount = 0;
  for ( std::size_t cell = 0; cell < cells; ++cell )
  {
    const int label = barrier.cellComponent[cell];
    if ( label < 0 )
      ++out->barrierCellCount;
    else
      out->cells[static_cast<std::size_t>( label )].push_back( cell );
  }
  for ( std::size_t merged = 0; merged < deduped.size(); ++merged )
    out->sampleIdx[static_cast<std::size_t>( dedupedComponent[merged] )].push_back(
        static_cast<std::uint32_t>( merged ) );

  out->tables.resize( static_cast<std::size_t>( barrier.componentCount ) );
  out->models.resize( static_cast<std::size_t>( barrier.componentCount ) );
  out->indexes.resize( static_cast<std::size_t>( barrier.componentCount ) );
  for ( int component = 0; component < barrier.componentCount; ++component )
  {
    const auto &indices = out->sampleIdx[static_cast<std::size_t>( component )];
    if ( indices.empty() )
      return "barrier component " + std::to_string( component ) +
             " has no samples (unsupported no-well closed region)";
    if ( out->cells[static_cast<std::size_t>( component )].empty() )
      return "barrier component " + std::to_string( component ) + " has no grid cells";
    std::vector<Sample> componentSamples;
    std::vector<double> componentValues;
    componentSamples.reserve( indices.size() );
    componentValues.reserve( indices.size() );
    for ( std::uint32_t merged : indices )
    {
      componentSamples.push_back( deduped[merged] );
      componentValues.push_back( deduped[merged].value );
    }
    // W4：分量内去重样本 → 分量自己的正态得分表（反变换用该分量支持分布）。
    const detail::NormalScoreTable table = detail::NormalScoreTable::build( componentValues );
    out->tables[static_cast<std::size_t>( component )] = table;
    // 共享变差模型按分量实测得分方差标定；refit 开关语义与全场路径一致
    //（不偷偷开启，分量样本 <8 时 refitOnNormalScores 自身回退比例缩放）。
    out->models[static_cast<std::size_t>( component )] = regularizedForKriging(
        clamped.refitVariogramOnNormalScores
            ? detail::refitOnNormalScores( model, table, componentSamples )
            : detail::rescaleToGaussianDomain( model, detail::gaussianDomainVariance( table, componentValues ) ) );
    out->indexes[static_cast<std::size_t>( component )] = detail::NeighborIndex::build( componentSamples );
  }
  return {};
}

} // namespace

SgsResult sgs( const std::vector<Sample> &samples, const GridSpec &grid,
               const VariogramModel &model, const SgsParams &params,
               const Control &control )
{
  SgsResult result;
  if ( !grid.isValid() )
  {
    result.message = "grid is not valid";
    return result;
  }
  const std::int64_t cells64 = static_cast<std::int64_t>( grid.rows ) * grid.cols;
  if ( cells64 > 100'000'000 )
  {
    result.message = "grid budget exceeded (cells > 1e8)";
    return result;
  }
  // #468：组合内存闸——单轴预算挡不住乘积爆炸（64×1e8×8B ≈ 51GB）；
  // realizations 输出张量总字节超预算即拒绝（拒绝而非钳制，见
  // sgs_internal.h kMaxRealizationBytes）。此处用钳制后的 R（真正分配的量）。
  const int budgetRealizations = std::clamp( params.nRealizations, 1, 64 );
  if ( static_cast<std::int64_t>( budgetRealizations ) * cells64 * 8 >
       detail::kMaxRealizationBytes )
  {
    result.message = "realization memory budget exceeded (realizations x cells x 8B > 4 GiB)";
    return result;
  }
  // D1：nugget/sill 的 NaN 会穿过「< 0」「和 <= 0」比较（NaN 比较恒 false），
  // 进入方程组后全场解 NaN 且 Status 仍 Ok——与 kriging 的 modelUsable 同口径，
  // 入口显式拒绝非有限参数（azimuth/ratio 的 NaN 会让 γ(dx,dy) 塌缩成 0，
  // SK 协方差矩阵全同值必奇异，一并拒绝）。
  if ( !( model.range > 0 ) || !std::isfinite( model.range ) ||
       !std::isfinite( model.nugget ) || !std::isfinite( model.sill ) ||
       !std::isfinite( model.azimuthDeg ) || !std::isfinite( model.anisotropyRatio ) ||
       model.nugget < 0 || model.sill < 0 )
  {
    result.message = "variogram model invalid (finite range > 0, nugget/sill >= 0)";
    return result;
  }
  if ( model.nugget + model.sill <= 0 )
  {
    result.message = "zero total sill: SGS needs nugget + sill > 0";
    return result;
  }
  SgsParams clamped = params;
  clamped.nRealizations = std::clamp( clamped.nRealizations, 1, 64 );
  clamped.maxPoints = std::clamp( clamped.maxPoints, 1, 64 );
  if ( clamped.searchRadius < 0 )
    clamped.searchRadius = 0;

  // P3：dedupe 映射（屏障输入按原始样本序对齐，去重后映射到合并组）；
  // gaussianModel/index 仅为无屏障路径物化（有屏障时各分量自带）。
  std::vector<std::uint32_t> inputToDeduped;
  VariogramModel gaussianModel;
  detail::NeighborIndex index;

  const std::vector<Sample> deduped =
      detail::dedupeSamples( samples, &result.mergedDuplicates, &inputToDeduped );
  if ( deduped.empty() )
  {
    result.message = "no finite samples";
    return result;
  }
  std::vector<double> sampleValues;
  sampleValues.reserve( deduped.size() );
  for ( const Sample &sample : deduped )
    sampleValues.push_back( sample.value );
  const detail::NormalScoreTable table = detail::NormalScoreTable::build( sampleValues );
  result.sampleMean = table.sampleMean;
  result.sampleStd = table.sampleStd;

  // P3：硬屏障隔断——合法性检查与分量物化全部在 realizations 大数组分配
  // 之前完成；无屏障输入（std::nullopt）时下方数值路径与旧实现逐位一致。
  SgsPartition partition;
  const SgsBarrierInput *barrierInput = params.barrier ? &*params.barrier : nullptr;
  if ( barrierInput )
  {
    result.message = prepareSgsPartition( *barrierInput, grid, samples, deduped, inputToDeduped,
                                          model, clamped, &partition );
    if ( !result.message.empty() )
      return result; // InvalidInput（缺省 status），大数组未分配
    result.barrierCells = partition.barrierCellCount;
    result.componentCellCount.reserve( static_cast<std::size_t>( partition.components ) );
    result.componentSampleCount.reserve( static_cast<std::size_t>( partition.components ) );
    for ( int component = 0; component < partition.components; ++component )
    {
      result.componentCellCount.push_back( static_cast<int>( partition.cells[static_cast<std::size_t>( component )].size() ) );
      result.componentSampleCount.push_back( static_cast<int>( partition.sampleIdx[static_cast<std::size_t>( component )].size() ) );
    }
    result.consumedBarrierIds = barrierInput->consumedBarrierIds;
    result.barrierModel = barrierInput->barrierModel;
  }
  else
  {
    // #325：模型标定到正态得分域。原值域基台与该域方差差几个量级，直接用会
    // 让 sqrt(variance) 的随机振幅整体缩放错误。块金/拱高比保持不变。
    gaussianModel = regularizedForKriging(
        params.refitVariogramOnNormalScores
            ? detail::refitOnNormalScores( model, table, deduped )
            : detail::rescaleToGaussianDomain( model, detail::gaussianDomainVariance( table, sampleValues ) ) );

    // 静态样本邻域按索引原位取 deduped（detail::NeighborIndex 语义）。
    index = detail::NeighborIndex::build( deduped );
  }

  const std::size_t cells = static_cast<std::size_t>( cells64 );
  const int R = clamped.nRealizations;
  result.realizations.assign( static_cast<std::size_t>( R ),
                              std::vector<double>( cells,
                                                   std::numeric_limits<double>::quiet_NaN() ) );

  // #478：每实现一份派生随机流 + 独立工作缓冲——实现间零数据依赖，
  // R 个实现天然可并行（sgsfill.cpp 按带散布种子同口径：seed + r·黄金比
  // 常数，模 2⁶⁴ 确定回绕）。r=0 种子不变：实现 0 与旧共享流实现逐位一致。
  struct RealizationCounts
  {
    int finite = 0;
    int nodata = 0;
    int failures = 0;
  };
  std::vector<RealizationCounts> counts( static_cast<std::size_t>( R ) );

  const auto runRealization = [&]( int realization ) {
    std::mt19937_64 rng(
        detail::derivedStreamSeed( clamped.seed, static_cast<std::uint64_t>( realization ) ) );
    SimulatedLattice lattice;
    std::vector<std::uint32_t> staticNeighborhood;
    std::vector<CondPoint> simulatedNeighborhood;
    std::vector<CondPoint> neighborhood;
    std::vector<std::size_t> path( cells );
    detail::KrigingWorkspace krigeWorkspace; // #478：方程组缓冲跨格复用
    std::vector<double> gaussianField( cells, std::numeric_limits<double>::quiet_NaN() );
    RealizationCounts &tally = counts[static_cast<std::size_t>( realization )];

    // 随机路径：Fisher–Yates（先 shuffle 后逐格抽样）
    for ( std::size_t i = 0; i < cells; ++i )
      path[i] = i;
    for ( std::size_t i = cells; i > 1; --i )
    {
      const std::size_t j = static_cast<std::size_t>( rng() % i );
      std::swap( path[i - 1], path[j] );
    }
    lattice.reset( grid.cols, grid.rows );

    std::size_t sinceCheck = 0;
    const std::size_t checkInterval = 2048;
    for ( std::size_t step = 0; step < cells; ++step )
    {
      const std::size_t cell = path[step];
      const int row = static_cast<int>( cell / static_cast<std::size_t>( grid.cols ) );
      const int column = static_cast<int>( cell % static_cast<std::size_t>( grid.cols ) );
      const double x0 = grid.cellCenterX( column );
      const double y0 = grid.cellCenterY( row );

      index.queryNearest( x0, y0, clamped.maxPoints, clamped.searchRadius, &staticNeighborhood );
      lattice.queryNearest( column, row, grid, clamped.maxPoints, &simulatedNeighborhood );

      // 合并邻域：静态样本优先；与静态样本重合的已模拟点剔除——
      // 重合点的 C 矩阵两行完全相同（γ(0)=0），会让 LU 奇异，
      // 且条件值已由静态样本承载（信息等价）。
      neighborhood.clear();
      neighborhood.reserve( staticNeighborhood.size() + simulatedNeighborhood.size() );
      for ( std::uint32_t staticIndex : staticNeighborhood )
      {
        const Sample &sample = deduped[staticIndex];
        neighborhood.push_back( CondPoint{ sample.x, sample.y, 0.0, table.forward( sample.value ) } );
      }
      const std::size_t staticCount = neighborhood.size();
      for ( const CondPoint &simulated : simulatedNeighborhood )
      {
        bool coincidentWithStatic = false;
        for ( std::size_t i = 0; i < staticCount; ++i )
        {
          const double ex = 1e-9 * std::max( { 1.0, std::fabs( simulated.x ), std::fabs( neighborhood[i].x ) } );
          const double ey = 1e-9 * std::max( { 1.0, std::fabs( simulated.y ), std::fabs( neighborhood[i].y ) } );
          if ( std::fabs( simulated.x - neighborhood[i].x ) <= ex &&
               std::fabs( simulated.y - neighborhood[i].y ) <= ey )
          {
            coincidentWithStatic = true;
            break;
          }
        }
        if ( !coincidentWithStatic )
          neighborhood.push_back( simulated );
      }
      if ( clamped.searchRadius > 0 )
      {
        const double radius2 = clamped.searchRadius * clamped.searchRadius;
        neighborhood.erase( std::remove_if( neighborhood.begin(), neighborhood.end(),
                                            [x0, y0, radius2]( const CondPoint &point ) {
                                              const double dx = point.x - x0;
                                              const double dy = point.y - y0;
                                              return dx * dx + dy * dy > radius2;
                                            } ),
                            neighborhood.end() );
      }
      if ( static_cast<int>( neighborhood.size() ) > clamped.maxPoints )
      {
        // 距离并列（规则网格对称等距点）时 partial_sort 对等价类的选取是
        // 标准库实现定义（libstdc++/MSVC 漂移）——违背「同 seed 逐位可复现」
        // 契约（sgs.h 头注）。比较器加 (x,y) 字典序次键构成严格全序。
        std::partial_sort( neighborhood.begin(), neighborhood.begin() + clamped.maxPoints,
                           neighborhood.end(), [x0, y0]( const CondPoint &a, const CondPoint &b ) {
                             const double da = ( a.x - x0 ) * ( a.x - x0 ) + ( a.y - y0 ) * ( a.y - y0 );
                             const double db = ( b.x - x0 ) * ( b.x - x0 ) + ( b.y - y0 ) * ( b.y - y0 );
                             if ( da != db )
                               return da < db;
                             if ( a.x != b.x )
                               return a.x < b.x;
                             return a.y < b.y;
                           } );
        neighborhood.resize( static_cast<std::size_t>( clamped.maxPoints ) );
      }

      double estimate = 0;
      double variance = 0;
      // D5：查询格与静态样本重合（格心恰落井位）的显式特判——对齐 sgs3 主循环
      // 同款特例。SK 的数值路径在精确通过场景下残差为小正数时，sqrt(variance)
      // 会把 LU 舍入放大成 ~1e-7 高斯域抖动（井位读数不等于井值）；特判后方差
      // 恒 0。随机流照常推进（gaussianRandom 仍求值、乘 sqrt(0)），同 seed
      // 逐位可复现的黄金锚契约不受影响。
      double coincidentScore = 0;
      bool coincidentWithStatic = false;
      for ( std::uint32_t staticIndex : staticNeighborhood )
      {
        const Sample &sample = deduped[staticIndex];
        const double ex = 1e-9 * std::max( { 1.0, std::fabs( sample.x ), std::fabs( x0 ) } );
        const double ey = 1e-9 * std::max( { 1.0, std::fabs( sample.y ), std::fabs( y0 ) } );
        if ( std::fabs( sample.x - x0 ) <= ex && std::fabs( sample.y - y0 ) <= ey )
        {
          coincidentScore = table.forward( sample.value );
          coincidentWithStatic = true;
          break;
        }
      }
      bool solved = false;
      if ( coincidentWithStatic )
      {
        estimate = coincidentScore;
        variance = 0;
        solved = true;
      }
      else if ( !neighborhood.empty() &&
                detail::solveSimpleKriging( neighborhood, x0, y0, 0.0, gaussianModel, &estimate,
                                            &variance, &krigeWorkspace ) )
      {
        solved = true;
      }
      if ( solved )
      {
        const double draw = estimate + std::sqrt( variance ) * detail::gaussianRandom( rng );
        gaussianField[cell] = draw;
        lattice.setValue( column, row, draw );
      }
      else
      {
        ++tally.failures; // 无邻域或数值失败：该格保持 nodata（如实计数）
      }

      ++sinceCheck;
      if ( sinceCheck >= checkInterval )
      {
        sinceCheck = 0;
        // 并行档不设回调（见下），这里的轮询在并行档恒为空操作。
        if ( control.cancelled && control.cancelled() )
          return false;
        if ( control.progress )
          control.progress( ( static_cast<double>( realization ) + static_cast<double>( step + 1 ) / static_cast<double>( cells ) ) / static_cast<double>( R ) );
      }
    }

    // 反变换写输出；计数随实现产出一次写入（#478：不再在实现循环里清零
    // 重算——多实现的 nodata 模式可不同（随机路径不同），结果口径如实
    // 取最后一实现的计数，见 sgs.h 字段注释）。
    std::vector<double> &output = result.realizations[static_cast<std::size_t>( realization )];
    for ( std::size_t cell2 = 0; cell2 < cells; ++cell2 )
    {
      const double gaussian = gaussianField[cell2];
      if ( std::isfinite( gaussian ) )
      {
        output[cell2] = table.backTransform( gaussian );
        ++tally.finite;
      }
      else
      {
        ++tally.nodata;
      }
    }
    return true;
  };

  // P3：分量路径实现。逐句对齐上方 runRealization 的每格步进（合并邻域/
  // 重合剔除/半径过滤/K 截断全序比较器/重合特判/取消节奏），差异只有三处：
  // (1) 静态/动态两类邻域都在分量 c 内取（跨分量点不可见——不为凑邻居跨
  //     屏障借样本，也不借另一分量已模拟的格）；
  // (2) 正态得分/反变换用分量 c 自己的表与按分量方差标定的模型；
  // (3) 随机流按「实现 × 稳定分量号」派生（derivedStreamSeed 双重混合），
  //     路径只在分量格子集上洗牌——约束行序/线程调度不影响同一输入的结果。
  // 反事实保证（固定显式变差模型）：改变其他分量的样本值既不进本分量的
  // 条件点，也不进本分量的正态得分表——本分量实现逐位不变。
  const auto runPartitionedRealization = [&]( int realization ) {
    SimulatedLattice lattice;
    std::vector<std::uint32_t> staticNeighborhood;
    std::vector<CondPoint> simulatedNeighborhood;
    std::vector<CondPoint> neighborhood;
    std::vector<std::size_t> path;
    detail::KrigingWorkspace krigeWorkspace;
    std::vector<double> gaussianField( cells, std::numeric_limits<double>::quiet_NaN() );
    RealizationCounts &tally = counts[static_cast<std::size_t>( realization )];
    lattice.reset( grid.cols, grid.rows );
    std::size_t simulatedTotal = 0;
    std::size_t sinceCheck = 0;
    const std::size_t checkInterval = 2048;

    for ( int component = 0; component < partition.components; ++component )
    {
      const std::vector<std::size_t> &componentCells = partition.cells[static_cast<std::size_t>( component )];
      const detail::NormalScoreTable &compTable = partition.tables[static_cast<std::size_t>( component )];
      const VariogramModel &compModel = partition.models[static_cast<std::size_t>( component )];
      const detail::NeighborIndex &compIndex = partition.indexes[static_cast<std::size_t>( component )];
      const std::vector<std::uint32_t> &compSampleIdx = partition.sampleIdx[static_cast<std::size_t>( component )];
      std::mt19937_64 rng( detail::derivedStreamSeed(
          detail::derivedStreamSeed( clamped.seed, static_cast<std::uint64_t>( realization ) ),
          static_cast<std::uint64_t>( component ) + 1 ) );

      // 随机路径：分量格子集上的 Fisher–Yates（格子集升序 = 起始序确定）。
      path.resize( componentCells.size() );
      for ( std::size_t i = 0; i < componentCells.size(); ++i )
        path[i] = componentCells[i];
      for ( std::size_t i = componentCells.size(); i > 1; --i )
      {
        const std::size_t j = static_cast<std::size_t>( rng() % i );
        std::swap( path[i - 1], path[j] );
      }

      for ( std::size_t step = 0; step < componentCells.size(); ++step )
      {
        const std::size_t cell = path[step];
        const int row = static_cast<int>( cell / static_cast<std::size_t>( grid.cols ) );
        const int column = static_cast<int>( cell % static_cast<std::size_t>( grid.cols ) );
        const double x0 = grid.cellCenterX( column );
        const double y0 = grid.cellCenterY( row );

        compIndex.queryNearest( x0, y0, clamped.maxPoints, clamped.searchRadius, &staticNeighborhood );
        lattice.queryNearestInComponent( column, row, grid, clamped.maxPoints,
                                         barrierInput->cellComponent, component, &simulatedNeighborhood );

        neighborhood.clear();
        neighborhood.reserve( staticNeighborhood.size() + simulatedNeighborhood.size() );
        for ( std::uint32_t localIndex : staticNeighborhood )
        {
          const Sample &sample = deduped[compSampleIdx[localIndex]];
          neighborhood.push_back( CondPoint{ sample.x, sample.y, 0.0, compTable.forward( sample.value ) } );
        }
        const std::size_t staticCount = neighborhood.size();
        for ( const CondPoint &simulated : simulatedNeighborhood )
        {
          bool coincidentWithStatic = false;
          for ( std::size_t i = 0; i < staticCount; ++i )
          {
            const double ex = 1e-9 * std::max( { 1.0, std::fabs( simulated.x ), std::fabs( neighborhood[i].x ) } );
            const double ey = 1e-9 * std::max( { 1.0, std::fabs( simulated.y ), std::fabs( neighborhood[i].y ) } );
            if ( std::fabs( simulated.x - neighborhood[i].x ) <= ex &&
                 std::fabs( simulated.y - neighborhood[i].y ) <= ey )
            {
              coincidentWithStatic = true;
              break;
            }
          }
          if ( !coincidentWithStatic )
            neighborhood.push_back( simulated );
        }
        if ( clamped.searchRadius > 0 )
        {
          const double radius2 = clamped.searchRadius * clamped.searchRadius;
          neighborhood.erase( std::remove_if( neighborhood.begin(), neighborhood.end(),
                                              [x0, y0, radius2]( const CondPoint &point ) {
                                                const double dx = point.x - x0;
                                                const double dy = point.y - y0;
                                                return dx * dx + dy * dy > radius2;
                                              } ),
                              neighborhood.end() );
        }
        if ( static_cast<int>( neighborhood.size() ) > clamped.maxPoints )
        {
          std::partial_sort( neighborhood.begin(), neighborhood.begin() + clamped.maxPoints,
                             neighborhood.end(), [x0, y0]( const CondPoint &a, const CondPoint &b ) {
                               const double da = ( a.x - x0 ) * ( a.x - x0 ) + ( a.y - y0 ) * ( a.y - y0 );
                               const double db = ( b.x - x0 ) * ( b.x - x0 ) + ( b.y - y0 ) * ( b.y - y0 );
                               if ( da != db )
                                 return da < db;
                               if ( a.x != b.x )
                                 return a.x < b.x;
                               return a.y < b.y;
                             } );
          neighborhood.resize( static_cast<std::size_t>( clamped.maxPoints ) );
        }

        double estimate = 0;
        double variance = 0;
        double coincidentScore = 0;
        bool coincidentWithStatic = false;
        for ( std::uint32_t localIndex : staticNeighborhood )
        {
          const Sample &sample = deduped[compSampleIdx[localIndex]];
          const double ex = 1e-9 * std::max( { 1.0, std::fabs( sample.x ), std::fabs( x0 ) } );
          const double ey = 1e-9 * std::max( { 1.0, std::fabs( sample.y ), std::fabs( y0 ) } );
          if ( std::fabs( sample.x - x0 ) <= ex && std::fabs( sample.y - y0 ) <= ey )
          {
            coincidentScore = compTable.forward( sample.value );
            coincidentWithStatic = true;
            break;
          }
        }
        bool solved = false;
        if ( coincidentWithStatic )
        {
          estimate = coincidentScore;
          variance = 0;
          solved = true;
        }
        else if ( !neighborhood.empty() &&
                  detail::solveSimpleKriging( neighborhood, x0, y0, 0.0, compModel, &estimate,
                                              &variance, &krigeWorkspace ) )
        {
          solved = true;
        }
        if ( solved )
        {
          const double draw = estimate + std::sqrt( variance ) * detail::gaussianRandom( rng );
          gaussianField[cell] = draw;
          lattice.setValue( column, row, draw );
        }
        else
        {
          ++tally.failures;
        }
        ++simulatedTotal;

        ++sinceCheck;
        if ( sinceCheck >= checkInterval )
        {
          sinceCheck = 0;
          if ( control.cancelled && control.cancelled() )
            return false;
          if ( control.progress )
            control.progress( ( static_cast<double>( realization ) +
                                static_cast<double>( simulatedTotal ) / static_cast<double>( cells ) ) /
                              static_cast<double>( R ) );
        }
      }
    }

    // 反变换用各格所属分量的表；屏障/域外格（<0）保持 nodata。
    std::vector<double> &output = result.realizations[static_cast<std::size_t>( realization )];
    for ( std::size_t cell2 = 0; cell2 < cells; ++cell2 )
    {
      const int component = barrierInput->cellComponent[cell2];
      const double gaussian = gaussianField[cell2];
      if ( component >= 0 && std::isfinite( gaussian ) )
      {
        output[cell2] = partition.tables[static_cast<std::size_t>( component )].backTransform( gaussian );
        ++tally.finite;
      }
      else
      {
        ++tally.nodata;
      }
    }
    return true;
  };

  const auto runOne = [&]( int realization ) {
    return barrierInput ? runPartitionedRealization( realization ) : runRealization( realization );
  };

  const bool hasCallbacks = control.cancelled || control.progress;
  if ( R > 1 && !hasCallbacks )
  {
    // #478：实现级并行——输出行不相交、每实现流/缓冲私有，无需同步。
    // 回调在场时不并行——Control 回调无线程安全契约，取消/进度仍走顺序档。
    // FB-002：每实现工作区 = path + gaussianField + lattice.values ≈ 3·cells·8B
    //（#468 输出闸只看张量，此处把 workers × 工作区计入总峰值并按预算收缩；
    // worker 数是资源面，收缩不动成果语义——串行档=并行化之前的既有行为）。
    const std::int64_t workspacePerWorker = cells64 * 3 * 8;
    const std::int64_t outputBytes = static_cast<std::int64_t>( R ) * cells64 * 8;
    const int workers =
        detail::workersForPeakBudget( std::min<int>( R, 4 ), outputBytes, workspacePerWorker );
    // PW-082：worker 内异常（bad_alloc 等）逸出线程函数 = std::terminate，
    // 主线程异常时未 join 的线程析构同样 terminate。每 worker 捕获存
    // exception_ptr，全部 join 后重抛第一个。
    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> errors( static_cast<std::size_t>( workers ) );
    threads.reserve( static_cast<std::size_t>( workers ) - 1 );
    const auto runStripe = [&runOne, &errors, workers, R]( int worker ) {
      try
      {
        for ( int realization = worker; realization < R; realization += workers )
          runOne( realization );
      }
      catch ( ... )
      {
        errors[static_cast<std::size_t>( worker )] = std::current_exception();
      }
    };
    // review L4：建线程失败（资源耗尽 std::system_error）时已建线程仍 joinable，
    // 异常逸出 = 其析构 std::terminate。失败即停止派生，未派出的条带由调用
    // 线程顺序补跑——成果语义不变，只是并行度降级。
    int launched = 1;
    try
    {
      for ( ; launched < workers; ++launched )
        threads.emplace_back( runStripe, launched );
    }
    catch ( ... )
    {
    }
    runStripe( 0 );
    for ( int worker = launched; worker < workers; ++worker )
      runStripe( worker );
    for ( std::thread &thread : threads )
      thread.join();
    for ( const std::exception_ptr &error : errors )
      if ( error )
        std::rethrow_exception( error );
  }
  else
  {
    for ( int realization = 0; realization < R; ++realization )
    {
      if ( !runOne( realization ) )
      {
        result.status = Status::Cancelled;
        result.message = "cancelled";
        return result;
      }
    }
  }
  for ( const RealizationCounts &tally : counts )
    result.solverFailures += tally.failures; // 跨实现累计（与既有口径一致）
  result.finiteCells = counts.back().finite;
  result.nodataCells = counts.back().nodata;
  if ( control.progress )
    control.progress( 1.0 );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::geostat
