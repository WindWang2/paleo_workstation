// 层：数据
#include "wellcompositemodel.h"

#include <algorithm>
#include <cmath>

namespace WellComposite
{

// 二分查找插值曲线在目标深度处的数值
float CurveData::valueAtDepth(float d) const
{
  if (depths.size() < 2 || values.size() < 2)
    return (depths.isEmpty() || values.isEmpty()) ? 0.0f : values.first();

  if (d <= depths.first())
    return values.first();
  if (d >= depths.last())
    return values.last();

  auto it = std::lower_bound(depths.begin(), depths.end(), d);
  if (it == depths.end())
    return values.last();

  const int idx = static_cast<int>(std::distance(depths.begin(), it));
  if (idx == 0)
    return values.first();

  const float d0 = depths.at(idx - 1);
  const float d1 = depths.at(idx);
  const float v0 = values.at(idx - 1);
  const float v1 = values.at(idx);

  if (!std::isfinite(v0) || !std::isfinite(v1))
    return std::isfinite(v0) ? v0 : (std::isfinite(v1) ? v1 : 0.0f);

  const float t = (d - d0) / qMax(1e-5f, (d1 - d0));
  return v0 + t * (v1 - v0);
}

bool ComprehensiveWellData::isEmpty() const
{
  return continuousCurves.isEmpty() && discreteCurves.isEmpty() &&
         lithologyIntervals.isEmpty() && formationIntervals.isEmpty() &&
         stratigraphyIntervals.isEmpty() && faciesIntervals.isEmpty();
}

} // namespace WellComposite
