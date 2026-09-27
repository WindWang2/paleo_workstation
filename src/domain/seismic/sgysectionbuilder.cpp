#include "domain/seismic/sgysectionbuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "domain/seismic/sgyreadsession.h"

namespace seismic {
namespace {

struct ColumnTarget {
    float inlineValue = 0.0f;
    float xlineValue = 0.0f;
    float distance = 0.0f;
    int traceIndex = -1;
    bool outside = false;
};

} // namespace

bool BuildLineSection(
    const SgyVolume& volume,
    const std::vector<glm::ivec2>& pathPoints,
    const SgySectionOptions& options,
    SgySliceImage& image,
    SgySectionStats& stats,
    std::string& errorMessage,
    const std::function<bool(int processed, int total)>& progress) {
    stats = SgySectionStats{};
    image = {};
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

    // Geometry: cumulative distance along the path.
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
    // Nearest-trace sampling gains nothing from more columns than grid steps,
    // so it is capped by the path length. Interpolation may oversample up to
    // the requested budget to produce a smooth blend between trace centres.
    const int naturalColumns = static_cast<int>(std::round(totalLength)) + 1;
    const int columns = options.interpolate
        ? requestedColumns
        : std::clamp(std::min(requestedColumns, naturalColumns), 2, requestedColumns);
    stats.columns = columns;
    stats.columnDistances.resize(static_cast<std::size_t>(columns));

    // Resolve every column to a trace first.
    std::vector<ColumnTarget> targets(static_cast<std::size_t>(columns));
    std::size_t segmentIndex = 0;
    float segmentStartDistance = 0.0f;
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
    }

    // Deduplicate: read each unique trace once, in ascending file order.
    std::vector<int> uniqueTraces;
    uniqueTraces.reserve(targets.size());
    for(const ColumnTarget& target : targets) {
        if(target.traceIndex >= 0) {
            uniqueTraces.push_back(target.traceIndex);
        }
    }
    std::sort(uniqueTraces.begin(), uniqueTraces.end());
    uniqueTraces.erase(std::unique(uniqueTraces.begin(), uniqueTraces.end()), uniqueTraces.end());
    stats.uniqueTraces = static_cast<int>(uniqueTraces.size());

    int previousTrace = -2;
    for(int traceIndex : uniqueTraces) {
        if(traceIndex != previousTrace + 1) {
            ++stats.readRanges;
        }
        previousTrace = traceIndex;
    }

    std::unordered_map<int, int> traceColumn; // trace index -> first column that uses it
    traceColumn.reserve(uniqueTraces.size());
    for(int column = 0; column < columns; ++column) {
        const int traceIndex = targets[static_cast<std::size_t>(column)].traceIndex;
        if(traceIndex >= 0 && traceColumn.find(traceIndex) == traceColumn.end()) {
            traceColumn.emplace(traceIndex, column);
        } else if(traceIndex >= 0) {
            ++stats.duplicateColumns;
        }
    }

    std::vector<float> values(
        static_cast<std::size_t>(columns) * static_cast<std::size_t>(sampleCount),
        std::numeric_limits<float>::quiet_NaN());

    // One read session for the whole build: the file is opened once and the
    // traces are read in ascending file order.
    SgyReadSession session;
    if(!session.Open(volume.Index(), errorMessage)) {
        return false;
    }

    std::unordered_map<int, std::vector<float>> traceSamples;
    traceSamples.reserve(uniqueTraces.size());
    int processed = 0;
    for(int traceIndex : uniqueTraces) {
        if(progress && !progress(processed, static_cast<int>(uniqueTraces.size()))) {
            errorMessage = "Line section build cancelled by caller.";
            return false;
        }
        std::vector<float> samples;
        if(!session.ReadTrace(traceIndex, samples, errorMessage)) {
            // Unreadable trace or rule-layout mismatch: leave the columns NaN.
            continue;
        }
        ++stats.tracesRead;
        traceSamples.emplace(traceIndex, std::move(samples));
        ++processed;
    }
    if(progress && !progress(static_cast<int>(uniqueTraces.size()), static_cast<int>(uniqueTraces.size()))) {
        errorMessage = "Line section build cancelled by caller.";
        return false;
    }

    for(int column = 0; column < columns; ++column) {
        const ColumnTarget& target = targets[static_cast<std::size_t>(column)];
        if(target.traceIndex < 0) {
            ++stats.missingColumns;
            continue;
        }
        const auto it = traceSamples.find(target.traceIndex);
        if(it == traceSamples.end()) {
            ++stats.missingColumns;
            continue;
        }
        const std::vector<float>& samples = it->second;
        for(int s = 0; s < sampleCount; ++s) {
            const int row = sampleCount - 1 - s;
            values[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                   static_cast<std::size_t>(column)] = samples[static_cast<std::size_t>(s)];
        }
    }

    if(options.interpolate) {
        // Linear interpolation along the path between the trace centres: a
        // column is blended between the trace it resolved to and the next
        // trace along the path, weighted by the distance between the two trace
        // centres. The nearest-trace result stays the default.
        std::vector<std::pair<int, float>> traceOrder;
        traceOrder.reserve(uniqueTraces.size());
        for(int column = 0; column < columns; ++column) {
            const int traceIndex = targets[static_cast<std::size_t>(column)].traceIndex;
            if(traceIndex < 0) {
                continue;
            }
            if(traceOrder.empty() || traceOrder.back().first != traceIndex) {
                traceOrder.emplace_back(traceIndex, stats.columnDistances[static_cast<std::size_t>(column)]);
            }
        }

        std::vector<float> blended = values;
        for(int column = 0; column < columns; ++column) {
            const int traceIndex = targets[static_cast<std::size_t>(column)].traceIndex;
            if(traceIndex < 0) {
                continue;
            }
            std::size_t orderIndex = traceOrder.size();
            for(std::size_t i = 0; i < traceOrder.size(); ++i) {
                if(traceOrder[i].first == traceIndex) {
                    orderIndex = i;
                    break;
                }
            }
            if(orderIndex + 1 >= traceOrder.size()) {
                continue;
            }
            const float d0 = traceOrder[orderIndex].second;
            const float d1 = traceOrder[orderIndex + 1].second;
            if(d1 - d0 <= 1e-6f) {
                continue;
            }
            const auto nextIt = traceSamples.find(traceOrder[orderIndex + 1].first);
            const auto currentIt = traceSamples.find(traceIndex);
            if(nextIt == traceSamples.end() || currentIt == traceSamples.end()) {
                continue;
            }
            const float weight = std::clamp(
                (stats.columnDistances[static_cast<std::size_t>(column)] - d0) / (d1 - d0), 0.0f, 1.0f);
            for(int s = 0; s < sampleCount; ++s) {
                const std::size_t row = static_cast<std::size_t>(sampleCount - 1 - s) *
                                            static_cast<std::size_t>(columns) +
                                        static_cast<std::size_t>(column);
                const float a = currentIt->second[static_cast<std::size_t>(s)];
                const float b = nextIt->second[static_cast<std::size_t>(s)];
                blended[row] = (1.0f - weight) * a + weight * b;
            }
        }
        values = std::move(blended);
    }

    image.width = columns;
    image.height = sampleCount;
    image.values = std::move(values);
    SgyVolume::Recolorize(image);
    return true;
}

} // namespace seismic
