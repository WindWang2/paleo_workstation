#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Data/Sgy/SgyCoordinateMapper.h"
#include "Data/Sgy/SgySectionBuilder.h"
#include "Data/Sgy/SgyVolume.h"
#include "Engine/ReadPlan.h"
#include "Engine/Types.h"

namespace seismic {
namespace engine {

// Per-column metadata produced by the planned section builder.
struct PlannedSectionColumns {
    std::vector<float> inlineNos;
    std::vector<float> xlineNos;
    std::vector<int> traceIndices;
    std::vector<float> xyX;
    std::vector<float> xyY;
};

struct PlannedSectionStats {
    int columns = 0;
    int uniqueTraces = 0;
    int tracesRead = 0;
    std::uint64_t sanitizedSampleReads = 0;
    int duplicateColumns = 0;
    int missingColumns = 0;
    int readRanges = 0;
    int gapTracesRead = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t validBytes = 0;
    double readAmplification = 0.0;
    double ioMicros = 0.0;
    double decodeMicros = 0.0;
    double resampleMicros = 0.0;
    std::vector<float> columnDistances;
};

// Nearest-trace planned section. Values are identical to BuildLineSection with
// interpolate=false; the read side is a ReadPlan (dedupe, ascending trace
// order, optional range merge) executed with one read session and a buffer pool.
bool BuildPlannedLineSection(
    const SgyVolume& volume,
    const std::vector<glm::ivec2>& pathPoints,
    const SgySectionOptions& options,
    const ReadPlanOptions& planOptions,
    SgySliceImage& image,
    PlannedSectionColumns& outColumns,
    PlannedSectionStats& stats,
    std::string& errorMessage,
    CancelToken* cancel,
    const std::function<bool(int processed, int total)>& progress = {},
    const SgyCoordinateMapper* mapper = nullptr);

} // namespace engine
} // namespace seismic
