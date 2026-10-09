// 层：数据
#include "variogram.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

// 层：数据
namespace paleo::geostat
{

double VariogramModel::semivariance( double h ) const
{
  if ( !( h > 0 ) )
    return 0; // γ(0) = 0：对角元与重合点口径，块金是 h→0⁺ 的跳变
  if ( !( range > 0 ) )
    return nugget + sill;
  const double a = h / range;
  double shape = 1;
  switch ( type )
  {
    case VariogramModelType::Spherical:
      if ( a < 1 )
        shape = 1.5 * a - 0.5 * a * a * a;
      else
        shape = 1;
      break;
    case VariogramModelType::Exponential:
      shape = 1 - std::exp( -3.0 * a );
      break;
    case VariogramModelType::Gaussian:
      shape = 1 - std::exp( -3.0 * a * a );
      break;
  }
  return nugget + sill * shape;
}

double VariogramModel::semivariance( double dx, double dy ) const
{
  if ( !( anisotropyRatio > 1.0 ) )
    return semivariance( std::hypot( dx, dy ) );
  const double azimuthRad = azimuthDeg * std::numbers::pi / 180.0;
  const double sinA = std::sin( azimuthRad );
  const double cosA = std::cos( azimuthRad );
  const double along = dx * sinA + dy * cosA; // 走向分量（长变程）
  const double across = -dx * cosA + dy * sinA; // 垂直分量（短变程）
  return semivariance( std::hypot( along, across * anisotropyRatio ) );
}

double VariogramModel::semivariance( double dx, double dy, double dz ) const
{
  if ( dz == 0.0 )
    return semivariance( dx, dy ); // 2D 消费方逐位不变（z 恒 0 → dz 恒 +0.0）
  const double zScale = verticalRangeRatio > 1.0 ? verticalRangeRatio : 1.0;
  if ( !( anisotropyRatio > 1.0 ) )
    return semivariance( std::hypot( std::hypot( dx, dy ), dz * zScale ) );
  const double azimuthRad = azimuthDeg * std::numbers::pi / 180.0;
  const double sinA = std::sin( azimuthRad );
  const double cosA = std::cos( azimuthRad );
  const double along = dx * sinA + dy * cosA;
  const double across = -dx * cosA + dy * sinA;
  return semivariance( std::hypot( std::hypot( along, across * anisotropyRatio ),
                                   dz * zScale ) );
}

ExperimentalVariogram experimentalVariogram( const std::vector<Sample> &samples,
    double lag, int nLags, const VariogramDirection &direction, const VariogramBarriers &barriers,
    const Control &control )
{
  ExperimentalVariogram result;
  result.lag = lag;
  result.nLags = nLags;
  if ( !( lag > 0 ) || nLags <= 0 )
  {
    result.message = "lag must be > 0 and nLags > 0";
    return result;
  }
  std::size_t usable = 0;
  for ( const Sample &sample : samples )
  {
    if ( std::isfinite( sample.x ) && std::isfinite( sample.y ) && std::isfinite( sample.value ) )
      ++usable;
  }
  if ( usable < 2 )
  {
    result.message = "need at least 2 finite samples";
    return result;
  }

  // 隔断感知档：每样本一个绕障测地距离场（faultpath 核），对的滞后距取场值，
  // 不可达（NaN）= 跨隔断，不进累积。关或无隔断 → barrierTrack 为空，逐位走
  // 既有欧氏距代码路径。
  constexpr std::size_t kBarrierSampleLimit = 512;
  const bool barrierTrack = barriers.enabled && !barriers.polygons.empty();
  GridSpec barrierGrid;
  std::vector<std::vector<double>> barrierFields; // 有限样本序的第 i 个距离场
  if ( barrierTrack )
  {
    if ( samples.size() > kBarrierSampleLimit )
    {
      result.message = "barrier-aware variogram supports at most 512 samples";
      return result;
    }
    // 栅格化格网：覆盖有限样本范围 + 一格余量；分辨率沿最长边。
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for ( const Sample &sample : samples )
    {
      if ( !std::isfinite( sample.x ) || !std::isfinite( sample.y ) )
        continue;
      minX = std::min( minX, sample.x );
      minY = std::min( minY, sample.y );
      maxX = std::max( maxX, sample.x );
      maxY = std::max( maxY, sample.y );
    }
    int resolution = barriers.gridResolution > 0 ? barriers.gridResolution : 256;
    resolution = std::clamp( resolution, 32, 1024 );
    const double span = std::max( maxX - minX, maxY - minY );
    double cellSize = span > 0 ? span / resolution : 1.0;
    const auto cellsAlong = [cellSize]( double extent ) {
      return std::max( 1, static_cast<int>( std::ceil( extent / cellSize ) ) );
    };
    barrierGrid.originX = minX - cellSize;
    barrierGrid.originY = maxY + cellSize; // north-up：行向南
    barrierGrid.pixelWidth = cellSize;
    barrierGrid.pixelHeight = -cellSize;
    barrierGrid.cols = cellsAlong( maxX - minX ) + 2;
    barrierGrid.rows = cellsAlong( maxY - minY ) + 2;
    // 总单元预算闸（分辨率参数与长宽比组合出的极端格网不无限放大）。
    constexpr std::int64_t kCellBudget = 4'000'000;
    if ( static_cast<std::int64_t>( barrierGrid.rows ) * barrierGrid.cols > kCellBudget )
    {
      const double scale = std::sqrt(
          static_cast<double>( kCellBudget ) /
          static_cast<double>( static_cast<std::int64_t>( barrierGrid.rows ) * barrierGrid.cols ) );
      barrierGrid.cols = std::max( 2, static_cast<int>( barrierGrid.cols * scale ) );
      barrierGrid.rows = std::max( 2, static_cast<int>( barrierGrid.rows * scale ) );
    }
    barrierFields.reserve( samples.size() );
    for ( const Sample &sample : samples )
    {
      // #327：隔断档每个样本一次全网格 Dijkstra，512 样本上限下这段是长任务。
      // 取消必须能在这里打断，否则要等拟合结束，evaluateLocalKriging 才会看到。
      if ( control.cancelled && control.cancelled() )
      {
        result.status = Status::Cancelled;
        result.message = "cancelled";
        return result;
      }
      if ( !std::isfinite( sample.x ) || !std::isfinite( sample.y ) ||
           !std::isfinite( sample.value ) )
      {
        barrierFields.emplace_back(); // 占位：与非有限样本对本来就不累积
        continue;
      }
      const FaultPathResult field =
          faultPathMetric( barrierGrid, barriers.polygons, sample.x, sample.y, control );
      if ( field.status != Status::Ok )
      {
        result.message = "barrier distance field failed: " + field.message;
        return result;
      }
      barrierFields.push_back( std::move( field.distance ) );
    }
    result.barrierAware = true;
  }

  const bool omnidirectional = direction.omnidirectional || direction.toleranceDeg >= 90;
  double azimuthRad = 0;
  double cosTolerance = 0;
  if ( !omnidirectional )
  {
    // 从北顺时针 → 方向单位向量 (sin az, cos az)（x 东 y 北）
    azimuthRad = direction.azimuthDeg * std::numbers::pi / 180.0;
    const double toleranceRad = direction.toleranceDeg * std::numbers::pi / 180.0;
    cosTolerance = std::cos( toleranceRad );
  }

  result.lagDistance.assign( static_cast<std::size_t>( nLags ), 0 );
  result.semivariance.assign( static_cast<std::size_t>( nLags ), 0 );
  result.pairCount.assign( static_cast<std::size_t>( nLags ), 0 );
  std::vector<double> sumH( static_cast<std::size_t>( nLags ), 0 );
  std::vector<double> sumGamma( static_cast<std::size_t>( nLags ), 0 );

  for ( std::size_t i = 0; i + 1 < samples.size(); ++i )
  {
    const Sample &a = samples[i];
    if ( !std::isfinite( a.x ) || !std::isfinite( a.y ) || !std::isfinite( a.value ) )
      continue;
    const std::vector<double> *fieldOfA =
        barrierTrack ? &barrierFields[i] : nullptr;
    for ( std::size_t j = i + 1; j < samples.size(); ++j )
    {
      const Sample &b = samples[j];
      if ( !std::isfinite( b.x ) || !std::isfinite( b.y ) || !std::isfinite( b.value ) )
        continue;
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double hEuclid = std::hypot( dx, dy );
      if ( !( hEuclid > 0 ) )
        continue; // 重合点对：γ 由重合均值承担，跳过（与 γ(0)=0 口径一致）
      double h = hEuclid;
      if ( fieldOfA )
      {
        // 测地滞后距：场在 b 所在格的值（方向过滤仍按欧氏位移与欧氏距）。
        const int column = static_cast<int>( std::floor( ( b.x - barrierGrid.originX ) /
                                                         barrierGrid.pixelWidth ) );
        const int row = static_cast<int>( std::floor( ( b.y - barrierGrid.originY ) /
                                                      barrierGrid.pixelHeight ) );
        if ( column < 0 || column >= barrierGrid.cols || row < 0 || row >= barrierGrid.rows )
        {
          ++result.unreachablePairs;
          continue; // 样本落在格网外（理论不可能：格网覆盖样本范围）——如实跳过
        }
        const double geodesic = ( *fieldOfA )[static_cast<std::size_t>( row ) * barrierGrid.cols +
                                              static_cast<std::size_t>( column )];
        if ( !std::isfinite( geodesic ) )
        {
          ++result.unreachablePairs;
          continue; // 跨隔断（不同连通域）：不进累积
        }
        h = geodesic;
        if ( !( h > 0 ) )
          continue; // 同格不同点（测地 0）：按 γ(0) 口径跳过
      }
      const int lagIndex = static_cast<int>( h / lag );
      if ( lagIndex >= nLags )
        continue;
      if ( !omnidirectional )
      {
        // |cos| 处理 h 与 -h 的对称：对方向与反方向都计入
        const double cosine =
            std::fabs( dx * std::sin( azimuthRad ) + dy * std::cos( azimuthRad ) ) / hEuclid;
        if ( cosine < cosTolerance )
          continue;
      }
      const std::size_t bin = static_cast<std::size_t>( lagIndex );
      const double half = 0.5 * ( a.value - b.value ) * ( a.value - b.value );
      sumH[bin] += h;
      sumGamma[bin] += half;
      result.pairCount[bin] += 1;
    }
  }
  for ( int bin = 0; bin < nLags; ++bin )
  {
    const std::size_t index = static_cast<std::size_t>( bin );
    const int count = result.pairCount[index];
    if ( count > 0 )
    {
      result.lagDistance[index] = sumH[index] / count;
      result.semivariance[index] = sumGamma[index] / count;
    }
  }
  result.status = Status::Ok;
  return result;
}

namespace
{

struct FitAccumulator
{
  const std::vector<double> *h = nullptr;
  const std::vector<double> *gamma = nullptr;
  std::vector<std::size_t> index; // pairCount > 0 的 lag

