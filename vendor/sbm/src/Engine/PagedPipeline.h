#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Engine/LodBuilder.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/Types.h"

namespace seismic {
namespace engine {

// Production SEG-Y -> paged workspace pipeline.  The writer uses the measured
// 8x8x256 / 8x8x16 geometry, reads every source trace at most once for L0 and
// leaves a resumable .partial file after cancellation or failure.
struct PagedPipelineOptions {
    int inlineBegin = 0;
    int inlineCount = 0;
    int xlineBegin = 0;
    int xlineCount = 0;
    std::uint32_t codec = kPagedCodecRaw;
    std::uint32_t codecLevel = 3;
    bool resume = true;
    bool buildLod1 = true;
    bool buildLod2 = true;
    std::uint64_t buildGeneration = 1;
};

struct PagedPipelineProgress {
    std::string phase; // l0-transcode | l1-build | l2-build | finalizing
    std::uint64_t chunksDone = 0;
    std::uint64_t chunksTotal = 0;
    std::uint64_t chunksSkipped = 0;
    std::uint64_t tracesRead = 0;
    std::uint64_t bytesWritten = 0;
    double elapsedSeconds = 0.0;
};

struct PagedBuildResult {
    Status status;
    std::filesystem::path path;
    PagedWorkspaceInfo info;
    std::uint64_t chunksWritten = 0;
    std::uint64_t chunksSkipped = 0;
    std::uint64_t tracesRead = 0;
    std::uint64_t bytesWritten = 0;
    bool reused = false;
    double elapsedSeconds = 0.0;

    // P9 (paleo): L0 quality evidence (LOD levels aggregate the L0 numbers).
    std::uint64_t missingTraceCount = 0;   // (inline, xline) absent in source
    std::uint64_t damagedTraceCount = 0;   // source read failed -> NaN-filled
    std::uint64_t sanitizedSampleCount = 0;
    std::uint64_t sanitizedTraceCount = 0;
    std::vector<std::pair<int, int>> damagedTraceSample; // first 32
    float valueMin = std::numeric_limits<float>::quiet_NaN();
    float valueMax = std::numeric_limits<float>::quiet_NaN();
};

struct PagedPyramidResult {
    Status status;
    PagedBuildResult l0;
    PagedBuildResult l1;
    PagedBuildResult l2;
};

using PagedProgressFn = std::function<bool(const PagedPipelineProgress&)>;

std::filesystem::path PagedLodPath(const std::filesystem::path& l0Path, int level);

PagedBuildResult TranscodeSegyToPagedWorkspace(
    const std::filesystem::path& segyPath,
    const std::filesystem::path& targetPath,
    const PagedPipelineOptions& options,
    CancelToken* cancel,
    const PagedProgressFn& progress = {});

// Builds a cumulative LOD directly from L0.  factor* values are relative to L0
// (L1=4x4x1, L2=8x8x1 in the production pyramid), which makes coordinate
// mapping deterministic and prevents repeated averaging drift.
PagedBuildResult BuildPagedLodFromL0(
    const std::filesystem::path& l0Path,
    const std::filesystem::path& targetPath,
    std::uint32_t level,
    std::uint32_t factorInlines,
    std::uint32_t factorXlines,
    std::uint32_t factorSamples,
    LodMethod method,
    const PagedPipelineOptions& options,
    CancelToken* cancel,
    const PagedProgressFn& progress = {});

PagedPyramidResult BuildPagedPyramid(
    const std::filesystem::path& segyPath,
    const std::filesystem::path& l0Path,
    const PagedPipelineOptions& options,
    CancelToken* cancel,
    const PagedProgressFn& progress = {});

} // namespace engine
} // namespace seismic
