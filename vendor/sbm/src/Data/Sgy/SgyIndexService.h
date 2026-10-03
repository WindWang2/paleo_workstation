#pragma once

// Production index service: the single entry point that decides how a SEG-Y is
// indexed for the first time.
//
// Order of decisions:
//   1. a valid persistent index in the cache (only FullyScanned indexes are
//      ever published there);
//   2. a bounded-window sequential scan (ScanSegySequentially) for fixed-length
//      trace SEG-Y files, with device-adaptive window/queue parameters and a
//      checkpoint in an isolated cache directory;
//   3. the legacy strided builder ONLY when the sequential layout is
//      unsupported (never after a user cancellation).
//
// Both SgyIndexJob (Viewer background import) and SgyVolumeSource::Open use this
// service, so there is exactly one indexing strategy in production.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "Data/Sgy/SgyIndex.h"
#include "Data/Sgy/SgySequentialScan.h"

namespace seismic {

struct SgyIndexBuildRequest {
    std::filesystem::path path;
    bool useCache = true;
    bool saveCache = true;
    // Device-adaptive scan parameters (filled from the StorageProfile by the
    // caller; the service itself has no device dependency).
    std::size_t windowBytes = 8u << 20;
    int readQueueDepth = 1;
    std::size_t checkpointIntervalBytes = 256u << 20;
    // Checkpoint files live here (an isolated index cache directory), never next
    // to the source SEG-Y. Empty disables checkpointing.
    std::filesystem::path checkpointPath;
    // The strided fallback is only for explicitly unsupported layouts.
    bool allowStridedFallback = true;
    bool verbose = true;
};

struct SgyIndexBuildOutcome {
    SgyIndexPtr index;
    bool fromCache = false;
    bool cancelled = false;
    // "cache" | "sequential" | "strided-fallback"
    std::string strategy;
    std::string message; // error text when index is null
    std::string cacheNote;
    // Non-empty when the sequential scan had to abandon checkpoint persistence.
    // Surfaced instead of swallowed: the index itself is still valid, only the
    // ability to resume a future interrupted scan was lost.
    std::string checkpointNote;
    SgyScanStats scanStats;
    double wallMs = 0.0;
    std::uint64_t resumedFromRecord = 0;
};

// progress(processed, total) returns false to cancel; shouldCancel is polled by
// the scan. A cancellation is reported as cancelled=true and never triggers a
// fallback rescan.
SgyIndexBuildOutcome BuildSgyIndexAuto(const SgyIndexBuildRequest& request,
                                       const std::function<bool(int, int)>& progress = {},
                                       const std::function<bool()>& shouldCancel = {});

} // namespace seismic
