#pragma once

#include <memory>
#include <mutex>

#include "Engine/VolumeSource.h"
#include "Engine/WorkspaceFormat.h"

namespace seismic {
namespace engine {

// Optimized-mode backend: reads slices and traces from a native workspace.
// Chunk/slice caches are optional and can be attached by the caller.
class WorkspaceVolumeSource final : public IVolumeSource {
public:
    static std::unique_ptr<WorkspaceVolumeSource> Open(const std::filesystem::path& basePath, Status& status);

    const DatasetMetadata& Metadata() const override;

    Status ReadTrace(int inlineNo, int xlineNo, TraceData& out, CancelToken* cancel) override;
    Status ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) override;
    Status ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) override;
    Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) override;
    Status ReadVoxelWindow(const VoxelWindowRequest& request, VoxelWindow& out, CancelToken* cancel) override;
    Status ReadArbitrarySection(const SectionRequest& request, Slice2D& out, CancelToken* cancel) override;

    SourceStatistics Statistics() const override;
    void ResetStatistics() override;

    WorkspaceReader& Reader() { return reader_; }
    const WorkspaceReader& Reader() const { return reader_; }

private:
    Status ReadAxisSlice(int mode, int index, Slice2D& out, CancelToken* cancel, int maxColumns);
    void CountRequest(const Status& status);

    WorkspaceReader reader_;
    DatasetMetadata metadata_;
    mutable std::mutex statsMutex_;
    SourceStatistics stats_;
};

} // namespace engine
} // namespace seismic