  double meanGamma() const
  {
    double sum = 0;
    for ( std::size_t k : index )
      sum += ( *gamma )[k];
    return index.empty() ? 0 : sum / static_cast<double>( index.size() );
  }

  // 固定 range 下解 (nugget, sill) 的线性最小二乘，非负钳制。
  // 设计矩阵列 [1, g(h/range)]，2×2 正规方程手展开。
  void solveLinear( VariogramModelType type, double range, double *nugget, double *sill ) const
  {
    double sumG = 0, sumGG = 0, sumY = 0, sumGY = 0;
    for ( std::size_t k : index )
    {
      const double g = shapeFor( type, ( *h )[k] / range );
      const double y = ( *gamma )[k];
      sumG += g;
      sumGG += g * g;
      sumY += y;
      sumGY += g * y;
    }
    const double n = static_cast<double>( index.size() );
    const double det = n * sumGG - sumG * sumG;
    if ( std::fabs( det ) < 1e-12 * std::max( 1.0, n * sumGG ) )
    {
      *nugget = meanGamma();
      *sill = 0;
      return;
    }
    double nug = ( sumY * sumGG - sumGY * sumG ) / det;
    double sil = ( n * sumGY - sumG * sumY ) / det;
    if ( sil < 0 ) // 拱高非负钳制：退化为纯块金
    {
      sil = 0;
      nug = meanGamma();
    }
    if ( nug < 0 )
      nug = 0;
    *nugget = nug;
    *sill = sil;
  }

