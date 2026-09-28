#include "Data/Sgy/SgyWellLocator.h"

#include <algorithm>
#include <cmath>

namespace seismic {

SgyWellLocationResult SgyWellLocator::Locate(
    const SgyIndex& index,
    const std::vector<SgyWellInput>& wells) {
    SgyWellLocationResult result;
    result.wells.reserve(wells.size());

    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(index);
    result.mapperAvailable = mapper.valid();
    result.mapperDescription = mapper.Describe();

    for(std::size_t i = 0; i < wells.size(); ++i) {
        const SgyWellInput& well = wells[i];
        SgyWellFix fix;
        fix.wellIndex = static_cast<int>(i);
        fix.name = well.name;
        fix.x = well.x;
        fix.y = well.y;
        fix.bottomX = well.bottomX;
        fix.bottomY = well.bottomY;

        if(!result.mapperAvailable) {
            fix.note = "cannot project: SEG-Y trace coordinates are unavailable";
            ++result.unmappedCount;
            result.wells.push_back(std::move(fix));
            continue;
        }

        double inlineNo = 0.0;
        double xlineNo = 0.0;
        if(!mapper.MapXY(well.x, well.y, inlineNo, xlineNo)) {
            fix.note = "cannot project: the affine mapping is degenerate";
            ++result.unmappedCount;
            result.wells.push_back(std::move(fix));
            continue;
        }

        fix.mapped = true;
        fix.inlineNo = index.FindNearestInlineValue(static_cast<float>(inlineNo));
        fix.xlineNo = index.FindNearestXlineValue(static_cast<float>(xlineNo));
        fix.traceIndex = index.FindTraceIndex(fix.inlineNo, fix.xlineNo);
        fix.inside = mapper.InCoverage(well.x, well.y) && fix.traceIndex >= 0;
        if(fix.inside) {
            ++result.insideCount;
        } else {
            fix.note = "outside the SEG-Y coverage (reported, not clamped)";
            ++result.outsideCount;
        }
        result.wells.push_back(std::move(fix));
    }

    return result;
}

std::vector<glm::ivec2> SgyWellLocator::BuildSectionPath(const SgyWellLocationResult& location) {
    std::vector<glm::ivec2> path;
    for(const SgyWellFix& fix : location.wells) {
        if(!fix.inside) {
            continue;
        }
        const glm::ivec2 point(fix.inlineNo, fix.xlineNo);
        if(!path.empty() && path.back() == point) {
            continue;
        }
        path.push_back(point);
    }
    return path;
}

} // namespace seismic
