#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "Engine/Types.h"
#include "Engine/WorkspaceFormat.h"

namespace seismic {
namespace engine {

enum class LodMethod : std::uint32_t {
    None = 0,
    Average = 1,
    Decimate = 2,
    Rms = 3,
    Envelope = 4,
};

constexpr std::uint32_t kLodAlgorithmVersion = 1;

struct LodBuildOptions {
    std::uint32_t level = 1;
    LodMethod method = LodMethod::Average;
    std::uint32_t factorSamples = 2;
    std::uint32_t factorInlines = 2;
    std::uint32_t factorXlines = 2;
    std::uint32_t chunkSamples = 64;
    std::uint32_t chunkInlines = 64;
    std::uint32_t chunkXlines = 64;
    std::uint32_t chunksPerShard = 64;
};

struct LodBuildResult {
    Status status;
    WorkspaceInfo info;
    std::uint64_t outputVoxels = 0;
    std::uint64_t sourceChunksRead = 0;
    std::size_t maxQueueDepth = 0; // bounded pipeline back-pressure evidence
    double elapsedSeconds = 0.0;
};

// Builds one LOD level from a workspace at the level below it. The source is
// never modified; the target is a new workspace with the LOD metadata.
LodBuildResult BuildLodLevel(
    const std::filesystem::path& sourceBase,
    const std::filesystem::path& targetBase,
    const LodBuildOptions& options,
    CancelToken* cancel,
    const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress = {});

enum class LodSelectPolicy {
    Fastest = 0,       // coarsest available level (interactive drag)
    BestAvailable = 1, // finest level not finer than the request (settled view)
};

// Returns the chosen level or nullptr when the list is empty.
const WorkspaceInfo* ChooseLodLevel(
    const std::vector<WorkspaceInfo>& levels,
    LodSelectPolicy policy,
    int requestedSamples = 0);

} // namespace engine
} // namespace seismic