// 层：数据
#pragma once

#include "linsolve.h"
#include "types.h"
#include "variogram.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

// geostat/sgs_internal — 序贯高斯模拟的共享机件（kriging.cpp/sgs.cpp 之外的
// 非公开内面，sgs3.cpp 与 sgs.cpp 共用）。2D 入口（sgs.cpp）与三维点集入口
// （sgs3.cpp）走同一套：正态得分变换、Box-Muller mt19937_64 随机流、简单克里金
// 求解、分位反变换。这里抽出来的每一段都必须在 2D 路径上算术恒等——dz=0 精确
// 走 VariogramModel 的 2D 半变差重载，2D 逐位结果不变。
// 层：数据
namespace paleo::geostat
{
namespace detail
{

// 条件点（高斯域）。2D 消费方 z 恒 0。
struct CondPoint
{
  double x = 0;
  double y = 0;
  double z = 0;
  double value = 0;
};

// 确定性 (0,1) 均匀：mt19937_64 原始 u64 → 53bit 尾数。
inline double unitRandom( std::mt19937_64 &rng )
{
  return ( static_cast<double>( rng() >> 11 ) + 0.5 ) * 0x1.0p-53;
}

inline double gaussianRandom( std::mt19937_64 &rng )
{
  // Box-Muller（cos 分支；u1 ∈ (0,1) 保证 log 有限）
  const double u1 = unitRandom( rng );
  const double u2 = unitRandom( rng );
  return std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 2.0 * std::numbers::pi * u2 );
}

// Φ⁻¹：erf 二分（erf 严格单调、std 实现可移植；100 次迭代到双精度
// 饱和。建表每样本只调一次，成本可忽略。不用有理逼近——系数手打
// 易错，尾部分支爆过 e300（见 ledger 轮3 纠错记录）。
inline double standardNormalQuantile( double p )
{
  const double target = 2.0 * std::clamp( p, 1e-12, 1.0 - 1e-12 ) - 1.0;
  double lo = -9.0; // erf(±9) = ±1 − <1e-17，覆盖双精度全域
  double hi = 9.0;
  for ( int iteration = 0; iteration < 100; ++iteration )
  {
    const double mid = 0.5 * ( lo + hi );
    if ( std::erf( mid ) < target )
      lo = mid;
    else
      hi = mid;
  }
  return std::numbers::sqrt2 * 0.5 * ( lo + hi );
}

// 正态得分变换：排序 → (i+0.5)/n 分位 → Φ⁻¹。同值并列取平均分位。
// 输入是值数组（调用方按采样序收集）——sum/sumSq 的累加序与历史实现一致，
// 数值恒等。
struct NormalScoreTable
{
  std::vector<double> z; // 升序原值（并列合组）
  std::vector<double> y; // 对应高斯得分（升序）
  double sampleMean = 0;
  double sampleStd = 0;

  static NormalScoreTable build( const std::vector<double> &values )
  {
    NormalScoreTable table;
    double sum = 0;
    double sumSq = 0;
    for ( const double value : values )
    {
      sum += value;
      sumSq += value * value;
    }
    std::vector<double> sorted = values;
    std::sort( sorted.begin(), sorted.end() );
    const double n = static_cast<double>( sorted.size() );
    table.sampleMean = sum / n;
    table.sampleStd = std::sqrt( std::max( 0.0, sumSq / n - table.sampleMean * table.sampleMean ) );
    std::size_t i = 0;
    while ( i < sorted.size() )
    {
      std::size_t j = i;
      while ( j + 1 < sorted.size() &&
              sorted[j + 1] - sorted[i] <= 1e-12 * std::max( 1.0, std::fabs( sorted[i] ) ) )
        ++j;
      double rankSum = 0;
      for ( std::size_t k = i; k <= j; ++k )
        rankSum += ( static_cast<double>( k ) + 0.5 );
      const double p = rankSum / static_cast<double>( j - i + 1 ) / n;
      table.z.push_back( sorted[i] );
      table.y.push_back( standardNormalQuantile( std::clamp( p, 1e-9, 1.0 - 1e-9 ) ) );
      i = j + 1;
    }
    return table;
  }

  // 样本值 → 高斯得分（build 的样本必命中某组；越界值线性外推兜底）。
  double forward( double value ) const
  {
    if ( z.size() == 1 )
      return y.front();
    if ( value <= z.front() )
    {
      const double slope = ( y[1] - y[0] ) / std::max( z[1] - z[0], 1e-12 );
      return y[0] + slope * ( value - z.front() );
    }
    if ( value >= z.back() )
    {
      const std::size_t m = z.size();
      const double slope = ( y[m - 1] - y[m - 2] ) / std::max( z[m - 1] - z[m - 2], 1e-12 );
      return y[m - 1] + slope * ( value - z.back() );
    }
    const std::size_t k = static_cast<std::size_t>(
        std::upper_bound( z.begin(), z.end(), value ) - z.begin() ) - 1;
    const double t = ( value - z[k] ) / std::max( z[k + 1] - z[k], 1e-12 );
    return y[k] + t * ( y[k + 1] - y[k] );
  }

