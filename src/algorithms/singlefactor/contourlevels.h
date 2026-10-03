// 层：数据
#pragma once

#include <cstdint>
#include <vector>

// 上游移植：drawing/ui/dialogs/single_factor_dialog.py 的等值线级别助手
// —— _nice_number / suggest_contour_step_and_range / contour_grid_value_range
// + ContourExtractDialog 的自适应/用户间距两级规则（QDoubleSpinBox 按
// decimals 取整、floor((end-start)/step+1e-10) 条数、按 decimals 格式化级别）。

namespace paleo::singlefactor
{

// _nice_number(value, round_up) → 1/2/5×10^n。
double niceContourNumber( double value, bool roundUp );

// suggest_contour_step_and_range(data_min, data_max, target_levels)
// 返回 {start, end, step}（start/end 已按 decimals+2 位、step 按 decimals+1
// 位做 Python round-half-even 规整）。
struct ContourStepRange
{
  double start = 0;
  double end = 1;
  double step = 0.1;
};
ContourStepRange suggestContourStepAndRange( double dataMin, double dataMax,
                                           int targetLevels = 8 );

// contour_grid_value_range(grid, valid_mask)：有限且（mask 空或 true）的
// 像元 min/max；无有效像元返回 false。
bool contourGridValueRange( const std::vector<double> &grid,
                            const std::vector<std::uint8_t> &validMask,
                            double *lo, double *hi );

// ContourExtractDialog 的最终级别方案：
//   spinDecimals(step) —— while s<1-1e-12 && d<6: s*=10,d++ → max(2,min(6,d+1))
//   spin 值 = start/end/step 各自按 spinDecimals 文本格式化回读（QDoubleSpinBox
//     取整），end<start 时交换
//   count = floor((end-start)/step + 1e-10)，levels[i]=start+i*step 各按
//     decimals 格式化（repr(float) 等价的十进制四舍五入）
struct ContourLevelPlan
{
  double valueMin = 0, valueMax = 0; // 有效值域（未规整）
  double start = 0, end = 0, step = 0; // spin 回读值
  int decimals = 2;
  std::vector<double> levels;
};

// 自动（interval<=0）：suggest(target_levels=10) → spin 取整 → 级别列。
// 无有效像元返回 false。
bool autoContourLevels( const std::vector<double> &grid,
                        const std::vector<std::uint8_t> &validMask,
                        ContourLevelPlan *plan );

// 用户间距（interval>0）：start/end 沿用自动 spin 值，step = interval 按其
// 自身 decimals 规则取整，级别 decimals 由该 step 重算。
bool intervalContourLevels( const std::vector<double> &grid,
                            const std::vector<std::uint8_t> &validMask,
                            double interval, ContourLevelPlan *plan );

} // namespace paleo::singlefactor
