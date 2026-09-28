#include "Engine/SgyVolumeSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "Data/Sgy/SgyIndexCache.h"
#include "Data/Sgy/SgyIndexService.h"
#include "Engine/StorageProfile.h"
#include "Data/Sgy/SgyReadSession.h"
#include "Data/Sgy/SgySectionBuilder.h"
#include "Engine/PlannedSection.h"

namespace seismic {
namespace engine {

const char* StatusName(StatusCode code) {
    switch(code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::NotIndexed: return "NotIndexed";
        case StatusCode::Cancelled: return "Cancelled";
        case StatusCode::IoError: return "IoError";
        case StatusCode::Unsupported: return "Unsupported";
        case StatusCode::Internal: return "Internal";
    }
    return "Unknown";
}

namespace {

double MicrosSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

bool Cancelled(const CancelToken* cancel) {
    return cancel != nullptr && cancel->IsCancelled();
}

Slice2D ToSlice2D(SgySliceImage&& image) {
    Slice2D out;
    out.width = image.width;
    out.height = image.height;
    out.valueMin = image.valueMin;
    out.valueMax = image.valueMax;
    out.values = std::move(image.values);
    out.rgba = std::move(image.rgba);
    return out;
}

} // namespace

SgyVolumeSource::SgyVolumeSource(SgyVolume volume, bool compactPreview)
    : volume_(std::move(volume)), compactPreview_(compactPreview) {
    const SgyIndexPtr index = volume_.Index();
    if(index) {
        metadata_.name = index->path.filename().u8string();
        metadata_.path = index->path;
        metadata_.fileSize = index->fileSize;
        metadata_.traceCount = index->traceCount;
        metadata_.sampleCount = index->sampleCount;
        metadata_.sampleIntervalUs = index->sampleIntervalUs;
        metadata_.inlineMin = index->inlineMin;
        metadata_.inlineMax = index->inlineMax;
        metadata_.xlineMin = index->xlineMin;
        metadata_.xlineMax = index->xlineMax;
        metadata_.ruleBased = index->ruleBased;
        metadata_.indexComplete = index->complete;
    // Exact axis models from the index: a verified rule grid keeps its real step
    // (e.g. inline step 2), otherwise the explicit sorted line values are used.
    metadata_.inlineAxis = index->InlineAxis();
    metadata_.xlineAxis = index->XlineAxis();
    }
}

std::unique_ptr<SgyVolumeSource> SgyVolumeSource::Open(
    const std::filesystem::path& path, Status& status) {
    // One production strategy for every caller: the shared index service loads a
    // valid persistent index, otherwise scans sequentially with device-adaptive
    // parameters, and only falls back to the strided indexer for unsupported
    // layouts. The checkpoint lives in the index cache directory, never next to
    // the source.
    const engine::StorageProfileSet profiles = engine::ResolveStorageProfiles(
        path, std::filesystem::current_path(), SgyIndexCache::CacheDirectory());
    SgyIndexBuildRequest request;
    request.path = path;
    request.windowBytes = std::max<std::size_t>(profiles.source.sequentialBlockBytes, 64u << 10);
    request.readQueueDepth = static_cast<int>(std::max<std::uint32_t>(1, profiles.source.readQueueDepth));
    request.checkpointIntervalBytes = profiles.source.checkpointIntervalBytes;
    request.checkpointPath = SgyIndexCache::CacheDirectory() / "scan-checkpoints" /
                             (path.filename().u8string() + ".ckpt");
    const SgyIndexBuildOutcome outcome = BuildSgyIndexAuto(request);
    if(!outcome.index) {
        status = Status::Error(StatusCode::IoError,
                               outcome.message.empty() ? "failed to open the SEG-Y file" : outcome.message);
        return nullptr;
    }
    SgyVolume volume;
    volume.AdoptIndex(outcome.index);
    status = Status::Ok();
    return std::make_unique<SgyVolumeSource>(std::move(volume));
}

const DatasetMetadata& SgyVolumeSource::Metadata() const {
    return metadata_;
}

void SgyVolumeSource::CountRequest(const Status& status) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.requests;
    if(status.code == StatusCode::Cancelled) {
        ++stats_.cancelledRequests;
    }
}

Status SgyVolumeSource::ReadAxisSlice(
    SgySliceType type, int index, Slice2D& out, CancelToken* cancel, int maxColumns) {
    if(!volume_.IsLoaded()) {
        const Status status = Status::Error(StatusCode::NotIndexed, "the volume index is not complete");
        CountRequest(status);
        return status;
    }
    if(maxColumns < 0) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "maxColumns must not be negative");
        CountRequest(status);
        return status;
    }
    // Exact coordinate gate: a missing inline/xline is NotFound and must never
    // be substituted by its neighbour.
    if(type == SgySliceType::Inline) {
        int exact = 0;
        if(!volume_.ExactInlineValue(index, exact)) {
            const Status status = Status::Error(StatusCode::NotFound,
                                                "inline " + std::to_string(index) + " is not present");
            CountRequest(status);
            return status;
        }
    } else if(type == SgySliceType::Xline) {
        int exact = 0;
        if(!volume_.ExactXlineValue(index, exact)) {
            const Status status = Status::Error(StatusCode::NotFound,
                                                "xline " + std::to_string(index) + " is not present");
            CountRequest(status);
            return status;
        }
    } else if(index < 0 || index >= volume_.SampleCount()) {
        const Status status = Status::Error(StatusCode::NotFound,
                                            "sample " + std::to_string(index) + " is outside the volume");
        CountRequest(status);
        return status;
    }
    const auto start = std::chrono::steady_clock::now();
    SgySliceImage image;
    std::string error;
    int columnsRead = 0;
    int totalColumns = 0;
    bool ok = false;
    if(maxColumns > 0) {
        ok = volume_.ExtractPreviewSlice(type, index, maxColumns, image, error, columnsRead, totalColumns,
            compactPreview_, [cancel](int, int) { return !Cancelled(cancel); });
    } else {
        ok = volume_.ExtractSlice(type, index, image, error,
                                  [cancel](int, int) { return !Cancelled(cancel); });
    }
    if(!ok) {
        const bool wasCancelled = Cancelled(cancel);
        const Status status = Status::Error(
            wasCancelled ? StatusCode::Cancelled : StatusCode::IoError,
            error.empty() ? (wasCancelled ? "request cancelled" : "slice extraction failed") : error);
        CountRequest(status);
        return status;
    }
    out = ToSlice2D(std::move(image));
    out.columnsRead = maxColumns > 0 ? columnsRead : out.width;
    out.totalColumns = maxColumns > 0 ? totalColumns : out.width;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.requests;
        stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, out.width));
        stats_.decodeMicros += static_cast<std::uint64_t>(MicrosSince(start));
    }
    return Status::Ok();
}

