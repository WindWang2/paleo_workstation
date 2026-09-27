// 层：数据
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "domain/seismic/sgyvolume.h"

namespace seismic {

struct SgySectionOptions {
    // Upper bound on the number of columns. Coarse previews use ~256, the fine
    // pass uses the viewport budget (up to 2048).
    int maxColumns = 2048;
    // false (default): nearest trace per column - reliable and numerically
    // testable. true: linear blend between the two neighbouring traces.
    bool interpolate = false;
    bool keepOutsideColumns = false;
};

struct SgySectionStats {
    int columns = 0;
    int uniqueTraces = 0;
    int tracesRead = 0;
    int duplicateColumns = 0; // columns that reuse a trace read for another column
    int missingColumns = 0;   // no trace (or a rule mismatch) - stored as NaN
    int readRanges = 0;       // contiguous trace ranges actually read
    std::vector<float> columnDistances; // cumulative distance per column
};

// Builds a line section from raw amplitudes.
//
// Read strategy:
//   * each column resolves to a trace through the shared index,
//   * columns that resolve to the same trace are read once (duplicateColumns),
//   * unique traces are read in ascending file order (readRanges) so the disk
//     sees sequential I/O instead of random seeks,
//   * missing traces become NaN, never zero amplitude,
//   * progress(processedColumns, totalColumns) may cancel the build.
bool BuildLineSection(
    const SgyVolume& volume,
    const std::vector<glm::ivec2>& pathPoints,
    const SgySectionOptions& options,
    SgySliceImage& image,
    SgySectionStats& stats,
    std::string& errorMessage,
    const std::function<bool(int processed, int total)>& progress = {});

} // namespace seismic
