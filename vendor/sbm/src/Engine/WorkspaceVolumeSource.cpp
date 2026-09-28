#include "Engine/WorkspaceVolumeSource.h"

#include "Engine/Cache.h"
#include "Engine/LodBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace seismic {
namespace engine {
namespace {

double MicrosSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

} // namespace

std::unique_ptr<WorkspaceVolumeSource> WorkspaceVolumeSource::Open(
    const std::filesystem::path& basePath, Status& status) {
    auto source = std::unique_ptr<WorkspaceVolumeSource>(new WorkspaceVolumeSource());
    std::string error;
    if(!source->reader_.Open(basePath, error)) {
        status = Status::Error(StatusCode::IoError, error);
        return nullptr;
    }
    const WorkspaceInfo& info = source->reader_.Info();
    source->metadata_.name = basePath.filename().u8string();
    source->metadata_.path = basePath;
    // The stable workspace format stores a line count and a minimum line number,
    // which only describes a step-1 axis; the Paged format (version 4) stores
    // the real AxisDescriptor. traceCount is the number of real traces, i.e. the
    // line product for a full rectangle, never samples x lines x lines.
    source->metadata_.traceCount = static_cast<std::int64_t>(info.inlines) *
                                   static_cast<std::int64_t>(info.xlines);
    source->metadata_.sampleCount = static_cast<int>(info.samples);
    source->metadata_.sampleIntervalUs = static_cast<int>(info.sampleIntervalUs);
    source->metadata_.inlineMin = info.inlineMin;
    source->metadata_.inlineMax = info.inlineMin + static_cast<int>(info.inlines) - 1;
    source->metadata_.xlineMin = info.xlineMin;
    source->metadata_.xlineMax = info.xlineMin + static_cast<int>(info.xlines) - 1;
    source->metadata_.ruleBased = true;
    source->metadata_.indexComplete = true;
    // The stable format can only express a step-1 axis; the Paged format stores
    // the real AxisDescriptor (see PagedWorkspaceVolumeSource).
    source->metadata_.inlineAxis = AxisDescriptor::Uniform(info.inlineMin, 1,
                                                           static_cast<int>(info.inlines));
    source->metadata_.xlineAxis = AxisDescriptor::Uniform(info.xlineMin, 1,
                                                          static_cast<int>(info.xlines));
    status = Status::Ok();
    return source;
}

const DatasetMetadata& WorkspaceVolumeSource::Metadata() const {
    return metadata_;
}

void WorkspaceVolumeSource::CountRequest(const Status& status) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.requests;
    if(status.code == StatusCode::Cancelled) {
        ++stats_.cancelledRequests;
    }
}

Status WorkspaceVolumeSource::ReadAxisSlice(
    int mode, int index, Slice2D& out, CancelToken* cancel, int maxColumns) {
    if(cancel != nullptr && cancel->IsCancelled()) {
        const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
        CountRequest(status);
        return status;
    }
    if(maxColumns > 0) {
        // The prototype assembles the full slice and reports it; a bounded
        // preview path can be added later.
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> values;
    std::string error;
    bool ok = false;
    if(mode == 0) {
        ok = reader_.ReadInlineSlice(index, values, error);
    } else if(mode == 1) {
        // Xline slices are assembled column by column (no dedicated helper).
        const WorkspaceInfo& info = reader_.Info();
        const std::int64_t xl = index - info.xlineMin;
        if(xl < 0 || xl >= info.xlines) {
            const Status status = Status::Error(StatusCode::InvalidArgument, "xline is outside the volume");
            CountRequest(status);
            return status;
        }
        values.assign(static_cast<std::size_t>(info.inlines) * info.samples,
                      std::numeric_limits<float>::quiet_NaN());
        std::vector<float> trace;
        for(std::uint32_t il = 0; il < info.inlines; ++il) {
            if(cancel != nullptr && cancel->IsCancelled()) {
                const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
                CountRequest(status);
                return status;
            }
            if(!reader_.ReadTrace(info.inlineMin + static_cast<int>(il), index, trace, error)) {
                continue;
            }
            for(std::uint32_t s = 0; s < info.samples && s < trace.size(); ++s) {
                // SEG-Y display convention: row = samples-1-sample, column = inline index.
                const std::size_t row = static_cast<std::size_t>(info.samples - 1 - s);
                values[row * info.inlines + il] = trace[s];
            }
        }
        ok = true;
    } else {
        ok = reader_.ReadTimeSlice(index, values, error);
    }
    if(!ok) {
        const Status status = Status::Error(StatusCode::IoError, error);
        CountRequest(status);
        return status;
    }
    const WorkspaceInfo& info = reader_.Info();
    out = Slice2D{};
    out.values = std::move(values);
    if(mode == 0) {
        out.width = static_cast<int>(info.xlines);
        out.height = static_cast<int>(info.samples);
    } else if(mode == 1) {
        out.width = static_cast<int>(info.inlines);
        out.height = static_cast<int>(info.samples);
    } else {
        out.width = static_cast<int>(info.xlines);
        out.height = static_cast<int>(info.inlines);
    }
    out.totalColumns = out.width;
    out.columnsRead = out.width;
    for(float value : out.values) {
        if(std::isnan(value)) {
            continue;
        }
        if(out.valueMin == 0.0f && out.valueMax == 0.0f) {
            out.valueMin = value;
            out.valueMax = value;
        } else {
            out.valueMin = std::min(out.valueMin, value);
            out.valueMax = std::max(out.valueMax, value);
        }
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.requests;
        stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, out.width));
        stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    }
    return Status::Ok();
}