Status SgyVolumeSource::ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel, int maxColumns) {
    return ReadAxisSlice(SgySliceType::Inline, inlineNo, out, cancel, maxColumns);
}

Status SgyVolumeSource::ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel, int maxColumns) {
    return ReadAxisSlice(SgySliceType::Xline, xlineNo, out, cancel, maxColumns);
}

Status SgyVolumeSource::ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) {
    return ReadAxisSlice(SgySliceType::Time, sampleIndex, out, cancel, 0);
}

Status SgyVolumeSource::ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel,
                                      const ProgressFn& progress) {
    if(!volume_.IsLoaded()) {
        const Status status = Status::Error(StatusCode::NotIndexed, "the volume index is not complete");
        CountRequest(status);
        return status;
    }
    const auto start = std::chrono::steady_clock::now();
    SgySliceImage image;
    std::string error;
    const bool ok = volume_.ExtractSlice(
        SgySliceType::Time, sampleIndex, image, error,
        [cancel, &progress](int processed, int total) {
            if(progress && !progress(processed, total)) {
                return false;
            }
            return !Cancelled(cancel);
        });
    if(!ok) {
        const bool wasCancelled = Cancelled(cancel);
        const Status status = Status::Error(
            wasCancelled ? StatusCode::Cancelled : StatusCode::IoError,
            error.empty() ? (wasCancelled ? "request cancelled" : "slice extraction failed") : error);
        CountRequest(status);
        return status;
    }
    out = ToSlice2D(std::move(image));
    out.columnsRead = out.width;
    out.totalColumns = out.width;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.requests;
        stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, out.width));
        stats_.decodeMicros += static_cast<std::uint64_t>(MicrosSince(start));
    }
    return Status::Ok();
}

