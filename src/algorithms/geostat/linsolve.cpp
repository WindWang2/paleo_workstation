// 层：数据
#include "linsolve.h"

#include <algorithm>
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

  // H1（数值评审 2026-10-25）：主元下限按「列」取尺度，而非全矩阵最大值。
  // OK 加边矩阵里 Γ 块随基台值缩放、Lagrange 边固定为 1，消元后末主元约
  // n/sill；旧的全局 1e-12·max|A| 在 sill≳1e8 时把良态方程判成奇异
  // （每格 NaN）。按列尺度后对角块的真奇异（重复样本）仍按 1e-12·列尺度
  // 判出；已能通过旧判据的方程组下限只会更低，解逐位不变。
  std::vector<double> &columnFloor = workspace.columnFloor;
  columnFloor.assign( static_cast<std::size_t>( n ), 0.0 );
  double scale = 0;
  for ( int i = 0; i < n; ++i )
    for ( int j = 0; j < n; ++j )
    {
      const double magnitude = std::fabs( a[static_cast<std::size_t>( i ) * n + j] );
      columnFloor[static_cast<std::size_t>( j )] = std::max( columnFloor[static_cast<std::size_t>( j )], magnitude );
      scale = std::max( scale, magnitude );
    }
  if ( !( scale > 0 ) || !std::isfinite( scale ) )
    return false;
  for ( double &floor : columnFloor )
  {
    if ( !( floor > 0 ) )
      return false; // 整列为零：必奇异
    floor *= 1e-12;
  }

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
    if ( !( pivotBest > columnFloor[static_cast<std::size_t>( k )] ) )
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
