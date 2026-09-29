#include "Engine/Sdk.h"
#include "Engine/TimePlaneCache.h"

#include <algorithm>
#include <cassert>

#include "Engine/Cache.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/SgyVolumeSource.h"
#include "Engine/VolumeSource.h"
#include "Engine/WorkspaceFormat.h"
#include "Engine/WorkspaceVolumeSource.h"

namespace seismic {
namespace sdk {

namespace {

engine::DatasetMetadata MetadataFromPagedInfo(
    const engine::PagedWorkspaceInfo& info, const std::filesystem::path& path) {
    engine::DatasetMetadata metadata;
    metadata.name = path.filename().u8string();
    metadata.path = path;
    metadata.sampleCount = static_cast<int>(info.samples);
    metadata.sampleIntervalUs = static_cast<int>(info.sampleIntervalUs);
    metadata.traceCount = static_cast<std::int64_t>(info.inlineAxis.count) * info.xlineAxis.count;
    metadata.inlineAxis = info.inlineAxis;
    metadata.xlineAxis = info.xlineAxis;
    metadata.inlineMin = info.inlineAxis.count > 0 ? info.inlineAxis.ValueAt(0) : 0;
    metadata.inlineMax = info.inlineAxis.count > 0 ? info.inlineAxis.ValueAt(info.inlineAxis.count - 1) : 0;
    metadata.xlineMin = info.xlineAxis.count > 0 ? info.xlineAxis.ValueAt(0) : 0;
    metadata.xlineMax = info.xlineAxis.count > 0 ? info.xlineAxis.ValueAt(info.xlineAxis.count - 1) : 0;
    metadata.ruleBased = false;
    metadata.indexComplete = true;
    return metadata;
}

} // namespace

engine::WorkspaceReader& Dataset::AsWorkspaceReader() {
    auto* workspace = dynamic_cast<seismic::engine::WorkspaceVolumeSource*>(source_.get());
    assert(workspace != nullptr && "the workspace reader is only valid for workspace backends");
    return workspace->Reader();
}

engine::Status Dataset::OpenWorkspaceAt(const std::filesystem::path& base, const OpenOptions& options) {
    engine::Status status;
    std::unique_ptr<seismic::engine::WorkspaceVolumeSource> source =
        seismic::engine::WorkspaceVolumeSource::Open(base, status);
    if(!source) {
        if(status.ok()) {
            status = engine::Status::Error(engine::StatusCode::IoError, "cannot open the workspace");
        }
        return status;
    }
    workspaceBackend_ = true;
    pagedBackend_ = false;
    source_ = std::move(source);
    if(options.chunkCacheBytes > 0) {
        if(!chunkCache_) {
            chunkCache_ = std::make_shared<seismic::engine::ChunkCache>(options.chunkCacheBytes);
        } else {
            chunkCache_->SetBudget(options.chunkCacheBytes);
        }
        AsWorkspaceReader().SetChunkCache(chunkCache_);
    }
    if(options.sliceCacheBytes > 0) {
        if(!sliceCache_) {
            sliceCache_ = std::make_shared<seismic::engine::SliceCache>(options.sliceCacheBytes);
        } else {
            sliceCache_->SetBudget(options.sliceCacheBytes);
        }
        AsWorkspaceReader().SetSliceCache(sliceCache_);
    }
    return engine::Status::Ok();
}

engine::Status Dataset::OpenPagedAt(const std::filesystem::path& path, const OpenOptions& options) {
    engine::Status status;
    std::unique_ptr<seismic::engine::PagedWorkspaceVolumeSource> source =
        seismic::engine::PagedWorkspaceVolumeSource::Open(path, status);
    if(!source) {
        if(status.ok()) {
            status = engine::Status::Error(engine::StatusCode::IoError,
                                           "cannot open the paged workspace");
        }
        return status;
    }
    workspaceBackend_ = true;
    pagedBackend_ = true;
    if(options.chunkCacheBytes > 0) {
        if(!chunkCache_) {
            chunkCache_ = std::make_shared<seismic::engine::ChunkCache>(options.chunkCacheBytes);
        } else {
            chunkCache_->SetBudget(options.chunkCacheBytes);
        }
        source->Reader().SetPageCache(chunkCache_);
    }
    if(source->Reader().Info().lodLevel == 0) source->SetTimeCachePath(options.timeCachePath);
    source_ = std::move(source);
    return engine::Status::Ok();
}

engine::Status Dataset::OpenDirect(const std::filesystem::path& path) {
    engine::Status status;
    std::unique_ptr<seismic::engine::SgyVolumeSource> source =
        seismic::engine::SgyVolumeSource::Open(path, status);
    if(!source) {
        if(status.ok()) {
            status = engine::Status::Error(engine::StatusCode::IoError, "cannot open the SEG-Y source");
        }
        return status;
    }
    workspaceBackend_ = false;
    pagedBackend_ = false;
    source_ = std::move(source);
    chunkCache_.reset();
    sliceCache_.reset();
    return engine::Status::Ok();
}

std::shared_ptr<Dataset> Dataset::Open(
    const std::filesystem::path& path,
    const OpenOptions& options,
    engine::Status& status) {
    auto dataset = std::shared_ptr<Dataset>(new Dataset());
    dataset->options_ = options;

    const bool paged = options.backend == Backend::Paged || path.extension() == ".sf3p";
    bool workspace = options.backend == Backend::Workspace;
    bool companionMetaNotReady = false;
    if(options.backend == Backend::Auto && !paged) {
        std::error_code ec;
        workspace = path.extension() == ".meta" ||
                    std::filesystem::exists(seismic::engine::WorkspaceMetaPath(path), ec);
        if(workspace && path.extension() != ".meta") {
            // P8 (paleo): an interrupted-but-resumable workspace must never be
            // mistaken for a ready one. A cheap header probe decides; incomplete
            // or unreadable metas keep the direct backend until the transcode
            // finishes, flagged as a fallback so consumers see the truth.
            seismic::engine::WorkspaceMetaSummary summary;
            std::string probeError;
            if(!seismic::engine::ProbeWorkspaceMeta(path, summary, probeError) ||
               !summary.exists || !summary.readable || !summary.complete) {
                workspace = false;
                companionMetaNotReady = true;
            }
        }
    }

    const auto stripMetaSuffix = [](const std::filesystem::path& metaPath) -> std::filesystem::path {
        const std::string suffix = ".sf3c.meta";
        std::string name = metaPath.filename().u8string();
        if(name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.resize(name.size() - suffix.size());
        } else {
            name = metaPath.stem().u8string();
        }
        return metaPath.parent_path() / name;
    };
    std::filesystem::path base = path;
    if(base.extension() == ".meta") {
        base = stripMetaSuffix(base);
    }
    dataset->workspaceBase_ = base;
    dataset->directPath_ = path;

    if(paged) {
        dataset->workspaceBase_ = path;
        if(options.progressiveLod) {
            const engine::Status discoverStatus = dataset->DiscoverPagedLods(path);
            if(!discoverStatus.ok()) {
                status = discoverStatus;
                return nullptr;
            }
            if(!dataset->lodLevels_.empty()) {
                const int coarsest = dataset->lodLevels_.back().level;
                const engine::Status lodStatus =
                    dataset->OpenPagedAt(dataset->lodLevels_.back().base, options);
                if(lodStatus.ok()) {
                    dataset->activeLodLevel_ = coarsest;
                } else {
                    const engine::Status baseStatus = dataset->OpenPagedAt(path, options);
                    if(!baseStatus.ok()) {
                        status = baseStatus;
                        return nullptr;
                    }
                    dataset->activeLodLevel_ = 0;
                }
            } else {
                const engine::Status baseStatus = dataset->OpenPagedAt(path, options);
                if(!baseStatus.ok()) {
                    status = baseStatus;
                    return nullptr;
                }
            }
        } else {
            const engine::Status openStatus = dataset->OpenPagedAt(path, options);
            if(!openStatus.ok()) {
                status = openStatus;
                return nullptr;
            }
            dataset->baseMetadata_ = dataset->source_->Metadata();
        }
    } else if(workspace) {
        engine::Status openStatus = dataset->OpenWorkspaceAt(base, options);
        if(!openStatus.ok()) {
            if(options.backend == Backend::Auto) {
                // Safe fallback: a missing or unusable workspace must never stop
                // a SEG-Y from opening.
                engine::Status directStatus = dataset->OpenDirect(path);
                if(!directStatus.ok()) {
                    status = openStatus;
                    return nullptr;
                }
                dataset->fellBackToDirect_ = true;
                dataset->baseMetadata_ = dataset->source_->Metadata();
            } else {
                status = openStatus;
                return nullptr;
            }
        }
        if(dataset->workspaceBackend_) {
            dataset->baseMetadata_ = dataset->source_->Metadata();
        }
        if(dataset->workspaceBackend_ && options.progressiveLod) {
            // Discover sibling LOD workspaces with the same geometry and source
            // identity. The coarsest level is used first.
            std::error_code ec;
            const std::filesystem::path directory = base.parent_path();
            for(const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
                if(ec) {
                    break;
                }
                const std::filesystem::path candidate = entry.path();
                if(candidate.extension() != ".meta") {
                    continue;
                }
                std::filesystem::path candidateBase = stripMetaSuffix(candidate);
                if(candidateBase == base) {
                    continue;
                }
                seismic::engine::WorkspaceReader probe;
                std::string probeError;
                if(!probe.Open(candidateBase, probeError)) {

                    continue;
                }
                const seismic::engine::WorkspaceInfo& info = probe.Info();
                if(info.lodLevel == 0) {

                    continue;
                }
                // A level is compatible when its reduced geometry times its own
                // factors covers the base geometry (ceil division by the builder).
                const std::uint64_t baseSamples =
                    static_cast<std::uint64_t>(dataset->source_->Metadata().sampleCount);
                const std::uint64_t baseInlines = static_cast<std::uint64_t>(
                    dataset->source_->Metadata().inlineMax - dataset->source_->Metadata().inlineMin + 1);
                const std::uint64_t baseXlines = static_cast<std::uint64_t>(
                    dataset->source_->Metadata().xlineMax - dataset->source_->Metadata().xlineMin + 1);
                const std::uint64_t factorS = std::max<std::uint64_t>(1, info.lodFactorSamples);
                const std::uint64_t factorI = std::max<std::uint64_t>(1, info.lodFactorInlines);
                const std::uint64_t factorX = std::max<std::uint64_t>(1, info.lodFactorXlines);
                const auto covers = [](std::uint64_t reduced, std::uint64_t factor, std::uint64_t base) {
                    return reduced * factor >= base && reduced * factor < base + factor;
                };
                if(!covers(info.samples, factorS, baseSamples) ||
                   !covers(info.inlines, factorI, baseInlines) ||
                   !covers(info.xlines, factorX, baseXlines)) {

                    continue;
                }
                Dataset::LodEntry lod;
                lod.level = static_cast<int>(info.lodLevel);
                lod.base = candidateBase;
                lod.factorSamples = info.lodFactorSamples;
                lod.factorInlines = info.lodFactorInlines;
                lod.factorXlines = info.lodFactorXlines;
                dataset->lodLevels_.push_back(lod);
            }
            std::sort(dataset->lodLevels_.begin(), dataset->lodLevels_.end(),
                      [](const Dataset::LodEntry& a, const Dataset::LodEntry& b) { return a.level < b.level; });
            if(!dataset->lodLevels_.empty()) {
                const int coarsest = dataset->lodLevels_.back().level;
                engine::Status lodStatus = dataset->SetActiveLod(coarsest);
                if(!lodStatus.ok()) {
                    dataset->activeLodLevel_ = 0;
                }
            }
        }
    } else {
        engine::Status directStatus = dataset->OpenDirect(path);
        if(!directStatus.ok()) {
            status = directStatus;
            return nullptr;
        }
        dataset->baseMetadata_ = dataset->source_->Metadata();
        if(companionMetaNotReady) {
            dataset->fellBackToDirect_ = true; // P8: companion meta exists but is not ready
        }
    }
    dataset->open_ = true;
    status = engine::Status::Ok();
    return dataset;
}

engine::Status Dataset::DiscoverPagedLods(const std::filesystem::path& l0Path) {
    lodLevels_.clear();
    engine::PagedWorkspaceReader baseReader;
    std::string error;
    if(!baseReader.Open(l0Path, error, true)) {
        return engine::Status::Error(engine::StatusCode::IoError, error);
    }
    const engine::PagedWorkspaceInfo baseInfo = baseReader.Info();
    if(!baseInfo.complete || baseInfo.lodLevel != 0) {
        return engine::Status::Error(
            engine::StatusCode::InvalidArgument, "progressive paged open requires a complete L0 workspace");
    }
    baseMetadata_ = MetadataFromPagedInfo(baseInfo, l0Path);
    std::error_code ec;
    for(const auto& entry : std::filesystem::directory_iterator(l0Path.parent_path(), ec)) {
        if(ec || entry.path() == l0Path || entry.path().extension() != ".sf3p") {
            continue;
        }
        engine::PagedWorkspaceReader probe;
        std::string probeError;
        if(!probe.Open(entry.path(), probeError, true)) {
            continue;
        }
        const engine::PagedWorkspaceInfo& info = probe.Info();
        if(!info.complete || info.lodLevel == 0 || info.sourceSize != baseInfo.sourceSize ||
           info.sourceMtimeTicks != baseInfo.sourceMtimeTicks ||
           info.sourceFingerprint != baseInfo.sourceFingerprint ||
           info.lodSourceHash != baseInfo.sourceFingerprint) {
            continue;
        }
        const auto covers = [](std::uint64_t reduced, std::uint64_t factor, std::uint64_t full) {
            factor = std::max<std::uint64_t>(1, factor);
            return reduced == (full + factor - 1) / factor;
        };
        if(!covers(static_cast<std::uint64_t>(info.inlineAxis.count), info.lodFactorInlines,
                   static_cast<std::uint64_t>(baseInfo.inlineAxis.count)) ||
           !covers(static_cast<std::uint64_t>(info.xlineAxis.count), info.lodFactorXlines,
                   static_cast<std::uint64_t>(baseInfo.xlineAxis.count)) ||
           !covers(info.samples, info.lodFactorSamples, baseInfo.samples)) {
            continue;
        }
        LodEntry lod;
        lod.level = static_cast<int>(info.lodLevel);
        lod.base = entry.path();
        lod.factorSamples = info.lodFactorSamples;
        lod.factorInlines = info.lodFactorInlines;
        lod.factorXlines = info.lodFactorXlines;
        lodLevels_.push_back(std::move(lod));
    }
    std::sort(lodLevels_.begin(), lodLevels_.end(),
              [](const LodEntry& a, const LodEntry& b) { return a.level < b.level; });
    return engine::Status::Ok();
}

int Dataset::LodLevelAt(int index) const {
    if(index < 0 || index >= static_cast<int>(lodLevels_.size())) {
        return 0;
    }
    return lodLevels_[static_cast<std::size_t>(index)].level;
}

std::uint64_t Dataset::LodFactorSamplesAt(int index) const {
    if(index < 0 || index >= static_cast<int>(lodLevels_.size())) {
        return 1;
    }
    return lodLevels_[static_cast<std::size_t>(index)].factorSamples;
}

const Dataset::LodEntry* Dataset::ActiveLodEntry() const {
    if(activeLodLevel_ == 0) {
        return nullptr;
    }
    for(const LodEntry& entry : lodLevels_) {
        if(entry.level == activeLodLevel_) {
            return &entry;
        }
    }
    return nullptr;
}

bool Dataset::MapInline(int baseValue, int& activeValue) const {
    if(activeLodLevel_ == 0) {
        activeValue = baseValue;
        return true;
    }
    const LodEntry* lod = ActiveLodEntry();
    int baseIndex = 0;
    if(lod == nullptr || !baseMetadata_.inlineAxis.ExactIndexOf(baseValue, baseIndex) ||
       source_ == nullptr || source_->Metadata().inlineAxis.count <= 0) {
        return false;
    }
    const int activeIndex = std::min(source_->Metadata().inlineAxis.count - 1,
        baseIndex / static_cast<int>(std::max<std::uint64_t>(1, lod->factorInlines)));
    activeValue = source_->Metadata().inlineAxis.ValueAt(activeIndex);
    return true;
}

bool Dataset::MapXline(int baseValue, int& activeValue) const {
    if(activeLodLevel_ == 0) {
        activeValue = baseValue;
        return true;
    }
    const LodEntry* lod = ActiveLodEntry();
    int baseIndex = 0;
    if(lod == nullptr || !baseMetadata_.xlineAxis.ExactIndexOf(baseValue, baseIndex) ||
       source_ == nullptr || source_->Metadata().xlineAxis.count <= 0) {
        return false;
    }
    const int activeIndex = std::min(source_->Metadata().xlineAxis.count - 1,
        baseIndex / static_cast<int>(std::max<std::uint64_t>(1, lod->factorXlines)));
    activeValue = source_->Metadata().xlineAxis.ValueAt(activeIndex);
    return true;
}

int Dataset::MapSample(int baseSample) const {
    const LodEntry* lod = ActiveLodEntry();
    const std::uint64_t factor = lod == nullptr ? 1 : std::max<std::uint64_t>(1, lod->factorSamples);
    return baseSample < 0 ? baseSample : static_cast<int>(static_cast<std::uint64_t>(baseSample) / factor);
}

engine::Status Dataset::SetActiveLod(int lodLevel) {
    if(!workspaceBackend_) {
        return engine::Status::Error(engine::StatusCode::Unsupported,
                                     "LOD levels exist only for workspace backends");
    }
    std::filesystem::path base = workspaceBase_;
    if(lodLevel != 0) {
        bool found = false;
        for(const LodEntry& entry : lodLevels_) {
            if(entry.level == lodLevel) {
                base = entry.base;
                found = true;
                break;
            }
        }
        if(!found) {
            return engine::Status::Error(engine::StatusCode::InvalidArgument,
                                         "unknown LOD level " + std::to_string(lodLevel));
        }
    }
    engine::Status status = pagedBackend_ ? OpenPagedAt(base, options_) : OpenWorkspaceAt(base, options_);
    if(!status.ok()) {
        return status;
    }
    activeLodLevel_ = lodLevel;
    return engine::Status::Ok();
}

std::string Dataset::QualityName() const {
    if(!workspaceBackend_) {
        return "direct (SEG-Y)";
    }
    if(activeLodLevel_ == 0) {
        return "L0 full";
    }
    for(const LodEntry& entry : lodLevels_) {
        if(entry.level == activeLodLevel_) {
            return "L" + std::to_string(entry.level) + " " +
                   std::to_string(entry.factorInlines) + "x" + std::to_string(entry.factorXlines) +
                   "x" + std::to_string(entry.factorSamples);
        }
    }
    return "L" + std::to_string(activeLodLevel_);
}

Dataset::~Dataset() {
    Close();
}

void Dataset::Close() {
    if(!open_) {
        return;
    }
    source_.reset();
    chunkCache_.reset();
    sliceCache_.reset();
    open_ = false;
}

const engine::DatasetMetadata& Dataset::Metadata() const {
    static const engine::DatasetMetadata empty;
    return source_ ? baseMetadata_ : empty;
}

std::string Dataset::GeometryDescription() const {
    const engine::DatasetMetadata& meta = Metadata();
    std::string text = "traces=" + std::to_string(meta.traceCount) +
                       " samples=" + std::to_string(meta.sampleCount) +
                       " inline=" + std::to_string(meta.inlineMin) + ".." + std::to_string(meta.inlineMax) +
                       " xline=" + std::to_string(meta.xlineMin) + ".." + std::to_string(meta.xlineMax) +
                       " sampleIntervalUs=" + std::to_string(meta.sampleIntervalUs);
    if(meta.ruleBased) {
        text += " rule-based";
    }
    if(workspaceBackend_) {
        text += " workspace";
    }
    return text;
}

engine::Status Dataset::ReadInline(int inlineNo, engine::Slice2D& out, engine::CancelToken* cancel,
                                   int maxColumns) {
    if(!open_ || !source_) {
        return engine::Status::Error(engine::StatusCode::Internal, "dataset is closed");
    }
    int mapped = 0;
    if(!MapInline(inlineNo, mapped)) {
        return engine::Status::Error(engine::StatusCode::NotFound,
                                     "inline is not present on the base axis");
    }
    return source_->ReadInline(mapped, out, cancel, maxColumns);
}

engine::Status Dataset::ReadCrossline(int xlineNo, engine::Slice2D& out, engine::CancelToken* cancel,
                                      int maxColumns) {
    if(!open_ || !source_) {
        return engine::Status::Error(engine::StatusCode::Internal, "dataset is closed");
    }
    int mapped = 0;
    if(!MapXline(xlineNo, mapped)) {
        return engine::Status::Error(engine::StatusCode::NotFound,
                                     "xline is not present on the base axis");
    }
    return source_->ReadCrossline(mapped, out, cancel, maxColumns);
}

engine::Status Dataset::ReadTimeSlice(int sampleIndex, engine::Slice2D& out, engine::CancelToken* cancel) {
    if(!open_ || !source_) {
        return engine::Status::Error(engine::StatusCode::Internal, "dataset is closed");
    }
    if(sampleIndex < 0 || sampleIndex >= baseMetadata_.sampleCount) {
        return engine::Status::Error(engine::StatusCode::InvalidArgument,
                                     "sample index is outside the base volume");
    }
    return source_->ReadTimeSlice(MapSample(sampleIndex), out, cancel);
}

engine::Status Dataset::ReadTrace(int inlineNo, int xlineNo, engine::TraceData& out, engine::CancelToken* cancel) {
    if(!open_ || !source_) {
        return engine::Status::Error(engine::StatusCode::Internal, "dataset is closed");
    }
    int mappedInline = 0;
    int mappedXline = 0;
    if(!MapInline(inlineNo, mappedInline) || !MapXline(xlineNo, mappedXline)) {
        return engine::Status::Error(engine::StatusCode::NotFound,
                                     "trace is not present on the base axes");
    }
    return source_->ReadTrace(mappedInline, mappedXline, out, cancel);
}

engine::Status Dataset::ReadCachedTimeSlice(int sampleIndex,engine::Slice2D& out,engine::CancelToken* cancel) {
    out={};
    if(!open_ || !pagedBackend_ || options_.timeCachePath.empty() ||
       sampleIndex<0 || sampleIndex>=baseMetadata_.sampleCount)
        return engine::Status::Error(engine::StatusCode::NotFound,"no exact time cache");
    engine::PagedWorkspaceReader header;
    std::string error;
    if(!header.Open(workspaceBase_,error,true))
        return engine::Status::Error(engine::StatusCode::NotFound,"time cache identity unavailable");
    if(engine::TimePlaneCache::Read(options_.timeCachePath,header.CacheSourceKey(),
        baseMetadata_.xlineAxis.count,baseMetadata_.inlineAxis.count,sampleIndex,out,cancel))
        return engine::Status::Ok();
    return engine::Status::Error(cancel && cancel->IsCancelled() ? engine::StatusCode::Cancelled :
                                engine::StatusCode::NotFound,"exact time cache miss");
}

engine::Status Dataset::ReadTimeSliceTiled(int sampleIndex, int tileSize, int focusInline, int focusXline,
    const engine::TimeTileCallback& publish, engine::Slice2D& out, engine::CancelToken* cancel) {
    if(!open_ || !source_ || activeLodLevel_ != 0)
        return engine::Status::Error(engine::StatusCode::Unsupported, "tiled refinement requires L0");
    if(auto* paged = dynamic_cast<engine::PagedWorkspaceVolumeSource*>(source_.get()))
        return paged->ReadTimeSliceTiled(sampleIndex, tileSize, focusInline, focusXline, publish, out, cancel);
    return engine::Status::Error(engine::StatusCode::Unsupported, "tiled refinement requires paged workspace");
}

engine::Status Dataset::ReadTimeSliceWindowed(int sampleIndex, engine::Slice2D& out, engine::CancelToken* cancel) {
    if(!open_ || !source_ || sampleIndex < 0 || sampleIndex >= baseMetadata_.sampleCount)
        return engine::Status::Error(engine::StatusCode::NotFound, "invalid time preview request");
    if(auto* paged = dynamic_cast<engine::PagedWorkspaceVolumeSource*>(source_.get()))
        return paged->ReadTimeSliceWindowed(MapSample(sampleIndex), out, cancel);
    return ReadTimeSlice(sampleIndex, out, cancel);
}

engine::Status Dataset::ReadVoxelWindow(const engine::VoxelWindowRequest& request, engine::VoxelWindow& out,
                                        engine::CancelToken* cancel) {
    if(!open_ || !source_) {
        return engine::Status::Error(engine::StatusCode::Internal, "dataset is closed");
    }
    engine::VoxelWindowRequest mapped = request;
    if(!MapInline(request.inlineBegin, mapped.inlineBegin) ||
       !MapXline(request.xlineBegin, mapped.xlineBegin)) {
        return engine::Status::Error(engine::StatusCode::NotFound,
                                     "voxel window origin is not present on the base axes");
    }
    const LodEntry* lod = ActiveLodEntry();
    const int fi = lod == nullptr ? 1 : static_cast<int>(std::max<std::uint64_t>(1, lod->factorInlines));
    const int fx = lod == nullptr ? 1 : static_cast<int>(std::max<std::uint64_t>(1, lod->factorXlines));
    const int fs = lod == nullptr ? 1 : static_cast<int>(std::max<std::uint64_t>(1, lod->factorSamples));
    mapped.sampleBegin = MapSample(request.sampleBegin);
    mapped.inlineCount = (request.inlineCount + fi - 1) / fi;
    mapped.xlineCount = (request.xlineCount + fx - 1) / fx;
    mapped.sampleCount = (request.sampleCount + fs - 1) / fs;
    return source_->ReadVoxelWindow(mapped, out, cancel);
}

engine::Status Dataset::ReadSection(const engine::SectionRequest& request, engine::Slice2D& out,
                                    engine::CancelToken* cancel) {
    if(!open_ || !source_) {
        return engine::Status::Error(engine::StatusCode::Internal, "dataset is closed");
    }
    engine::SectionRequest mapped = request;
    if(activeLodLevel_ != 0) {
        for(engine::PathPoint& point : mapped.pathPoints) {
            const int inlineFirst = baseMetadata_.inlineAxis.ValueAt(0);
            const int inlineLast = baseMetadata_.inlineAxis.ValueAt(baseMetadata_.inlineAxis.count - 1);
            const int xlineFirst = baseMetadata_.xlineAxis.ValueAt(0);
            const int xlineLast = baseMetadata_.xlineAxis.ValueAt(baseMetadata_.xlineAxis.count - 1);
            if(point.inlineNo < std::min(inlineFirst, inlineLast) ||
               point.inlineNo > std::max(inlineFirst, inlineLast) ||
               point.xlineNo < std::min(xlineFirst, xlineLast) ||
               point.xlineNo > std::max(xlineFirst, xlineLast)) {
                return engine::Status::Error(engine::StatusCode::NotFound,
                                             "section point is outside the base axes");
            }
            int baseI = 0;
            int baseX = 0;
            if(!baseMetadata_.inlineAxis.NearestIndexOf(point.inlineNo, baseI) ||
               !baseMetadata_.xlineAxis.NearestIndexOf(point.xlineNo, baseX)) {
                return engine::Status::Error(engine::StatusCode::NotFound,
                                             "section point is outside the base axes");
            }
            const LodEntry* lod = ActiveLodEntry();
            const int fi = static_cast<int>(std::max<std::uint64_t>(1, lod->factorInlines));
            const int fx = static_cast<int>(std::max<std::uint64_t>(1, lod->factorXlines));
            point.inlineNo = source_->Metadata().inlineAxis.ValueAt(std::min(
                source_->Metadata().inlineAxis.count - 1, baseI / fi));
            point.xlineNo = source_->Metadata().xlineAxis.ValueAt(std::min(
                source_->Metadata().xlineAxis.count - 1, baseX / fx));
        }
    }
    return source_->ReadArbitrarySection(mapped, out, cancel);
}

engine::SourceStatistics Dataset::Statistics() const {
    return source_ ? source_->Statistics() : engine::SourceStatistics{};
}

engine::CacheStats Dataset::ChunkCacheStats() const {
    return chunkCache_ ? chunkCache_->Stats() : engine::CacheStats{};
}

engine::CacheStats Dataset::SliceCacheStats() const {
    return sliceCache_ ? sliceCache_->Stats() : engine::CacheStats{};
}

void Dataset::ClearCaches() {
    if(auto* paged = dynamic_cast<engine::PagedWorkspaceVolumeSource*>(source_.get())) paged->ClearTimeWindow();
    if(chunkCache_) {
        chunkCache_->Clear();
    }
    if(sliceCache_) {
        sliceCache_->Clear();
    }
}

engine::Status Dataset::SetCacheBudget(std::size_t chunkBytes, std::size_t sliceBytes) {
    if(chunkCache_) {
        chunkCache_->SetBudget(chunkBytes);
    }
    if(sliceCache_) {
        sliceCache_->SetBudget(sliceBytes);
    }
    return engine::Status::Ok();
}

} // namespace sdk
} // namespace seismic
