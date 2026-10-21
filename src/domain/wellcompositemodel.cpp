// 层：数据
#include "wellcompositemodel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace WellComposite
{

// 二分查找插值曲线在目标深度处的数值
float CurveData::valueAtDepth(float d) const
{
  if (depths.size() < 2 || values.size() < 2)
    // 全空曲线 → NaN（opt-02：缺失不伪造 0，与空档口径一致）；退化单点
    // 取首值（既有口径）。
    return (depths.isEmpty() || values.isEmpty())
               ? std::numeric_limits<float>::quiet_NaN()
               : values.first();

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

  // #412 短边裁口径：depths/values 不等长（上游裁剪路径会留下不完整状态，
  // 见 welllogfacies 裁剪注释）时按 values 短边截断索引——QVector::at 在
  // release 编译期只有 Q_ASSERT 边界检查，道头实时读数/悬停 tooltip 经本
  // 函数取坏数据井的读数会越界读。等长数据下 i0/i1 即 idx-1/idx，口径不变。
  const int vLast = static_cast<int>(values.size()) - 1;
  const int i0 = qMin(idx - 1, vLast);
  const int i1 = qMin(idx, vLast);

  const float d0 = depths.at(idx - 1);
  const float d1 = depths.at(idx);
  const float v0 = values.at(i0);
  const float v1 = values.at(i1);

  if (!std::isfinite(v0) || !std::isfinite(v1))
    // 空档两端都缺值 → NaN（opt-02）：0 是合法物理量（如 NPHI 0），把缺失
    // 伪造成 0 会污染道头读数/tooltip；与绘制侧值 NaN 断笔口径一致。
    return std::isfinite(v0) ? v0
                             : (std::isfinite(v1)
                                    ? v1
                                    : std::numeric_limits<float>::quiet_NaN());

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
