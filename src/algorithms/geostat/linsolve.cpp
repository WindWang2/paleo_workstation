// 层：数据
#include "linsolve.h"

#include <cmath>
#include <utility>

namespace paleo::geostat
{

bool solveDenseLu( const std::vector<double> &a, int n,
                   const std::vector<double> &b, std::vector<double> &x )
{
  if ( n <= 0 || a.size() != static_cast<std::size_t>( n ) * n || b.size() != static_cast<std::size_t>( n ) )
    return false;
  std::vector<double> lu = a;
  std::vector<int> permutation( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
    permutation[static_cast<std::size_t>( i )] = i;

  double scale = 0;
  for ( double value : a )
    scale = std::max( scale, std::fabs( value ) );
  if ( !( scale > 0 ) )
    return false;
  const double pivotFloor = 1e-12 * scale;

  for ( int k = 0; k < n; ++k )
  {
    int pivotRow = k;
    double pivotBest = std::fabs( lu[static_cast<std::size_t>( k ) * n + k] );
    for ( int i = k + 1; i < n; ++i )
    {
      const double candidate = std::fabs( lu[static_cast<std::size_t>( i ) * n + k] );
      if ( candidate > pivotBest )
      {
        pivotBest = candidate;
        pivotRow = i;
      }
    }
    if ( pivotBest <= pivotFloor )
      return false;
    if ( pivotRow != k )
    {
      for ( int j = 0; j < n; ++j )
        std::swap( lu[static_cast<std::size_t>( k ) * n + j], lu[static_cast<std::size_t>( pivotRow ) * n + j] );
      std::swap( permutation[static_cast<std::size_t>( k )], permutation[static_cast<std::size_t>( pivotRow )] );
    }
    const double pivot = lu[static_cast<std::size_t>( k ) * n + k];
    for ( int i = k + 1; i < n; ++i )
    {
      const double factor = lu[static_cast<std::size_t>( i ) * n + k] / pivot;
      lu[static_cast<std::size_t>( i ) * n + k] = factor;
      for ( int j = k + 1; j < n; ++j )
        lu[static_cast<std::size_t>( i ) * n + j] -= factor * lu[static_cast<std::size_t>( k ) * n + j];
    }
  }

  std::vector<double> pb( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
    pb[static_cast<std::size_t>( i )] = b[static_cast<std::size_t>( permutation[static_cast<std::size_t>( i )] )];
  // Ly = Pb（L 单位下三角）
  for ( int i = 1; i < n; ++i )
    for ( int j = 0; j < i; ++j )
      pb[static_cast<std::size_t>( i )] -= lu[static_cast<std::size_t>( i ) * n + j] * pb[static_cast<std::size_t>( j )];
  // Ux = y
  for ( int i = n - 1; i >= 0; --i )
  {
    for ( int j = i + 1; j < n; ++j )
      pb[static_cast<std::size_t>( i )] -= lu[static_cast<std::size_t>( i ) * n + j] * pb[static_cast<std::size_t>( j )];
    pb[static_cast<std::size_t>( i )] /= lu[static_cast<std::size_t>( i ) * n + i ];
  }
  x = std::move( pb );
  return true;
}

} // namespace paleo::geostat
