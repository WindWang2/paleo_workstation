// 层：数据
#include "sectiongeometry.h"
#include <algorithm>
#include <cmath>
namespace seismic {
SectionGeometry
SectionGeometry::fromColumns(const std::vector<glm::ivec2> &gridPath,
                             const std::vector<glm::dvec2> &mapPath,
                             const std::vector<float> &gridDistances) {
  SectionGeometry out;
  if (gridPath.size() < 2 || gridPath.size() != mapPath.size())
    return out;
  std::vector<double> grid{0}, meters{0};
  for (std::size_t i = 1; i < gridPath.size(); ++i) {
    grid.push_back(grid.back() +
                   glm::length(glm::dvec2(gridPath[i] - gridPath[i - 1])));
    meters.push_back(meters.back() + glm::length(mapPath[i] - mapPath[i - 1]));
  }
  if (grid.back() <= 0)
    return out;
  for (double d : gridDistances) {
    if (!std::isfinite(d))
      return {};
    auto it = std::upper_bound(grid.begin(), grid.end(), d);
    auto i = std::clamp<std::size_t>(std::distance(grid.begin(), it), 1,
                                     grid.size() - 1);
    const double span = grid[i] - grid[i - 1];
    const double t =
        span > 0 ? std::clamp((d - grid[i - 1]) / span, 0.0, 1.0) : 0;
    out.distancesM.push_back(meters[i - 1] + t * (meters[i] - meters[i - 1]));
    out.coordinates.push_back(mapPath[i - 1] +
                              t * (mapPath[i] - mapPath[i - 1]));
  }
  return out;
}
} // namespace seismic
