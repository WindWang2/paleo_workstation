#include "domain/seismic/sgyreadplan.h"

#include <algorithm>

namespace seismic {
namespace engine {

ReadPlan BuildReadPlan(
    const SgyIndex& index,
    const std::vector<int>& traceIndices,
    const ReadPlanOptions& options) {
    ReadPlan plan;
    plan.stats.requestedTraces = static_cast<std::uint64_t>(traceIndices.size());
    plan.columnSlots.assign(traceIndices.size(), -1);

    if(index.sampleCount <= 0) {
        plan.error = "the index has no samples";
        return plan;
    }
    const int sampleBegin = std::max(0, options.sampleBegin);
    const int samplesRead = options.sampleCount > 0
        ? std::min(options.sampleCount, std::max(0, index.sampleCount - sampleBegin))
        : std::max(0, index.sampleCount - sampleBegin);
    if(samplesRead <= 0) {
        plan.error = "the sample window is empty";
        return plan;
    }
    const int bytesPerSample = index.formatSizeBytes > 0 ? index.formatSizeBytes : 4;
    const int bytesPerTrace = 240 + samplesRead * bytesPerSample;
    plan.stats.samplesRead = samplesRead;
    plan.stats.bytesPerTrace = bytesPerTrace;

    plan.uniqueTraces.reserve(traceIndices.size());
    for(int traceIndex : traceIndices) {
        if(traceIndex >= 0) {
            plan.uniqueTraces.push_back(traceIndex);
        }
    }
    std::sort(plan.uniqueTraces.begin(), plan.uniqueTraces.end());
    plan.uniqueTraces.erase(
        std::unique(plan.uniqueTraces.begin(), plan.uniqueTraces.end()),
        plan.uniqueTraces.end());
    plan.stats.uniqueTraces = static_cast<std::uint64_t>(plan.uniqueTraces.size());
    plan.stats.validBytes = plan.stats.uniqueTraces * static_cast<std::uint64_t>(samplesRead) *
                            static_cast<std::uint64_t>(bytesPerSample);

    // Column -> unique slot (first occurrence wins, duplicates counted by the caller).
    std::vector<std::pair<int, int>> slots; // (traceIndex, unique slot)
    slots.reserve(plan.uniqueTraces.size());
    for(std::size_t i = 0; i < plan.uniqueTraces.size(); ++i) {
        slots.emplace_back(plan.uniqueTraces[i], static_cast<int>(i));
    }
    for(std::size_t column = 0; column < traceIndices.size(); ++column) {
        const int traceIndex = traceIndices[column];
        if(traceIndex < 0) {
            continue;
        }
        const auto it = std::lower_bound(
            slots.begin(), slots.end(), std::make_pair(traceIndex, -1),
            [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                return a.first < b.first;
            });
        if(it != slots.end() && it->first == traceIndex) {
            plan.columnSlots[column] = it->second;
        }
    }

    // Build merged ranges over the unique traces.
    const int maxGap = std::max(0, options.maxGapTraces);
    plan.traces = plan.uniqueTraces;
    if(maxGap > 0) {
        std::vector<int> merged;
        merged.reserve(plan.traces.size());
        for(int traceIndex : plan.traces) {
            if(!merged.empty() && traceIndex - merged.back() - 1 <= maxGap) {
                for(int fill = merged.back() + 1; fill < traceIndex; ++fill) {
                    merged.push_back(fill);
                }
            }
            merged.push_back(traceIndex);
        }
        plan.traces = std::move(merged);
    }
    plan.stats.plannedTraces = static_cast<std::uint64_t>(plan.traces.size());
    plan.stats.gapTraces = plan.stats.plannedTraces - plan.stats.uniqueTraces;
    plan.stats.bytesRead = plan.stats.plannedTraces * static_cast<std::uint64_t>(bytesPerTrace);
    plan.stats.readAmplification = plan.stats.validBytes > 0
        ? static_cast<double>(plan.stats.bytesRead) / static_cast<double>(plan.stats.validBytes)
        : 0.0;

    for(std::size_t i = 0; i < plan.traces.size();) {
        PlannedRange range;
        range.firstTrace = plan.traces[i];
        range.firstSlot = static_cast<int>(i);
        std::size_t end = i + 1;
        while(end < plan.traces.size() && plan.traces[end] == plan.traces[end - 1] + 1) {
            ++end;
        }
        range.traceCount = static_cast<int>(end - i);
        plan.ranges.push_back(range);
        i = end;
    }
    plan.stats.readRanges = static_cast<std::uint64_t>(plan.ranges.size());
    plan.planned = !plan.uniqueTraces.empty();
    return plan;
}

} // namespace engine
} // namespace seismic