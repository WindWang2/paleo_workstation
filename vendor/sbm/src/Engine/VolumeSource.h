#pragma once

#include <functional>

#include "Engine/Types.h"

namespace seismic {
namespace engine {

// Uniform read interface over any seismic volume backend (Direct SEG-Y today,
// chunked/LOD workspaces later). Implementations must not require OpenGL or
// ImGui; the Viewer and upper platforms only see this interface.
//
// Threading: a source is not shared between worker threads unless a backend
// documents it; Direct SEG-Y handles are not thread safe, so callers either
// serialize access or open one source per task.
class IVolumeSource {
public:
    // Cooperative progress callback; returning false cancels the request.
    using ProgressFn = std::function<bool(int processed, int total)>;

    virtual ~IVolumeSource() = default;

    virtual const DatasetMetadata& Metadata() const = 0;

    virtual Status ReadTrace(int inlineNo, int xlineNo, TraceData& out, CancelToken* cancel) = 0;
    virtual Status ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) = 0;
    virtual Status ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) = 0;
    virtual Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) = 0;
    // Progress-aware variant (default: ignores progress, forwards to the plain
    // call). Backends that can report rows override it.
    virtual Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel,
                                 const ProgressFn& progress) {
        (void)progress;
        return ReadTimeSlice(sampleIndex, out, cancel);
    }
    virtual Status ReadVoxelWindow(const VoxelWindowRequest& request, VoxelWindow& out, CancelToken* cancel) = 0;
    virtual Status ReadArbitrarySection(const SectionRequest& request, Slice2D& out, CancelToken* cancel) = 0;

    virtual SourceStatistics Statistics() const = 0;
    virtual void ResetStatistics() = 0;
};

} // namespace engine
} // namespace seismic