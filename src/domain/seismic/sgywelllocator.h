// 层：数据
#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyindex.h"

namespace seismic {

// Minimal well description so the locator stays independent from the UI model.
struct SgyWellInput {
    std::string name;
    double x = 0.0;
    double y = 0.0;
    double bottomX = 0.0;
    double bottomY = 0.0;
};

struct SgyWellFix {
    int wellIndex = -1;
    std::string name;
    double x = 0.0;
    double y = 0.0;
    double bottomX = 0.0;
    double bottomY = 0.0;
    int inlineNo = 0;
    int xlineNo = 0;
    int traceIndex = -1;
    bool mapped = false; // XY -> inline/xline succeeded
    bool inside = false; // mapped position is inside the indexed coverage
    std::string note;    // human-readable reason when not locatable
};

struct SgyWellLocationResult {
    bool mapperAvailable = false;
    std::string mapperDescription;
    std::vector<SgyWellFix> wells;
    int insideCount = 0;
    int outsideCount = 0;
    int unmappedCount = 0;
};

// Pure data-layer localisation: XY -> inline/xline through the sampled trace
// coordinates of one SEG-Y index. No UI, no OpenGL, no Horizon dependency.
//
// Honest behaviour:
//   * a missing mapper leaves every well unmapped with a reason instead of
//     guessing a position,
//   * wells outside the coverage are marked outside and never clamped,
//   * the same index is used for every well (the caller decides which SEG-Y is
//     active).
class SgyWellLocator {
public:
    static SgyWellLocationResult Locate(const SgyIndex& index, const std::vector<SgyWellInput>& wells);

    // Ordered path (in the order the wells were given) through the wells that
    // are inside the coverage; used for the "section through wells" action.
    static std::vector<glm::ivec2> BuildSectionPath(const SgyWellLocationResult& location);
};

} // namespace seismic
