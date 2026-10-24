// 层：数据
#include "linsolve.h"

#include <cmath>
#include <utility>

// 层：数据
namespace paleo::geostat
{

bool solveDenseLu( const std::vector<double> &a, int n,
                   const std::vector<double> &b, std::vector<double> &x )
{
  LuWorkspace workspace;
  return solveDenseLu( a, n, b, x, workspace );
}

bool luFactor( const std::vector<double> &a, int n, LuWorkspace &workspace )
{
  if ( n <= 0 || a.size() != static_cast<std::size_t>( n ) * n )
    return false;
  workspace.lu.assign( a.begin(), a.end() );
  workspace.permutation.resize( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
    workspace.permutation[static_cast<std::size_t>( i )] = i;

  double scale = 0;
  for ( double value : a )
    scale = std::max( scale, std::fabs( value ) );
  if ( !( scale > 0 ) )
    return false;
  const double pivotFloor = 1e-12 * scale;

  std::vector<double> &lu = workspace.lu;
  std::vector<int> &permutation = workspace.permutation;
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

  return true;
}

void luSolve( const LuWorkspace &workspace, int n, const std::vector<double> &b, std::vector<double> &x )
{
  const std::vector<double> &lu = workspace.lu;
  const std::vector<int> &permutation = workspace.permutation;
  // 分解成功后才写 x：置换右端项直接落 x，就地前/回代（#478：不再单独
  // 分配 pb——算术序与旧实现逐位一致）。
  x.resize( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
    x[static_cast<std::size_t>( i )] = b[static_cast<std::size_t>( permutation[static_cast<std::size_t>( i )] )];
  // Ly = Pb（L 单位下三角）
  for ( int i = 1; i < n; ++i )
    for ( int j = 0; j < i; ++j )
      x[static_cast<std::size_t>( i )] -= lu[static_cast<std::size_t>( i ) * n + j] * x[static_cast<std::size_t>( j )];
  // Ux = y
  for ( int i = n - 1; i >= 0; --i )
  {
    for ( int j = i + 1; j < n; ++j )
      x[static_cast<std::size_t>( i )] -= lu[static_cast<std::size_t>( i ) * n + j] * x[static_cast<std::size_t>( j )];
    x[static_cast<std::size_t>( i )] /= lu[static_cast<std::size_t>( i ) * n + i ];
  }
}

bool solveDenseLu( const std::vector<double> &a, int n, const std::vector<double> &b,
                   std::vector<double> &x, LuWorkspace &workspace )
{
  if ( b.size() != static_cast<std::size_t>( n ) || !luFactor( a, n, workspace ) )
    return false;
  luSolve( workspace, n, b, x );
  return true;
}

} // namespace paleo::geostat
