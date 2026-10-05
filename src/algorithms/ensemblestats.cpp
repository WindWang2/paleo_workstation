// 层：数据
#include "ensemblestats.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::ensemble
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

double quantileOf( const double *values, std::size_t n, double q )
{
  if ( !values || n == 0 )
    return kNaN;
  std::vector<double> finite;
  finite.reserve( n );
  for ( std::size_t i = 0; i < n; ++i )
    if ( std::isfinite( values[i] ) )
      finite.push_back( values[i] );
  if ( finite.empty() )
    return kNaN;
  std::sort( finite.begin(), finite.end() );
  q = std::clamp( q, 0.0, 1.0 );
  const double h = q * static_cast<double>( finite.size() - 1 );
  const std::size_t lo = static_cast<std::size_t>( std::floor( h ) );
  const std::size_t hi = static_cast<std::size_t>( std::ceil( h ) );
  if ( lo == hi )
    return finite[lo];
  const double frac = h - static_cast<double>( lo );
  return finite[lo] + ( finite[hi] - finite[lo] ) * frac;
}

void StreamingMoments::reset( std::size_t cells )
{
  m_cells = cells;
  m_fields = 0;
  m_sum.assign( cells, 0.0 );
  m_sumsq.assign( cells, 0.0 );
  m_n.assign( cells, 0 );
}

void StreamingMoments::addField( const double *values )
{
  if ( !values )
    return;
  for ( std::size_t i = 0; i < m_cells; ++i )
  {
    const double v = values[i];
    if ( !std::isfinite( v ) )
      continue;
    m_sum[i] += v;
    m_sumsq[i] += v * v;
    ++m_n[i];
  }
  ++m_fields;
}

std::vector<double> StreamingMoments::mean() const
{
  std::vector<double> out( m_cells, kNaN );
  for ( std::size_t i = 0; i < m_cells; ++i )
    if ( m_n[i] > 0 )
      out[i] = m_sum[i] / m_n[i];
  return out;
}

std::vector<double> StreamingMoments::stddevPopulation() const
{
  std::vector<double> out( m_cells, kNaN );
  for ( std::size_t i = 0; i < m_cells; ++i )
  {
    const int n = m_n[i];
    if ( n <= 0 )
      continue;
    if ( n == 1 )
    {
      out[i] = 0.0; // 单成员：离散度为零（如实，不外推样本口径）
      continue;
    }
    const double m = m_sum[i] / n;
    // E[x²] − E[x]²；浮点噪声可能出微负——夹到 0（与 SGS 旁路同一写法）。
    out[i] = std::sqrt( std::max( 0.0, m_sumsq[i] / n - m * m ) );
  }
  return out;
}

StatsResult compute( const std::vector<const double *> &members, std::size_t cells,
                     const StatsRequest &want )
{
  StatsResult out;
  if ( members.empty() || cells == 0 )
    return out;
  const bool wantMoments = want.mean || want.stddev;
  StreamingMoments moments;
  if ( wantMoments )
  {
    moments.reset( cells );
    for ( const double *field : members )
      if ( field )
        moments.addField( field );
    if ( want.mean )
      out.mean = moments.mean();
    if ( want.stddev )
      out.stddev = moments.stddevPopulation();
  }
  out.validCount.assign( cells, 0 );
  for ( std::size_t i = 0; i < cells; ++i )
  {
    int n = 0;
    for ( const double *field : members )
      if ( field && std::isfinite( field[i] ) )
        ++n;
    out.validCount[i] = n;
  }
  if ( want.p10 || want.p90 )
  {
    out.p10.assign( cells, kNaN );
    out.p90.assign( cells, kNaN );
    std::vector<double> cellValues( members.size() );
    for ( std::size_t i = 0; i < cells; ++i )
    {
      if ( out.validCount[i] == 0 )
        continue;
      std::size_t k = 0;
      for ( const double *field : members )
        if ( field )
          cellValues[k++] = field[i];
      if ( want.p10 )
        out.p10[i] = quantileOf( cellValues.data(), k, 0.10 );
      if ( want.p90 )
        out.p90[i] = quantileOf( cellValues.data(), k, 0.90 );
    }
  }
  return out;
}

std::vector<std::vector<double>> quantilesBanded(
    std::size_t memberCount, int cols, int rows,
    const std::vector<double> &quantiles, std::size_t maxBandValues,
    const std::function<bool( std::size_t member, int row0, int nRows, double *out )> &readBand,
    const std::function<bool()> &cancelled )
{
  std::vector<std::vector<double>> result;
  if ( memberCount == 0 || cols <= 0 || rows <= 0 || quantiles.empty() || !readBand )
    return result;
  const std::size_t cells = static_cast<std::size_t>( cols ) * rows;
  result.assign( quantiles.size(), std::vector<double>( cells, kNaN ) );

  // 常驻上限 → 带高。成员数 × 列数 已超上限时仍取 1 行（硬上限让位给正确性，
  // 超限规模由调用方在成员数上限处把关——SGS nRealizations ≤ 64）。
  const std::size_t perRow = memberCount * static_cast<std::size_t>( cols );
  const std::size_t bandRows =
      std::max<std::size_t>( 1, maxBandValues == 0 ? rows : maxBandValues / perRow );
  std::vector<double> slice;
  slice.resize( perRow * std::min<std::size_t>( bandRows, static_cast<std::size_t>( rows ) ) );
  std::vector<double> cellValues( memberCount );

  for ( int row0 = 0; row0 < rows; row0 += static_cast<int>( bandRows ) )
  {
    if ( cancelled && cancelled() )
      return {};
    const int nRows = static_cast<int>(
        std::min<std::size_t>( bandRows, static_cast<std::size_t>( rows - row0 ) ) );
    const std::size_t bandCells = static_cast<std::size_t>( nRows ) * cols;
    for ( std::size_t m = 0; m < memberCount; ++m )
      if ( !readBand( m, row0, nRows, slice.data() + m * bandCells ) )
        return {};
    for ( std::size_t i = 0; i < bandCells; ++i )
    {
      std::size_t k = 0;
      for ( std::size_t m = 0; m < memberCount; ++m )
        cellValues[k++] = slice[m * bandCells + i];
      const std::size_t cell = static_cast<std::size_t>( row0 ) * cols + i;
      for ( std::size_t qi = 0; qi < quantiles.size(); ++qi )
        result[qi][cell] = quantileOf( cellValues.data(), k, quantiles[qi] );
    }
  }
  return result;
}

} // namespace paleo::ensemble
