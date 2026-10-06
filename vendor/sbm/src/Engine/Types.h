#pragma once

#include <atomic>
#include <cstdint>

#include "Data/Sgy/AxisDescriptor.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace seismic {
namespace engine {

// Error model shared by the engine API. No exception crosses the boundary.
enum class StatusCode {
    Ok = 0,
    InvalidArgument,
    NotFound,
    NotIndexed,
    Cancelled,
    IoError,
    Unsupported,
    Internal,
};

struct Status {
    StatusCode code = StatusCode::Ok;
    std::string message;

    bool ok() const { return code == StatusCode::Ok; }
    static Status Ok() { return Status{}; }
    static Status Error(StatusCode code, std::string message) {
        Status status;
        status.code = code;
        status.message = std::move(message);
        return status;
    }
};

const char* StatusName(StatusCode code);

// Cooperative cancellation token. The caller owns it and the engine only polls
// it. A cancelled request never publishes a result.
class CancelToken {
public:
    void Cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    // Optional external predicate (e.g. an existing task cancellation flag).
    // Set it before starting the request; the token polls it on every check.
    void SetPredicate(std::function<bool()> predicate) { predicate_ = std::move(predicate); }
    bool IsCancelled() const {
        if(cancelled_.load(std::memory_order_relaxed)) {
            return true;
        }
        return predicate_ ? predicate_() : false;
    }

private:
    std::atomic<bool> cancelled_{false};
    std::function<bool()> predicate_;
};

// 2D amplitude grid. values are raw amplitudes; NaN marks missing data
// (missing trace, unreadable trace or a not-yet-read preview column).
struct Slice2D {
    int width = 0;
    int height = 0;
    float valueMin = 0.0f;
    float valueMax = 0.0f;
    std::vector<float> values;
    std::vector<unsigned char> rgba;
    // Section extras: empty for axis-aligned slices.
    std::vector<float> distances;
    std::vector<float> inlineNos;
    std::vector<float> xlineNos;
    std::vector<int> traceIndices; // per column, -1 = missing
    std::vector<float> xyX;        // optional (includeXY)
    std::vector<float> xyY;
    std::vector<unsigned char> validMask;
    int columnsRead = 0;
    int totalColumns = 0;
    // Section read statistics (filled by the section paths).
    int sectionTracesRead = 0;
    int sectionDuplicateColumns = 0;
    int sectionMissingColumns = 0;
    // Read-plan metrics (planned paths only; zero for legacy reads).
    std::uint64_t planReadRanges = 0;
    std::uint64_t planUniqueTraces = 0;
    std::uint64_t planBytesRead = 0;
    std::uint64_t planValidBytes = 0;
    double planReadAmplification = 0.0;
    double planIoMicros = 0.0;

    bool empty() const { return width <= 0 || height <= 0 || values.empty(); }
};

// Display-oriented tile coordinates; callback may cancel by returning false.
using TimeTileCallback = std::function<bool(int x, int y, Slice2D&& tile, int completed, int total)>;

struct TraceData {
    std::vector<float> samples;
    int sampleCount = 0;
};

// Bounded voxel box. values[((il - inlineBegin) * xlineCount + (xl - xlineBegin))
// * sampleCount + (s - sampleBegin)]; NaN = missing trace/sample.
struct VoxelWindowRequest {
    int inlineBegin = 0;
    int xlineBegin = 0;
    int sampleBegin = 0;
    int inlineCount = 0;
    int xlineCount = 0;
    int sampleCount = 0;
};

struct VoxelWindow {
    VoxelWindowRequest box;
    std::vector<float> values;

    bool empty() const { return values.empty(); }
};

struct PathPoint {
    int inlineNo = 0;
    int xlineNo = 0;
};

struct SectionRequest {
    std::vector<PathPoint> pathPoints;
    bool interpolate = false; // false: nearest trace per column
    bool keepOutsideColumns = false;
    int maxColumns = 2048;
    // Stage C: read the section through a ReadPlan (dedupe + ascending trace
    // order + optional range merge). Nearest-trace mode only; interpolation
    // falls back to the legacy builder.
    bool useReadPlan = true;
    // Merge ranges separated by at most this many unused traces (0 = strict).
    int mergeGapTraces = 0;
    // Fill per-column X/Y from the source's affine coordinate fit (Direct mode
    // only; requires trace coordinates in the SEG-Y).
    bool includeXY = false;
};

struct DatasetMetadata {
    // Exact axis models (origin/step/count or explicit values). Reads must use
    // ExactIndexOf so a missing coordinate is reported as missing.
    AxisDescriptor inlineAxis;
    AxisDescriptor xlineAxis;
    std::string name;
    std::filesystem::path path;
    std::uintmax_t fileSize = 0;
    std::int64_t traceCount = 0;
    int sampleCount = 0;
    int sampleIntervalUs = 0;
    int inlineMin = 0;
    int inlineMax = 0;
    int xlineMin = 0;
    int xlineMax = 0;
    bool ruleBased = false;
    bool indexComplete = false;
};

// Monotonic counters/timings for one source. Zero means "not measured yet".
struct SourceStatistics {
    std::uint64_t requests = 0;
    std::uint64_t cancelledRequests = 0;
    std::uint64_t tracesRead = 0;
    std::uint64_t sanitizedSampleReads = 0; // nonfinite decodes, including rereads
    std::uint64_t bytesRead = 0;
    std::uint64_t ioMicros = 0;
    std::uint64_t decodeMicros = 0;
    std::uint64_t resampleMicros = 0;
};

} // namespace engine
} // namespace seismic
