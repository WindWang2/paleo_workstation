// 层：数据
#include "contourlevels.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::singlefactor
{
namespace
{

// Python round(x, d)：十进制 half-even（snprintf 语义与 CPython 一致）。
double roundHalfEven( double value, int decimals )
{
  const double scale = std::pow( 10.0, decimals );
  return std::nearbyint( value * scale ) / scale;
}

// float(f"{v:.{decimals}f}") 等价：十进制格式化回读。
double spinRound( double value, int decimals )
{
  return roundHalfEven( value, decimals );
}

// _apply_adaptive_step 的 decimals 推导（harness dialog_levels 同款）。
int spinDecimals( double step )
{
  int decimals = 0;
  double s = std::abs( step );
  while ( s > 0 && s < 1.0 - 1e-12 && decimals < 6 )
  {
    s *= 10.0;
    ++decimals;
  }
  return std::max( 2, std::min( 6, decimals + 1 ) );
}

// levels = [spin_round(start + i*step, decimals) for i in 0..count]，
// count = floor(max(0,end-start)/step + 1e-10)。start/end 已是最终 spin 值，
// 不再按 step 的 decimals 重取整。
ContourLevelPlan finalizePlan( double lo, double hi, double start, double end,
                               double step )
{
  ContourLevelPlan plan;
  plan.valueMin = lo;
  plan.valueMax = hi;
  plan.decimals = spinDecimals( step );
  plan.step = spinRound( step, plan.decimals );
  plan.start = start;
  plan.end = end;
  if ( plan.end < plan.start )
    std::swap( plan.start, plan.end );
  if ( !( plan.step > 0 ) )
  {
    // 自动路径不会走到这里（step>0 已由 suggest 保证）；用户间距小到 spin 取整为 0
    // （如 1e-300）→ 视同「级别数无穷」，按间距过小拒绝（#149）。
    plan.tooManyLevels = true;
    plan.estimatedLevels = std::numeric_limits<double>::infinity();
    return plan;
  }
  // #149：先用 double 估计条数并设上限，避免 double→int 溢出（UB）与巨量分配。
  const double estimated =
      estimateContourLevelCount( std::max( 0.0, plan.end - plan.start ), plan.step );
  plan.estimatedLevels = estimated;
  if ( !( estimated <= static_cast<double>( kMaxContourLevels ) ) )
  {
    plan.tooManyLevels = true;
    return plan;
  }
  const int count = static_cast<int>( estimated ) - 1;
  plan.levels.reserve( static_cast<std::size_t>( count ) + 1 );
  for ( int i = 0; i <= count; ++i )
    plan.levels.push_back( spinRound( plan.start + i * plan.step, plan.decimals ) );
  return plan;
}

} // namespace

double estimateContourLevelCount( double span, double step )
{
  if ( !( step > 0.0 ) || !std::isfinite( step ) || !std::isfinite( span ) )
    return std::numeric_limits<double>::infinity();
  return std::floor( std::max( 0.0, span ) / step + 1e-10 ) + 1.0;
}

double niceContourNumber( double value, bool roundUp )
{
  const double v = std::abs( value );
  if ( !std::isfinite( v ) || v <= 0.0 )
    return 0.1;
  const double exp = std::floor( std::log10( v ) );
  const double frac = v / std::pow( 10.0, exp );
  double niceFrac;
  if ( roundUp )
  {
    if ( frac <= 1.0 )
      niceFrac = 1.0;
    else if ( frac <= 2.0 )
      niceFrac = 2.0;
    else if ( frac <= 5.0 )
      niceFrac = 5.0;
    else
      niceFrac = 10.0;
  }
  else
  {
    if ( frac < 1.5 )
      niceFrac = 1.0;
    else if ( frac < 3.0 )
      niceFrac = 2.0;
    else if ( frac < 7.0 )
      niceFrac = 5.0;
    else
      niceFrac = 10.0;
  }
  return niceFrac * std::pow( 10.0, exp );
}

ContourStepRange suggestContourStepAndRange( double dataMin, double dataMax,
                                             int targetLevels )
{
  double lo = dataMin, hi = dataMax;
  if ( !std::isfinite( lo ) )
    lo = 0.0;
  if ( !std::isfinite( hi ) )
    hi = 1.0;
  if ( hi < lo )
    std::swap( lo, hi );
  const double span = hi - lo;
  const int n = std::max( 3, targetLevels );

  if ( span <= 1e-15 )
  {
    const double base = std::abs( lo ) > 1e-12 ? std::abs( lo ) : 1.0;
    const double step = niceContourNumber( base / 10.0, true );
    return ContourStepRange{ lo, lo + step * std::max( n - 1, 1 ), step };
  }

  const double rough = span / static_cast<double>( n );
  double step = niceContourNumber( rough, true );
  if ( step >= span && span > 0 )
  {
    step = niceContourNumber( span / std::max( n, 4 ), false );
    if ( step <= 0 )
      step = span / std::max( n, 4 );
  }

  int decimals = 0;
  double s = step;
  while ( s < 1.0 - 1e-12 && decimals < 6 )
  {
    s *= 10.0;
    ++decimals;
  }
  double start = roundHalfEven( lo, decimals + 2 );
  double end = roundHalfEven( hi, decimals + 2 );
  step = roundHalfEven( step, decimals + 1 );
  if ( step <= 0 )
    step = std::max( span / n, 1e-6 );
  return ContourStepRange{ start, end, step };
}

bool contourGridValueRange( const std::vector<double> &grid,
                            const std::vector<std::uint8_t> &validMask,
                            double *lo, double *hi )
{
  if ( !validMask.empty() && validMask.size() != grid.size() )
    return false; // "趋势面有效范围与栅格尺寸不一致"
  double mn = std::numeric_limits<double>::infinity();
  double mx = -std::numeric_limits<double>::infinity();
  bool any = false;
  for ( std::size_t i = 0; i < grid.size(); ++i )
  {
    if ( !validMask.empty() && !validMask[i] )
      continue;
    const double v = grid[i];
    if ( !std::isfinite( v ) )
      continue;
    any = true;
    mn = std::min( mn, v );
    mx = std::max( mx, v );
  }
  if ( !any )
    return false;
  *lo = mn;
  *hi = mx;
  return true;
}

bool autoContourLevels( const std::vector<double> &grid,
                        const std::vector<std::uint8_t> &validMask,
                        ContourLevelPlan *plan )
{
  double lo, hi;
  if ( !contourGridValueRange( grid, validMask, &lo, &hi ) )
    return false;
  const ContourStepRange suggested = suggestContourStepAndRange( lo, hi, 10 );
  // 自动路径：start/end/step 各自按 suggest 的 decimals 规则先入 spin。
  const int decimals = spinDecimals( suggested.step );
  *plan = finalizePlan( lo, hi, spinRound( suggested.start, decimals ),
                        spinRound( suggested.end, decimals ),
                        spinRound( suggested.step, decimals ) );
  return true;
}

bool intervalContourLevels( const std::vector<double> &grid,
                            const std::vector<std::uint8_t> &validMask,
                            double interval, ContourLevelPlan *plan )
{
  double lo, hi;
  if ( !contourGridValueRange( grid, validMask, &lo, &hi ) )
    return false;
  const ContourStepRange suggested = suggestContourStepAndRange( lo, hi, 10 );
  // start/end 沿用自动 spin 值；step = interval 按自身 decimals 取整，
  // 级别 decimals 由该 step 重算。
  const int decimals = spinDecimals( suggested.step );
  const double start = spinRound( suggested.start, decimals );
  const double end = spinRound( suggested.end, decimals );
  *plan = finalizePlan( lo, hi, start, end, interval );
  return plan->step > 0 && !plan->tooManyLevels;
}

} // namespace paleo::singlefactor