  // 反变换：线性内插 + 端部按末段斜率线性外推（直方图忠实样本分布）。
  double backTransform( double gaussian ) const
  {
    if ( z.size() == 1 )
      return z.front(); // 单样本退化：任何高斯值都映回该值（如实退化）
    if ( gaussian <= y.front() )
    {
      const double slope = ( z[1] - z[0] ) / std::max( y[1] - y[0], 1e-12 );
      return z[0] + slope * ( gaussian - y.front() );
    }
    if ( gaussian >= y.back() )
    {
      const std::size_t m = z.size();
      const double slope = ( z[m - 1] - z[m - 2] ) / std::max( y[m - 1] - y[m - 2], 1e-12 );
      return z[m - 1] + slope * ( gaussian - y.back() );
    }
    const std::size_t k = static_cast<std::size_t>(
        std::upper_bound( y.begin(), y.end(), gaussian ) - y.begin() ) - 1;
    const double t = ( gaussian - y[k] ) / std::max( y[k + 1] - y[k], 1e-12 );
    return z[k] + t * ( z[k + 1] - z[k] );
  }
};

// 简单克里金（高斯域，均值 0 已知）：C(h) = S − γ(h)，解 Cw = c0。
// 估计 = Σwᵢyᵢ，方差 = S − Σwᵢc0ᵢ。三维坐标；dz=0 时 γ 走 2D 重载（恒等）。
//
// #325：SGS 在这里跑的是正态得分域，模型必须也标定到该域。正态得分后方差
// 约 1，原值域基台差几个量级；条件方差 = S − Σwᵢc0ᵢ 用错域时，权重对整体
// 缩放不敏感（比值不变），但 sqrt(variance) 的随机振幅整体缩放错误——基台
// 远小于 1 时实现几乎没有随机性，远大于 1 时反变换把模拟值甩出样本直方图。
// 所以调用方应传 rescaleToGaussianDomain 之后的模型，而不是原值域模型。
inline VariogramModel rescaleToGaussianDomain( const VariogramModel &model,
                                               double targetVariance )
{
  const double sill = model.nugget + model.sill;
  if ( !( sill > 0.0 ) || !( targetVariance > 0.0 ) )
    return model;
  const double factor = targetVariance / sill;
  VariogramModel out = model;
  out.nugget = model.nugget * factor;
  out.sill = model.sill * factor;
  return out;
}

// 高斯域样本方差：正态得分理论上为 1，按变换后的样本实测（端部线性外推会
// 带来小漂移，用实测值标定更稳）。
inline double gaussianDomainVariance( const NormalScoreTable &table,
                                      const std::vector<double> &values )
{
  if ( values.empty() )
    return 1.0;
  double sum = 0;
  double sumSq = 0;
  for ( const double value : values )
  {
    const double g = table.forward( value );
    sum += g;
    sumSq += g * g;
  }
  const double n = static_cast<double>( values.size() );
  const double mean = sum / n;
  const double variance = sumSq / n - mean * mean;
  return variance > 0.0 ? variance : 1.0;
}

inline bool solveSimpleKriging( const std::vector<CondPoint> &data, double x0, double y0,
                                double z0, const VariogramModel &model, double *estimate,
                                double *variance )
{
  const int n = static_cast<int>( data.size() );
  if ( n <= 0 )
    return false;
  const double totalSill = model.nugget + model.sill;
  std::vector<double> a( static_cast<std::size_t>( n ) * n );
  std::vector<double> b( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
  {
    for ( int j = i; j < n; ++j )
    {
      if ( i == j )
      {
        a[static_cast<std::size_t>( i ) * n + i] = totalSill; // C(0) = S
        continue;
      }
      const double gamma = model.semivariance(
          data[static_cast<std::size_t>( i )].x - data[static_cast<std::size_t>( j )].x,
          data[static_cast<std::size_t>( i )].y - data[static_cast<std::size_t>( j )].y,
          data[static_cast<std::size_t>( i )].z - data[static_cast<std::size_t>( j )].z );
      const double covariance = totalSill - gamma;
      a[static_cast<std::size_t>( i ) * n + j] = covariance;
      a[static_cast<std::size_t>( j ) * n + i] = covariance;
    }
    b[static_cast<std::size_t>( i )] =
        totalSill - model.semivariance( data[static_cast<std::size_t>( i )].x - x0,
                                        data[static_cast<std::size_t>( i )].y - y0,
                                        data[static_cast<std::size_t>( i )].z - z0 );
  }
  std::vector<double> weights;
  if ( !solveDenseLu( a, n, b, weights ) )
    return false;
  double est = 0;
  double var = totalSill;
  for ( int i = 0; i < n; ++i )
  {
    est += weights[static_cast<std::size_t>( i )] * data[static_cast<std::size_t>( i )].value;
    var -= weights[static_cast<std::size_t>( i )] * b[static_cast<std::size_t>( i )];
  }
  if ( !std::isfinite( est ) || !std::isfinite( var ) )
    return false;
  if ( var < 0 )
  {
    if ( var < -1e-9 * std::max( 1.0, totalSill ) )
      return false;
    var = 0;
  }
  *estimate = est;
  *variance = var;
  return true;
}

} // namespace detail
} // namespace paleo::geostat
