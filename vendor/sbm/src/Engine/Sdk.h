#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "Engine/Cache.h"
#include "Engine/Types.h"
#include "Engine/WorkspaceFormat.h"

namespace seismic {
namespace engine {
class IVolumeSource;
} // namespace engine

namespace sdk {

// Stable C++ facade over the seismic engine (Stage I).
//
// The Viewer, tests and upper platforms (e.g. paleo-workbench) use this facade
// only; SEG-Y details, the workspace layout and the cache internals stay hidden.
//
// Contracts:
//   * ownership: Dataset owns its source; results own their buffers;
//   * lifetime: Open returns a shared_ptr; Close() is idempotent;
//   * threading: a Dataset is used by one thread at a time; long calls take a
//     cooperative CancelToken and never publish a cancelled result;
//   * errors: engine::Status with a StatusCode; no exceptions cross the API;
//   * ABI: kSdkAbiVersion identifies the facade layout.
constexpr std::uint32_t kSdkAbiVersion = 2;

enum class Backend {
    Auto = 0,      // workspace when <path>.sf3c.meta exists, otherwise Direct SEG-Y
    Direct = 1,    // SEG-Y + persistent index
    Workspace = 2, // native chunk/shard workspace
    Paged = 3,     // page-addressable v6 workspace (.sf3p)
};

struct OpenOptions {
    Backend backend = Backend::Auto;
    std::size_t chunkCacheBytes = 256ull * 1024ull * 1024ull; // 0 disables
    std::size_t sliceCacheBytes = 64ull * 1024ull * 1024ull;  // 0 disables
    // Progressive LOD: discover sibling LOD workspaces in the same directory
    // (same geometry and source identity, lodLevel > 0) and start at the
    // coarsest level so the first look is cheap; SetActiveLod refines later.
    bool progressiveLod = false;
    // Optional disposable nearby exact-Time cache (<=256 MiB plus atomic temporary).
    std::filesystem::path timeCachePath;
};

class Dataset {
public:
    static std::shared_ptr<Dataset> Open(
        const std::filesystem::path& path,
        const OpenOptions& options,
        engine::Status& status);
    ~Dataset();

    Dataset(const Dataset&) = delete;
    Dataset& operator=(const Dataset&) = delete;

    std::uint32_t AbiVersion() const { return kSdkAbiVersion; }
    bool IsOpen() const { return open_; }
    void Close();

    const engine::DatasetMetadata& Metadata() const;
    std::string GeometryDescription() const;

    engine::Status ReadInline(int inlineNo, engine::Slice2D& out, engine::CancelToken* cancel = nullptr,
                              int maxColumns = 0);
    engine::Status ReadCrossline(int xlineNo, engine::Slice2D& out, engine::CancelToken* cancel = nullptr,
                                 int maxColumns = 0);
    engine::Status ReadTimeSlice(int sampleIndex, engine::Slice2D& out, engine::CancelToken* cancel = nullptr);
    engine::Status ReadTimeSliceWindowed(int sampleIndex, engine::Slice2D& out, engine::CancelToken* cancel = nullptr);
    engine::Status ReadCachedTimeSlice(int sampleIndex, engine::Slice2D& out, engine::CancelToken* cancel = nullptr);
    engine::Status ReadTimeSliceTiled(int sampleIndex, int tileSize, int focusInline, int focusXline,
        const engine::TimeTileCallback& publish, engine::Slice2D& out, engine::CancelToken* cancel = nullptr);
    engine::Status ReadTrace(int inlineNo, int xlineNo, engine::TraceData& out, engine::CancelToken* cancel = nullptr);
    engine::Status ReadVoxelWindow(const engine::VoxelWindowRequest& request, engine::VoxelWindow& out,
                                   engine::CancelToken* cancel = nullptr);
    engine::Status ReadSection(const engine::SectionRequest& request, engine::Slice2D& out,
                               engine::CancelToken* cancel = nullptr);

    engine::SourceStatistics Statistics() const;
    engine::CacheStats ChunkCacheStats() const;
    engine::CacheStats SliceCacheStats() const;
    void ClearCaches();
    engine::Status SetCacheBudget(std::size_t chunkBytes, std::size_t sliceBytes);

    bool IsWorkspaceBackend() const { return workspaceBackend_; }
    bool IsPagedBackend() const { return pagedBackend_; }
    // "workspace", "paged-workspace" or "direct"; the direct backend is also reported when an Auto
    // request fell back because the workspace was missing or unusable.
    const char* BackendName() const {
        return pagedBackend_ ? "paged-workspace" : (workspaceBackend_ ? "workspace" : "direct");
    }
    bool FellBackToDirect() const { return fellBackToDirect_; }

    // Progressive LOD state (workspace backends only; empty otherwise).
    int LodLevelCount() const { return static_cast<int>(lodLevels_.size()); }
    int LodLevelAt(int index) const;
    std::uint64_t LodFactorSamplesAt(int index) const;
    int ActiveLodLevel() const { return activeLodLevel_; }
    // Reopens the backend at the given LOD level (0 = base). Caches are reset
    // because they are keyed by level. Returns an error for unknown levels.
    engine::Status SetActiveLod(int lodLevel);
    // "L0 full", "L1 2x2 samples=..." etc. for status labels.
    std::string QualityName() const;

private:
    struct LodEntry {
        int level = 0;
        std::filesystem::path base;
        std::uint64_t factorSamples = 1;
        std::uint64_t factorInlines = 1;
        std::uint64_t factorXlines = 1;
    };

    Dataset() = default;
    engine::WorkspaceReader& AsWorkspaceReader();
    engine::Status OpenWorkspaceAt(const std::filesystem::path& base, const OpenOptions& options);
    engine::Status OpenPagedAt(const std::filesystem::path& path, const OpenOptions& options);
    engine::Status OpenDirect(const std::filesystem::path& path);
    engine::Status DiscoverPagedLods(const std::filesystem::path& l0Path);
    const LodEntry* ActiveLodEntry() const;
    bool MapInline(int baseValue, int& activeValue) const;
    bool MapXline(int baseValue, int& activeValue) const;
    int MapSample(int baseSample) const;

    std::unique_ptr<engine::IVolumeSource> source_;
    std::shared_ptr<engine::ChunkCache> chunkCache_;
    std::shared_ptr<engine::SliceCache> sliceCache_;
    bool workspaceBackend_ = false;
    bool pagedBackend_ = false;
    bool fellBackToDirect_ = false;
    bool open_ = false;
    std::filesystem::path workspaceBase_;
    std::filesystem::path directPath_;
    std::vector<LodEntry> lodLevels_;
    int activeLodLevel_ = 0;
    OpenOptions options_;
    engine::DatasetMetadata baseMetadata_;
};

} // namespace sdk
} // namespace seismic
