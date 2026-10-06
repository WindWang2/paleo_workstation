#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Types.h"
#include "Engine/WorkspaceFormat.h"

namespace seismic {
namespace engine {

// Resumable SEG-Y -> workspace transcode (Stage E).
//
// The job is synchronous; the caller runs it on a worker thread. Cancellation
// keeps the workspace resumable: the completion map is finalized with whatever
// was written, and a later call with the same layout continues from there.
struct TranscodeOptions {
    // ROI in SEG-Y geometry (inclusive start, 0 = full extent).
    int inlineBegin = 0;
    int inlineCount = 0;
    int xlineBegin = 0;
    int xlineCount = 0;
    int sampleBegin = 0;
    int sampleCount = 0;

    // Workspace layout (defaults = the Stage D accepted layout).
    std::uint32_t chunkSamples = 64;
    std::uint32_t chunkInlines = 64;
    std::uint32_t chunkXlines = 64;
    std::uint32_t chunksPerShard = 64;
    // Workspace codec: kCodecRaw (default) or kCodecZstd when the build enables it.
    std::uint32_t codec = 0;
    std::uint32_t codecLevel = 3;

    // P6 (paleo): parallel chunk-encoding pool, clamped to 1..4. 1 keeps the
    // upstream single-thread pipeline (producer -> writer). N > 1 inserts N-1
    // encoder threads in front of the single writer thread so the codec CPU
    // cost (zstd dominates the sf3c wall time) is spread; write order and the
    // resumable layout are unchanged. Raw codec ignores the pool (nothing to
    // encode off-thread).
    std::uint32_t writerThreads = 1;
};

struct TranscodeProgress {
    std::string phase; // "scanning" | "transcoding" | "finalizing"
    std::uint64_t chunksDone = 0;
    std::uint64_t chunksTotal = 0;
    std::uint64_t chunksSkipped = 0;
    std::uint64_t tracesRead = 0;
    std::uint64_t bytesWritten = 0;
    std::size_t maxQueueDepth = 0; // bounded pipeline back-pressure evidence
    std::uint64_t writeCalls = 0;  // physical shard write calls (profile-adaptive)
    double elapsedSeconds = 0.0;
};

struct TranscodeResult {
    Status status;
    WorkspaceInfo info;
    std::uint64_t chunksWritten = 0;
    std::uint64_t chunksSkipped = 0;
    std::uint64_t tracesRead = 0;
    std::uint64_t bytesWritten = 0;
    std::size_t maxQueueDepth = 0; // bounded pipeline back-pressure evidence
    std::uint64_t writeCalls = 0;  // physical shard write calls (profile-adaptive)
    double elapsedSeconds = 0.0;

    // P6 (paleo): quality evidence for the transcode report.
    // missingTraceCount: (inline, xline) absent in the source (NaN in output).
    // damagedTraceCount: source read failed; the trace is skipped and NaN-filled
    // instead of failing the whole job. damagedTraceSample keeps the first 32.
    // valueMin/valueMax: NaN-aware amplitude range of what was read (NaN when
    // the source produced no data at all).
    std::uint64_t missingTraceCount = 0;
    std::uint64_t damagedTraceCount = 0;
    std::uint64_t sanitizedSampleCount = 0;
    std::uint64_t sanitizedTraceCount = 0;
    std::vector<std::pair<int, int>> damagedTraceSample;
    float valueMin = std::numeric_limits<float>::quiet_NaN();
    float valueMax = std::numeric_limits<float>::quiet_NaN();
};

// progress returns false to cancel (same effect as cancelling the token).
TranscodeResult TranscodeSegyToWorkspace(
    const std::filesystem::path& segyPath,
    const std::filesystem::path& workspaceBase,
    const TranscodeOptions& options,
    CancelToken* cancel,
    const std::function<bool(const TranscodeProgress&)>& progress = {});

} // namespace engine
} // namespace seismic