Status SgyVolumeSource::ReadTimeSlicePreview(
    int sampleIndex,
    int maxInlineRows,
    int maxXlineColumns,
    Slice2D& out,
    CancelToken* cancel,
    const ProgressFn& progress,
    SgyTimePreviewCache* cache) {
    if(!volume_.IsLoaded()) {
        const Status status = Status::Error(StatusCode::NotIndexed, "the volume index is not complete");
        CountRequest(status);
        return status;
    }
    if(sampleIndex < 0 || sampleIndex >= volume_.SampleCount() ||
       maxInlineRows <= 0 || maxXlineColumns <= 0) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "invalid time-slice preview request");
        CountRequest(status);
        return status;
    }

    const auto start = std::chrono::steady_clock::now();
    SgySliceImage image;
    std::string error;
    const bool ok = volume_.ExtractTimeSlicePreview(
        sampleIndex, maxInlineRows, maxXlineColumns, image, error,
        [cancel, &progress](int processed, int total) {
            if(progress && !progress(processed, total)) {
                return false;
            }
            return !Cancelled(cancel);
        }, cache);
    if(!ok) {
        const bool wasCancelled = Cancelled(cancel);
        const Status status = Status::Error(
            wasCancelled ? StatusCode::Cancelled : StatusCode::IoError,
            error.empty() ? (wasCancelled ? "request cancelled" : "time-slice preview failed") : error);
        CountRequest(status);
        return status;
    }
    out = ToSlice2D(std::move(image));
    out.columnsRead = out.width;
    out.totalColumns = out.width;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.requests;
        stats_.tracesRead += static_cast<std::uint64_t>(
            std::max(0, out.width) * std::max(0, out.height));
        stats_.decodeMicros += static_cast<std::uint64_t>(MicrosSince(start));
    }
    return Status::Ok();
}

Status SgyVolumeSource::ReadTrace(int inlineNo, int xlineNo, TraceData& out, CancelToken* cancel) {
    if(!volume_.IsLoaded()) {
        const Status status = Status::Error(StatusCode::NotIndexed, "the volume index is not complete");
        CountRequest(status);
        return status;
    }
    if(Cancelled(cancel)) {
        const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
        CountRequest(status);
        return status;
    }
    const auto start = std::chrono::steady_clock::now();
    SgyReadSession session;
    std::string error;
    if(!session.Open(volume_.Index(), error)) {
        const Status status = Status::Error(StatusCode::IoError, error);
        CountRequest(status);
        return status;
    }
    // Exact coordinates only: a trace that is not present must be NotFound, not
    // a neighbour (the Viewer may snap its selection explicitly, not the read).
    int exactInline = 0;
    int exactXline = 0;
    if(!volume_.ExactInlineValue(inlineNo, exactInline) || !volume_.ExactXlineValue(xlineNo, exactXline)) {
        const Status status = Status::Error(StatusCode::NotFound,
                                            "inline/xline is not present in this file");
        CountRequest(status);
        return status;
    }
    const int traceIndex = volume_.FindTraceIndex(exactInline, exactXline);
    if(traceIndex < 0) {
        const Status status = Status::Error(StatusCode::NotFound, "no trace for the requested inline/xline");
        CountRequest(status);
        return status;
    }
    std::vector<float> samples;
    if(!session.ReadTrace(traceIndex, samples, error)) {
        const Status status = Status::Error(StatusCode::IoError, error);
        CountRequest(status);
        return status;
    }
    out.samples = std::move(samples);
    out.sampleCount = static_cast<int>(out.samples.size());
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.requests;
        ++stats_.tracesRead;
        stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    }
    return Status::Ok();
}

