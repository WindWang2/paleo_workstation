// 层：数据
#pragma once

#include <glm/glm.hpp>

namespace seismic {

class ColorMap {
public:
    static glm::vec3 SampleHorizonDepth(float t);
};

} // namespace seismic
