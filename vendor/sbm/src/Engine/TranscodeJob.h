#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

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