Status WorkspaceVolumeSource::ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel, int maxColumns) {
    return ReadAxisSlice(0, inlineNo, out, cancel, maxColumns);
}

Status WorkspaceVolumeSource::ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel, int maxColumns) {
    return ReadAxisSlice(1, xlineNo, out, cancel, maxColumns);
}

Status WorkspaceVolumeSource::ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) {
    return ReadAxisSlice(2, sampleIndex, out, cancel, 0);
}

Status WorkspaceVolumeSource::ReadTrace(int inlineNo, int xlineNo, TraceData& out, CancelToken* cancel) {
    if(cancel != nullptr && cancel->IsCancelled()) {
        const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
        CountRequest(status);
        return status;
    }
    std::string error;
    if(!reader_.ReadTrace(inlineNo, xlineNo, out.samples, error)) {
        const Status status = Status::Error(StatusCode::NotFound, error);
        CountRequest(status);
        return status;
    }
    out.sampleCount = static_cast<int>(out.samples.size());
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.requests;
    ++stats_.tracesRead;
    return Status::Ok();
}

Status WorkspaceVolumeSource::ReadVoxelWindow(
    const VoxelWindowRequest& request, VoxelWindow& out, CancelToken* cancel) {
    if(request.inlineCount <= 0 || request.xlineCount <= 0 || request.sampleCount <= 0) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "the voxel window is empty");
        CountRequest(status);
        return status;
    }
    std::vector<float> trace;
    std::string error;
    VoxelWindow window;
    window.box = request;
    window.values.assign(static_cast<std::size_t>(request.inlineCount) * request.xlineCount *
                             request.sampleCount,
                         std::numeric_limits<float>::quiet_NaN());
    for(int il = 0; il < request.inlineCount; ++il) {
        for(int xl = 0; xl < request.xlineCount; ++xl) {
            if(cancel != nullptr && cancel->IsCancelled()) {
                const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
                CountRequest(status);
                return status;
            }
            if(!reader_.ReadTrace(request.inlineBegin + il, request.xlineBegin + xl, trace, error)) {
                continue;
            }
            for(int s = 0; s < request.sampleCount; ++s) {
                const int sample = request.sampleBegin + s;
                if(sample < 0 || sample >= static_cast<int>(trace.size())) {
                    continue;
                }
                window.values[(static_cast<std::size_t>(il) * request.xlineCount + xl) *
                                  request.sampleCount + s] = trace[static_cast<std::size_t>(sample)];
            }
        }
    }
    out = std::move(window);
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.requests;
    stats_.tracesRead += static_cast<std::uint64_t>(request.inlineCount * request.xlineCount);
    return Status::Ok();
}