Status SgyVolumeSource::ReadVoxelWindow(
    const VoxelWindowRequest& request, VoxelWindow& out, CancelToken* cancel) {
    if(!volume_.IsLoaded()) {
        const Status status = Status::Error(StatusCode::NotIndexed, "the volume index is not complete");
        CountRequest(status);
        return status;
    }
    if(request.inlineCount <= 0 || request.xlineCount <= 0 || request.sampleCount <= 0) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "the voxel window is empty");
        CountRequest(status);
        return status;
    }
    if(request.sampleBegin < 0 || request.sampleBegin + request.sampleCount > volume_.SampleCount()) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "the sample range is outside the volume");
        CountRequest(status);
        return status;
    }
    const auto start = std::chrono::steady_clock::now();
    SgyReadSession session;
    std::string error;
    if(!session.Open(volume_.Index(), error)) {
        const Status status = Status::Error(StatusCode::IoError, error);
        CountRequest(status);
        return status;
    }
    const std::size_t total = static_cast<std::size_t>(request.inlineCount) *
                              static_cast<std::size_t>(request.xlineCount) *
                              static_cast<std::size_t>(request.sampleCount);
    VoxelWindow window;
    window.box = request;
    window.values.assign(total, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> traceSamples;
    for(int il = 0; il < request.inlineCount; ++il) {
        for(int xl = 0; xl < request.xlineCount; ++xl) {
            if(Cancelled(cancel)) {
                const Status status = Status::Error(StatusCode::Cancelled, "request cancelled");
                CountRequest(status);
                return status;
            }
            const int inlineNo = request.inlineBegin + il;
            const int xlineNo = request.xlineBegin + xl;
            const int nearestInline = volume_.FindNearestInlineValue(static_cast<float>(inlineNo));
            const int nearestXline = volume_.FindNearestXlineValue(static_cast<float>(xlineNo));
            const int traceIndex = volume_.FindTraceIndex(nearestInline, nearestXline);
            if(traceIndex < 0) {
                continue; // missing traces stay NaN, never zero amplitude
            }
            if(!session.ReadTrace(traceIndex, traceSamples, error)) {
                continue;
            }
            for(int s = 0; s < request.sampleCount; ++s) {
                const int sampleIndex = request.sampleBegin + s;
                if(sampleIndex < 0 || sampleIndex >= static_cast<int>(traceSamples.size())) {
                    continue;
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(il) * request.xlineCount + xl) * request.sampleCount + s;
                window.values[offset] = traceSamples[static_cast<std::size_t>(sampleIndex)];
            }
        }
    }
    out = std::move(window);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.requests;
        stats_.tracesRead += static_cast<std::uint64_t>(request.inlineCount * request.xlineCount);
        stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
    }
    return Status::Ok();
}