  double sse( VariogramModelType type, double range, double nugget, double sill ) const
  {
    double sum = 0;
    for ( std::size_t k : index )
    {
      const double residual = ( *gamma )[k] - ( nugget + sill * shapeFor( type, ( *h )[k] / range ) );
      sum += residual * residual;
    }
    return sum;
  }

  static double shapeFor( VariogramModelType type, double a )
  {
    switch ( type )
    {
      case VariogramModelType::Spherical:
        return a < 1 ? ( 1.5 * a - 0.5 * a * a * a ) : 1;
      case VariogramModelType::Exponential:
        return 1 - std::exp( -3.0 * a );
      case VariogramModelType::Gaussian:
        return 1 - std::exp( -3.0 * a * a );
    }
    return 1;
  }
};

} // namespace

VariogramFit fitVariogram( const ExperimentalVariogram &experimental, VariogramModelType type )
{
  VariogramFit fit;
  if ( experimental.status != Status::Ok )
  {
    fit.message = "experimental variogram not ok: " + experimental.message;
    return fit;
  }
  fit.usedLags = 0;
  double hMin = std::numeric_limits<double>::max();
  double hMax = 0;
  double gammaScale = 0;
  for ( int k = 0; k < experimental.nLags; ++k )
  {
    const std::size_t index = static_cast<std::size_t>( k );
    if ( experimental.pairCount[index] <= 0 )
      continue;
    const double h = experimental.lagDistance[index];
    hMin = std::min( hMin, h );
    hMax = std::max( hMax, h );
    gammaScale = std::max( gammaScale, experimental.semivariance[index] );
    ++fit.usedLags;
  }
  if ( fit.usedLags < 2 )
  {
    fit.message = "need at least 2 lags with pairs";
    return fit;
  }
  if ( gammaScale <= 0 )
  {
    // 恒定场：全 0 半方差，零信号 → 零参数模型（如实，不虚构变程）
    fit.model.type = type;
    fit.model.nugget = 0;
    fit.model.sill = 0;
    fit.model.range = 1;
    fit.rmse = 0;
    fit.r2 = 0; // SST = 0，R² 无定义，报 0
    fit.status = Status::Ok;
    return fit;
  }

  FitAccumulator acc;
  acc.h = &experimental.lagDistance;
  acc.gamma = &experimental.semivariance;
  for ( int k = 0; k < experimental.nLags; ++k )
  {
    if ( experimental.pairCount[static_cast<std::size_t>( k )] > 0 )
      acc.index.push_back( static_cast<std::size_t>( k ) );
  }

  const double lo = std::max( hMin * 0.25, hMin + 1e-9 );
  const double hi = hMax * 4.0;
  double bestRange = lo;
  double bestNugget = 0;
  double bestSill = 0;
  double bestSse = std::numeric_limits<double>::max();
  const int coarseSteps = 121;
  for ( int step = 0; step <= coarseSteps; ++step )
  {
    const double t = static_cast<double>( step ) / coarseSteps;
    const double range = std::exp( std::log( lo ) * ( 1 - t ) + std::log( hi ) * t );
    double nugget = 0, sill = 0;
    acc.solveLinear( type, range, &nugget, &sill );
    const double sse = acc.sse( type, range, nugget, sill );
    if ( sse < bestSse )
    {
      bestSse = sse;
      bestRange = range;
      bestNugget = nugget;
      bestSill = sill;
    }
  }
  // 最优档附近局部细化（±1 档的 1/16 步长）
  const double coarseRatio = std::pow( hi / lo, 1.0 / coarseSteps );
  for ( int step = 0; step <= 32; ++step )
  {
    const double t = static_cast<double>( step ) / 32;
    const double range = bestRange * std::pow( coarseRatio, 2.0 * ( t - 0.5 ) );
    if ( !( range > 0 ) )
      continue;
    double nugget = 0, sill = 0;
    acc.solveLinear( type, range, &nugget, &sill );
    const double sse = acc.sse( type, range, nugget, sill );
    if ( sse < bestSse )
    {
      bestSse = sse;
      bestRange = range;
      bestNugget = nugget;
      bestSill = sill;
    }
  }

  fit.model.type = type;
  fit.model.nugget = bestNugget;
  fit.model.sill = bestSill;
  fit.model.range = bestRange;
  const double n = static_cast<double>( fit.usedLags );
  fit.rmse = std::sqrt( bestSse / n );
  const double meanGamma = acc.meanGamma();
  double sst = 0;
  for ( std::size_t k : acc.index )
  {
    const double d = experimental.semivariance[k] - meanGamma;
    sst += d * d;
  }
  fit.r2 = sst > 0 ? 1.0 - bestSse / sst : 0;
  fit.status = Status::Ok;
  return fit;
}

} // namespace paleo::geostat
