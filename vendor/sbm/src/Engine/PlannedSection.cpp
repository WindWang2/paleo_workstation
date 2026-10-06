#include "Engine/PlannedSection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "Engine/PlannedReader.h"

namespace seismic {
namespace engine {
namespace {

struct ColumnTarget {
    float inlineValue = 0.0f;
    float xlineValue = 0.0f;
    float distance = 0.0f;
    int traceIndex = -1;
    bool outside = false;
};

} // namespace

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
    const std::function<bool(int processed, int total)>& progress,
    const SgyCoordinateMapper* mapper) {
    stats = PlannedSectionStats{};
    image = {};
    outColumns = PlannedSectionColumns{};
    errorMessage.clear();

    if(!volume.IsLoaded()) {
        errorMessage = "SGY volume index is not complete.";
        return false;
    }
    if(pathPoints.size() < 2) {
        errorMessage = "Line section needs at least two path points.";
        return false;
    }
    const int sampleCount = volume.SampleCount();
    if(sampleCount <= 0) {
        errorMessage = "SGY volume has no samples.";
        return false;
    }
    if(options.interpolate) {
        errorMessage = "the planned section path supports nearest-trace sampling only.";
        return false;
    }
    const SgyIndexPtr index = volume.Index();
    if(!index) {
        errorMessage = "SGY index is missing.";
        return false;
    }

    const auto resolveStart = std::chrono::steady_clock::now();
    std::vector<float> segmentLengths(pathPoints.size() - 1, 0.0f);
    float totalLength = 0.0f;
    for(std::size_t i = 1; i < pathPoints.size(); ++i) {
        const float di = static_cast<float>(pathPoints[i].x - pathPoints[i - 1].x);
        const float dx = static_cast<float>(pathPoints[i].y - pathPoints[i - 1].y);
        const float length = std::sqrt(di * di + dx * dx);
        segmentLengths[i - 1] = length;
        totalLength += length;
    }
    if(totalLength <= 1e-4f) {
        errorMessage = "Line section path is too short.";
        return false;
    }
    const int requestedColumns = std::clamp(options.maxColumns, 2, 8192);
    const int naturalColumns = static_cast<int>(std::round(totalLength)) + 1;
    const int columns = std::clamp(std::min(requestedColumns, naturalColumns), 2, requestedColumns);
    stats.columns = columns;
    stats.columnDistances.resize(static_cast<std::size_t>(columns));

    std::vector<ColumnTarget> targets(static_cast<std::size_t>(columns));
    std::size_t segmentIndex = 0;
    float segmentStartDistance = 0.0f;
    std::vector<int> traceIndices(static_cast<std::size_t>(columns), -1);
    for(int column = 0; column < columns; ++column) {
        const float targetDistance = columns <= 1
            ? 0.0f
            : (static_cast<float>(column) / static_cast<float>(columns - 1)) * totalLength;
        while(segmentIndex + 1 < segmentLengths.size() &&
            targetDistance > segmentStartDistance + segmentLengths[segmentIndex]) {
            segmentStartDistance += segmentLengths[segmentIndex];
            ++segmentIndex;
        }
        const float segmentLength = std::max(segmentLengths[segmentIndex], 1e-4f);
        const float t = std::clamp((targetDistance - segmentStartDistance) / segmentLength, 0.0f, 1.0f);
        const glm::ivec2& a = pathPoints[segmentIndex];
        const glm::ivec2& b = pathPoints[segmentIndex + 1];

        ColumnTarget& target = targets[static_cast<std::size_t>(column)];
        target.inlineValue = static_cast<float>(a.x) + static_cast<float>(b.x - a.x) * t;
        target.xlineValue = static_cast<float>(a.y) + static_cast<float>(b.y - a.y) * t;
        target.distance = targetDistance;
        stats.columnDistances[static_cast<std::size_t>(column)] = targetDistance;

        if(options.keepOutsideColumns &&
            (target.inlineValue < static_cast<float>(volume.InlineMin()) ||
                target.inlineValue > static_cast<float>(volume.InlineMax()) ||
                target.xlineValue < static_cast<float>(volume.XlineMin()) ||
                target.xlineValue > static_cast<float>(volume.XlineMax()))) {
            target.outside = true;
            continue;
        }
        const int nearestInline = volume.FindNearestInlineValue(target.inlineValue);
        const int nearestXline = volume.FindNearestXlineValue(target.xlineValue);
        target.traceIndex = volume.FindTraceIndex(nearestInline, nearestXline);
        traceIndices[static_cast<std::size_t>(column)] = target.traceIndex;
    }
    stats.resampleMicros = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - resolveStart).count());

    ReadPlanOptions effectivePlan = planOptions;
    effectivePlan.sampleBegin = 0;
    effectivePlan.sampleCount = 0;
    ReadPlan plan = BuildReadPlan(*index, traceIndices, effectivePlan);
    if(!plan.error.empty()) {
        errorMessage = plan.error;
        return false;
    }
    stats.uniqueTraces = static_cast<int>(plan.uniqueTraces.size());
    stats.readRanges = static_cast<int>(plan.stats.readRanges);
    stats.bytesRead = plan.stats.bytesRead;
    stats.validBytes = plan.stats.validBytes;
    stats.readAmplification = plan.stats.readAmplification;

    // Duplicate columns: a column that resolves to a trace already used earlier.
    {
        std::vector<unsigned char> seen(plan.uniqueTraces.size(), 0);
        for(int column = 0; column < columns; ++column) {
            const int slot = plan.columnSlots[static_cast<std::size_t>(column)];
            if(slot < 0) {
                continue;
            }
            if(seen[static_cast<std::size_t>(slot)] != 0) {
                ++stats.duplicateColumns;
            }
            seen[static_cast<std::size_t>(slot)] = 1;
        }
    }

    auto pool = std::make_shared<TraceBufferPool>(8ull * 1024 * 1024);
    PlannedReader reader;
    if(!reader.Open(index, errorMessage)) {
        return false;
    }
    reader.SetBufferPool(pool);
    std::vector<PlannedTraceData> traceData;
    const bool readOk = reader.Execute(plan, traceData, cancel, errorMessage, progress);
    stats.tracesRead = static_cast<int>(reader.Stats().tracesRead);
    stats.sanitizedSampleReads = reader.Stats().sanitizedSampleReads;
    stats.gapTracesRead = static_cast<int>(reader.Stats().gapTracesRead);
    stats.ioMicros = reader.Stats().ioMicros;
    stats.decodeMicros = reader.Stats().decodeMicros;
    if(!readOk) {
        return false;
    }

    // Per-column metadata: distance, inline/xline, trace ordinal and optionally
    // X/Y from the affine coordinate fit.
    outColumns.inlineNos.reserve(static_cast<std::size_t>(columns));
    outColumns.xlineNos.reserve(static_cast<std::size_t>(columns));
    outColumns.traceIndices.assign(static_cast<std::size_t>(columns), -1);
    if(mapper != nullptr && mapper->valid()) {
        outColumns.xyX.reserve(static_cast<std::size_t>(columns));
        outColumns.xyY.reserve(static_cast<std::size_t>(columns));
    }
    for(int columnIndex = 0; columnIndex < columns; ++columnIndex) {
        const ColumnTarget& target = targets[static_cast<std::size_t>(columnIndex)];
        outColumns.inlineNos.push_back(target.inlineValue);
        outColumns.xlineNos.push_back(target.xlineValue);
        outColumns.traceIndices[static_cast<std::size_t>(columnIndex)] =
            plan.columnSlots[static_cast<std::size_t>(columnIndex)] >= 0
                ? plan.uniqueTraces[static_cast<std::size_t>(plan.columnSlots[static_cast<std::size_t>(columnIndex)])]
                : -1;
        if(mapper != nullptr && mapper->valid()) {
            double x = 0.0;
            double y = 0.0;
            if(mapper->MapInlineXline(target.inlineValue, target.xlineValue, x, y)) {
                outColumns.xyX.push_back(static_cast<float>(x));
                outColumns.xyY.push_back(static_cast<float>(y));
            } else {
                outColumns.xyX.push_back(std::numeric_limits<float>::quiet_NaN());
                outColumns.xyY.push_back(std::numeric_limits<float>::quiet_NaN());
            }
        }
    }

    std::vector<float> values(
        static_cast<std::size_t>(columns) * static_cast<std::size_t>(sampleCount),
        std::numeric_limits<float>::quiet_NaN());
    for(int column = 0; column < columns; ++column) {
        const int slot = plan.columnSlots[static_cast<std::size_t>(column)];
        if(slot < 0 || !traceData[static_cast<std::size_t>(slot)].valid) {
            ++stats.missingColumns;
            continue;
        }
        const std::vector<float>& samples = traceData[static_cast<std::size_t>(slot)].samples;
        for(int s = 0; s < sampleCount && s < static_cast<int>(samples.size()); ++s) {
            const int row = sampleCount - 1 - s;
            values[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                   static_cast<std::size_t>(column)] = samples[static_cast<std::size_t>(s)];
        }
    }

    image.width = columns;
    image.height = sampleCount;
    image.values = std::move(values);
    SgyVolume::Recolorize(image);
    return true;
}

} // namespace engine
} // namespace seismic
