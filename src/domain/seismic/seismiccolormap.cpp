// 层：数据
#include "domain/seismic/seismiccolormap.h"

#include <algorithm>

namespace seismic {

glm::vec3 ColorMap::SampleHorizonDepth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);

    const glm::vec3 deepBlue(0.05f, 0.12f, 0.35f);
    const glm::vec3 cyan(0.05f, 0.75f, 0.95f);
    const glm::vec3 yellow(0.95f, 0.85f, 0.20f);
    const glm::vec3 red(0.85f, 0.12f, 0.08f);

    if(t < 0.33f) {
        return glm::mix(deepBlue, cyan, t / 0.33f);
    }
    if(t < 0.66f) {
        return glm::mix(cyan, yellow, (t - 0.33f) / 0.33f);
    }
    return glm::mix(yellow, red, (t - 0.66f) / 0.34f);
}

} // namespace seismic
