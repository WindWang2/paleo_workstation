#include "Data/Sgy/SgyVolume.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace seismic {

bool SgyVolume::ReadSampleValue(
    int inlineNo,
    int xlineNo,
    int sampleIndex,
    float& value,
    std::string& errorMessage) const {
    value = 0.0f;
    if(!IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }
    if(SampleCount() <= 0) {
        errorMessage = "SGY volume has no samples.";
        return false;
    }

    const int nearestInline = FindNearestInlineValue(static_cast<float>(inlineNo));
    const int nearestXline = FindNearestXlineValue(static_cast<float>(xlineNo));
    const int traceIndex = FindTraceIndex(nearestInline, nearestXline);
    if(traceIndex < 0) {
        errorMessage = "No trace found for requested inline/xline.";
        return false;
    }

    std::vector<float> samples;
    if(!ReadTraceAsFloat(traceIndex, samples, errorMessage)) {
        return false;
    }

    const int clampedSample = std::clamp(sampleIndex, 0, SampleCount() - 1);
    value = samples[static_cast<std::size_t>(clampedSample)];
    return true;
}

} // namespace seismic
