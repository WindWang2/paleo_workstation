#pragma once

#include <memory>
#include <mutex>

#include "Data/Sgy/SgyCoordinateMapper.h"
#include "Data/Sgy/SgyVolume.h"
#include "Engine/StorageProfile.h"
#include "Engine/VolumeSource.h"

namespace seismic {
namespace engine {

// Direct-mode backend: SEG-Y + persistent index, no working format.
class SgyVolumeSource final : public IVolumeSource {
public:
    // Opens read-only and builds/adopts the persistent index. Large files keep
    // using SgyIndexJob in the Viewer; this entry point is synchronous.
    static std::unique_ptr<SgyVolumeSource> Open(const std::filesystem::path& path, Status& status);

    // Adopts an already loaded volume (background index or tests).
    explicit SgyVolumeSource(SgyVolume volume, bool compactPreview = false);

    const DatasetMetadata& Metadata() const override;

    Status ReadTrace(int inlineNo, int xlineNo, TraceData& out, CancelToken* cancel) override;
    Status ReadInline(int inlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) override;
    Status ReadCrossline(int xlineNo, Slice2D& out, CancelToken* cancel, int maxColumns = 0) override;
    Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel) override;
    Status ReadTimeSlice(int sampleIndex, Slice2D& out, CancelToken* cancel,
                         const ProgressFn& progress) override;
    Status ReadTimeSlicePreview(int sampleIndex, int maxInlineRows, int maxXlineColumns,
                                Slice2D& out, CancelToken* cancel,
                                const ProgressFn& progress = {},
                                SgyTimePreviewCache* cache = nullptr);
    Status ReadVoxelWindow(const VoxelWindowRequest& request, VoxelWindow& out, CancelToken* cancel) override;
    Status ReadArbitrarySection(const SectionRequest& request, Slice2D& out, CancelToken* cancel) override;

    SourceStatistics Statistics() const override;
    void ResetStatistics() override;

    const SgyVolume& Volume() const { return volume_; }

    // Device-adaptive I/O policy. When a section request leaves mergeGapTraces
    // at 0 the profile decides how aggressively ranges are merged (HDD: large
    // gap -> fewer seeks; NVMe: small gap -> fewer extra bytes).
    void SetIoProfile(const IoProfile& profile) { ioProfile_ = profile; }
    const IoProfile& IoProfileRef() const { return ioProfile_; }

private:
    Status ReadAxisSlice(SgySliceType type, int index, Slice2D& out, CancelToken* cancel, int maxColumns);
    void CountRequest(const Status& status);

    const SgyCoordinateMapper* Mapper() const;

    SgyVolume volume_;
    bool compactPreview_ = false;
    DatasetMetadata metadata_;
    IoProfile ioProfile_;
    mutable std::mutex mapperMutex_;
    mutable std::unique_ptr<SgyCoordinateMapper> mapper_;
    mutable bool mapperBuilt_ = false;
    mutable std::mutex statsMutex_;
    SourceStatistics stats_;
};

} // namespace engine
} // namespace seismic
