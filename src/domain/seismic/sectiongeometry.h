// 层：数据
#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace seismic {
struct SectionGeometry {
  std::vector<float> distancesM;
  std::vector<glm::dvec2> coordinates;
  // Engine distances use IL/XL steps. Interpolate each segment independently:
  // anisotropic/rotated grids cannot use one global metres-per-step scale.
  static SectionGeometry fromColumns(const std::vector<glm::ivec2> &gridPath,
                                     const std::vector<glm::dvec2> &mapPath,
                                     const std::vector<float> &gridDistances);
};
} // namespace seismic
