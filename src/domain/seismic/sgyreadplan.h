#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "domain/seismic/sgyindex.h"

namespace seismic {
namespace engine {

// Converts a resolved trace list (one entry per output column, -1 = missing)
// into a deduplicated, offset-ordered, range-merged read plan.
struct ReadPlanOptions {
    // Merge two ranges separated by at most this many unused traces.
    // 0 = strict adjacency (no gap fills). Larger values trade extra bytes for
    // fewer seeks.
    int maxGapTraces = 0;
    int sampleBegin = 0;
    int sampleCount = 0; // 0 = the full trace
};

struct PlannedRange {
    int firstTrace = 0; // trace ordinal in the file
    int traceCount = 0; // includes gap fills
    int firstSlot = 0;  // index into ReadPlan::traces
};

struct ReadPlanStats {
    std::uint64_t requestedTraces = 0;
    std::uint64_t uniqueTraces = 0;
    std::uint64_t plannedTraces = 0; // unique traces + gap fills
    std::uint64_t gapTraces = 0;
    std::uint64_t readRanges = 0;
    std::uint64_t bytesRead = 0;    // plannedTraces * bytesPerTrace
    std::uint64_t validBytes = 0;   // uniqueTraces * samplesRead * bytesPerSample
    double readAmplification = 0.0; // bytesRead / validBytes (0 when validBytes == 0)
    int samplesRead = 0;
    int bytesPerTrace = 0;
};

struct ReadPlan {
    std::vector<int> traces;       // planned traces (ascending, includes gap fills)
    std::vector<int> uniqueTraces; // deduplicated requested traces (ascending)
    std::vector<PlannedRange> ranges;
    std::vector<int> columnSlots;  // per input column: index into uniqueTraces, or -1
    ReadPlanStats stats;
    bool planned = false;
    std::string error;
};

ReadPlan BuildReadPlan(
    const SgyIndex& index,
    const std::vector<int>& traceIndices,
    const ReadPlanOptions& options);

} // namespace engine
} // namespace seismic