Status SgyVolumeSource::ReadArbitrarySection(
    const SectionRequest& request, Slice2D& out, CancelToken* cancel) {
    if(!volume_.IsLoaded()) {
        const Status status = Status::Error(StatusCode::NotIndexed, "the volume index is not complete");
        CountRequest(status);
        return status;
    }
    if(request.pathPoints.size() < 2) {
        const Status status = Status::Error(StatusCode::InvalidArgument, "a section needs at least two points");
        CountRequest(status);
        return status;
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<glm::ivec2> path;
    path.reserve(request.pathPoints.size());
    for(const PathPoint& point : request.pathPoints) {
        path.emplace_back(point.inlineNo, point.xlineNo);
    }
    SgySectionOptions options;
    options.maxColumns = request.maxColumns;
    options.interpolate = request.interpolate;
    options.keepOutsideColumns = request.keepOutsideColumns;
    SgySliceImage image;
    std::string error;
    int uniqueTraces = 0;
    int columns = 0;
    std::vector<float> columnDistances;

    if(request.useReadPlan && !request.interpolate) {
        // Stage C planned path: dedupe + ascending order + optional range merge.
        ReadPlanOptions planOptions;
        planOptions.maxGapTraces = std::max(0, request.mergeGapTraces);
        if(planOptions.maxGapTraces == 0 && ioProfile_.mergeGapBytes > 0) {
            // Auto: derive the gap tolerance from the device profile and the
            // real trace size so HDDs merge more while NVMe stays tight.
            const std::int64_t traceBytes =
                240 + static_cast<std::int64_t>(metadata_.sampleCount) *
                          std::max(1, volume_.Index() ? volume_.Index()->formatSizeBytes : 4);
            if(traceBytes > 0) {
                const std::int64_t gapTraces =
                    static_cast<std::int64_t>(ioProfile_.mergeGapBytes) / traceBytes;
                planOptions.maxGapTraces = static_cast<int>(std::clamp<std::int64_t>(gapTraces, 0, 64));
            }
        }
        PlannedSectionStats plannedStats;
        PlannedSectionColumns plannedColumns;
        const bool ok = BuildPlannedLineSection(
            volume_, path, options, planOptions, image, plannedColumns, plannedStats, error, cancel,
            [cancel](int, int) { return !Cancelled(cancel); },
            request.includeXY ? Mapper() : nullptr);
        if(!ok) {
            const bool wasCancelled = Cancelled(cancel);
            const Status status = Status::Error(
                wasCancelled ? StatusCode::Cancelled : StatusCode::IoError,
                error.empty() ? (wasCancelled ? "request cancelled" : "section build failed") : error);
            CountRequest(status);
            return status;
        }
        uniqueTraces = plannedStats.uniqueTraces;
        columns = plannedStats.columns;
        columnDistances = plannedStats.columnDistances;
        out = ToSlice2D(std::move(image));
        out.sectionTracesRead = static_cast<int>(plannedStats.tracesRead);
        out.sectionDuplicateColumns = plannedStats.duplicateColumns;
        out.sectionMissingColumns = plannedStats.missingColumns;
        out.traceIndices = std::move(plannedColumns.traceIndices);
        out.inlineNos = std::move(plannedColumns.inlineNos);
        out.xlineNos = std::move(plannedColumns.xlineNos);
        out.xyX = std::move(plannedColumns.xyX);
        out.xyY = std::move(plannedColumns.xyY);
        out.planReadRanges = static_cast<std::uint64_t>(plannedStats.readRanges);
        out.planUniqueTraces = static_cast<std::uint64_t>(plannedStats.uniqueTraces);
        out.planBytesRead = plannedStats.bytesRead;
        out.planValidBytes = plannedStats.validBytes;
        out.planReadAmplification = plannedStats.readAmplification;
        out.planIoMicros = plannedStats.ioMicros;
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.requests;
            stats_.tracesRead += static_cast<std::uint64_t>(plannedStats.tracesRead);
            stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
        }
    } else {
        // Legacy reference path (kept for A/B comparison; also handles interpolation).
        SgySectionStats legacyStats;
        const bool ok = BuildLineSection(
            volume_, path, options, image, legacyStats, error,
            [cancel](int, int) { return !Cancelled(cancel); });
        if(!ok) {
            const bool wasCancelled = Cancelled(cancel);
            const Status status = Status::Error(
                wasCancelled ? StatusCode::Cancelled : StatusCode::IoError,
                error.empty() ? (wasCancelled ? "request cancelled" : "section build failed") : error);
            CountRequest(status);
            return status;
        }
        uniqueTraces = legacyStats.uniqueTraces;
        columns = legacyStats.columns;
        columnDistances = legacyStats.columnDistances;
        out = ToSlice2D(std::move(image));
        out.sectionTracesRead = legacyStats.tracesRead;
        out.sectionDuplicateColumns = legacyStats.duplicateColumns;
        out.sectionMissingColumns = legacyStats.missingColumns;
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.requests;
            stats_.tracesRead += static_cast<std::uint64_t>(std::max(0, legacyStats.uniqueTraces));
            stats_.ioMicros += static_cast<std::uint64_t>(MicrosSince(start));
        }
    }

    out.distances = std::move(columnDistances);
    out.columnsRead = columns;
    out.totalColumns = columns;
    out.validMask.reserve(out.values.size());
    for(float value : out.values) {
        out.validMask.push_back(std::isnan(value) ? 0u : 1u);
    }
    (void)uniqueTraces;
    return Status::Ok();
}

const SgyCoordinateMapper* SgyVolumeSource::Mapper() const {
    std::lock_guard<std::mutex> lock(mapperMutex_);
    if(!mapperBuilt_) {
        mapperBuilt_ = true;
        if(volume_.Index()) {
            SgyCoordinateMapper candidate = SgyCoordinateMapper::Fit(*volume_.Index());
            if(candidate.valid()) {
                mapper_ = std::make_unique<SgyCoordinateMapper>(std::move(candidate));
            }
        }
    }
    return mapper_.get();
}

SourceStatistics SgyVolumeSource::Statistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void SgyVolumeSource::ResetStatistics() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = SourceStatistics{};
}

} // namespace engine
} // namespace seismic
