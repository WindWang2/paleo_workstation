#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace seismic {
namespace engine {

// Storage classes used to tune I/O without changing data formats or results.
enum class StorageClass {
    Unknown = 0,
    Rotational, // HDD: seek-avoiding, deep sequential reads
    SataSsd,
    Nvme,
};

const char* StorageClassName(StorageClass storageClass);

// I/O scheduling parameters for one device. Every field is a policy knob; none
// of them may change numeric results.
struct IoProfile {
    StorageClass storageClass = StorageClass::Unknown;
    // Size of one sequential read window (0 = per-trace strided reads).
    std::size_t sequentialBlockBytes = 4ull * 1024ull * 1024ull;
    // In-flight reads: 1 for rotational, bounded but >1 for SSDs.
    std::uint32_t readQueueDepth = 2;
    // Merge read ranges separated by at most this many bytes.
    std::size_t mergeGapBytes = 64ull * 1024ull;
    // Directional prefetch distance in requests.
    std::uint32_t prefetchDistance = 2;
    // Checkpoint interval while building derived data.
    std::size_t checkpointIntervalBytes = 256ull * 1024ull * 1024ull;
    // Background I/O share (percent) while a foreground request is active.
    std::uint32_t backgroundIoPercent = 50;
    // Write batching for workspace shards (larger = fewer, more sequential writes).
    std::size_t writeBatchBytes = 4ull * 1024ull * 1024ull;
    // Detection evidence (diagnostics only).
    bool detected = false;
    std::string deviceName;
    std::string busType;
    std::string note; // "override:hdd", "fallback", "detected", ...
};

// where each part of the pipeline lives; each can be on a different device
struct StorageProfileSet {
    IoProfile source;     // the SEG-Y file
    IoProfile workspace;  // workspace shards
    IoProfile indexCache; // persistent index cache
    std::string sourcePath;
    std::string workspacePath;
    std::string indexCachePath;
    std::string describe() const;
};

// "auto", "hdd", "sata", "nvme", "lowmem" (case-insensitive); invalid -> auto.
IoProfile ProfileFor(StorageClass storageClass, const std::string& overrideName);

// Physical device classification for the volume that contains `path`.
// Never throws; returns Unknown with a reason on any failure.
StorageClass ClassifyPath(const std::filesystem::path& path,
                          std::string* deviceName = nullptr,
                          std::string* busType = nullptr,
                          std::string* note = nullptr);

// Resolves the three device profiles. `overrideName` comes from the caller or
// the SEISMIC_STORAGE_PROFILE environment variable; empty = auto detection.
StorageProfileSet ResolveStorageProfiles(const std::filesystem::path& sourcePath,
                                         const std::filesystem::path& workspacePath,
                                         const std::filesystem::path& indexCachePath,
                                         const std::string& overrideName = std::string());

// Current override from the environment (empty when unset).
std::string StorageProfileOverride();

} // namespace engine
} // namespace seismic