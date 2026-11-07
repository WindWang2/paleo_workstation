// 层：数据
#pragma once

namespace paleo::seisattr {

// Shared weighting mode for the single-volume and tiled coherence kernels.
enum class CoherenceWeightMode
{
  Equal = 0,
  InverseDistance = 1
};

} // namespace paleo::seisattr