Status WorkspaceVolumeSource::ReadArbitrarySection(
    const SectionRequest& request, Slice2D& out, CancelToken* cancel) {
    if(request.pathPoints.size() < 2) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "a section needs at least two points");
        CountRequest(status);
        return status;
    }
    const WorkspaceInfo& info = reader_.Info();
    const int columns = std::clamp(request.maxColumns, 2, 8192);
    // Section cache: the key covers the path, interpolation, resolution and the
    // LOD algorithm version (variant hash), so a different request variant never
    // reuses a stale section.
    std::uint64_t variant = 1469598103934665603ull;
    for(const PathPoint& point : request.pathPoints) {
        variant ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(point.inlineNo));
        variant *= 1099511628211ull;
        variant ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(point.xlineNo));
        variant *= 1099511628211ull;
    }
    variant ^= request.interpolate ? 1ull : 0ull;
    variant ^= (static_cast<std::uint64_t>(columns) << 8);
    variant ^= request.keepOutsideColumns ? (1ull << 40) : 0ull;
    variant ^= (static_cast<std::uint64_t>(kLodAlgorithmVersion) << 48);
    const SliceCacheKey sectionKey{reader_.CacheSourceKey(), info.lodLevel, 4, 0, variant};
    if(std::shared_ptr<SliceCache> cache = reader_.SliceCacheHandle()) {
        if(std::shared_ptr<const Slice2D> cached = cache->Find(sectionKey)) {
            out = *cached;
            CountRequest(Status::Ok());
            return Status::Ok();
        }
    }
    // Distance-uniform sampling along the path.
    std::vector<float> segmentLengths(request.pathPoints.size() - 1, 0.0f);
    float totalLength = 0.0f;
    for(std::size_t i = 1; i < request.pathPoints.size(); ++i) {
        const float di = static_cast<float>(request.pathPoints[i].inlineNo - request.pathPoints[i - 1].inlineNo);
        const float dx = static_cast<float>(request.pathPoints[i].xlineNo - request.pathPoints[i - 1].xlineNo);
        const float length = std::sqrt(di * di + dx * dx);
        segmentLengths[i - 1] = length;
        totalLength += length;
    }
    if(totalLength <= 1e-4f) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "the section path is too short");
        CountRequest(status);
        return status;
    }
    const int naturalColumns = static_cast<int>(std::round(totalLength)) + 1;
    const int usedColumns = std::clamp(std::min(columns, naturalColumns), 2, columns);

    out = Slice2D{};
    out.width = usedColumns;
    out.height = static_cast<int>(info.samples);
    out.values.assign(static_cast<std::size_t>(usedColumns) * info.samples,
                      std::numeric_limits<float>::quiet_NaN());
    out.distances.resize(static_cast<std::size_t>(usedColumns));
    out.inlineNos.resize(static_cast<std::size_t>(usedColumns));
    out.xlineNos.resize(static_cast<std::size_t>(usedColumns));
    out.traceIndices.assign(static_cast<std::size_t>(usedColumns), -1);
    out.validMask.assign(out.values.size(), 0u);

    std::size_t segmentIndex = 0;
    float segmentStart = 0.0f;
    std::vector<float> trace;
    std::string error;
    for(int column = 0; column < usedColumns; ++column) {
        if(cancel != nullptr && cancel->IsCancelled()) {
            const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
            CountRequest(status);
            return status;
        }
        const float targetDistance = (static_cast<float>(column) / static_cast<float>(usedColumns - 1)) * totalLength;
        while(segmentIndex + 1 < segmentLengths.size() && targetDistance > segmentStart + segmentLengths[segmentIndex]) {
            segmentStart += segmentLengths[segmentIndex];
            ++segmentIndex;
        }
        const float segmentLength = std::max(segmentLengths[segmentIndex], 1e-4f);
        const float t = std::clamp((targetDistance - segmentStart) / segmentLength, 0.0f, 1.0f);
        const PathPoint& a = request.pathPoints[segmentIndex];
        const PathPoint& b = request.pathPoints[segmentIndex + 1];
        const float inlineValue = static_cast<float>(a.inlineNo) + static_cast<float>(b.inlineNo - a.inlineNo) * t;
        const float xlineValue = static_cast<float>(a.xlineNo) + static_cast<float>(b.xlineNo - a.xlineNo) * t;
        out.distances[static_cast<std::size_t>(column)] = targetDistance;
        out.inlineNos[static_cast<std::size_t>(column)] = inlineValue;
        out.xlineNos[static_cast<std::size_t>(column)] = xlineValue;
        const int inlineNo = static_cast<int>(std::lround(inlineValue));
        const int xlineNo = static_cast<int>(std::lround(xlineValue));
        if(!reader_.ReadTrace(inlineNo, xlineNo, trace, error)) {
            continue;
        }
        out.traceIndices[static_cast<std::size_t>(column)] = 0; // ordinal is not exposed by the workspace
        for(std::uint32_t s = 0; s < info.samples && s < trace.size(); ++s) {
            const int row = static_cast<int>(info.samples) - 1 - static_cast<int>(s);
            out.values[static_cast<std::size_t>(row) * usedColumns + static_cast<std::size_t>(column)] =
                trace[s];
            out.validMask[static_cast<std::size_t>(row) * usedColumns + static_cast<std::size_t>(column)] = 1u;
        }
    }
    out.columnsRead = usedColumns;
    out.totalColumns = usedColumns;
    if(std::shared_ptr<SliceCache> cache = reader_.SliceCacheHandle()) {
        cache->Insert(sectionKey, std::make_shared<const Slice2D>(out));
    }
    CountRequest(Status::Ok());
    return Status::Ok();
}

SourceStatistics WorkspaceVolumeSource::Statistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void WorkspaceVolumeSource::ResetStatistics() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = SourceStatistics{};
}

} // namespace engine
} // namespace seismic