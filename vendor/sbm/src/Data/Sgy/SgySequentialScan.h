#pragma once

// Stage K: bounded-window sequential SEG-Y scanning.
//
// Strategy B/C: instead of one small header read per trace (strided seek per
// trace), read large aligned windows of whole trace records, extract every
// trace header from the window buffer, and optionally hand the full records to
// a visitor so index + samples can be consumed in a single physical pass
// (strategy D / fused import).
//
// The scan is checkpointed: a compact progress marker plus an append-only
// partial array file allow a cancelled or crashed scan to resume at the
// checkpointed byte offset without re-reading the processed part. A partial
// scan never produces a published index (see SgyIndexCache::Save, which
// refuses incomplete indexes as well).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "Data/Sgy/SgyIndex.h"

namespace seismic {

struct SgyScanOptions {
    // Window size in bytes; a window is rounded down to whole trace records.
    std::size_t windowBytes = 8u << 20;
    // Outstanding read requests: 1 for a single sequential stream (HDD), up to
    // a bounded number for NVMe-style parallel reads.
    int readQueueDepth = 1;
    // Checkpoint cadence in source bytes; 0 disables checkpointing.
    std::size_t checkpointIntervalBytes = 0;
    std::filesystem::path checkpointPath;
    // Diagnostic bound: stop after covering this many source bytes (0 = whole
    // file). A bounded scan is never reported as complete.
    std::uint64_t maxSourceBytes = 0;
    // Cooperative cancellation, checked once per window.
    std::function<bool()> shouldCancel;
};

struct SgyScanStats {
    std::uint64_t readCalls = 0;       // file read operations issued
    std::uint64_t readBytes = 0;       // bytes requested from the file
    std::uint64_t sequentialWindows = 0;
    std::uint64_t stridedRequests = 0; // header-only seek+read requests (strategy A)
    std::uint64_t readRanges = 0;      // merged physical ranges (windows)
    std::uint64_t recordsParsed = 0;
    std::uint64_t recordsSkipped = 0;  // 0/0 header traces
    std::uint64_t checkpointWrites = 0;
    std::uint64_t checkpointBytes = 0;
    std::uint64_t resumedFromRecord = 0;
    std::uint64_t resumedFromByte = 0;
    std::size_t peakBufferBytes = 0;
    double wallMs = 0.0;
    double parseMs = 0.0; // wall time inside the parse loops (single threaded)
};

// Visitor over full trace records in physical order. Returning false cancels.
using SgyRecordVisitor = std::function<bool(int traceIndex, const char* record, int recordBytes)>;

struct SgyScanResult {
    SgyIndexPtr index; // set only when the whole file was scanned
    bool complete = false;
    bool cancelled = false;
    // True when the scan stopped early because maxSourceBytes was set: the
    // result is resumable progress, not an error and not a complete index.
    bool bounded = false;
    std::string message;
    SgyScanStats stats;
};

// Scans the SEG-Y sequentially in bounded windows. When the scan completes the
// result carries a fully assembled index whose semantics match
// SgyIndexBuilder::Build; when it is cancelled or bounded, the checkpoint (if
// enabled) keeps the progress for a later resume.
bool ScanSegySequentially(const std::filesystem::path& path,
                          const SgyScanOptions& options,
                          const SgyRecordVisitor& visitor,
                          SgyScanResult& outResult);

// Removes the checkpoint + partial files created by a scan (safe no-op when
// they do not exist). Only used for this round's own diagnostic artifacts.
void RemoveSegyScanCheckpoint(const std::filesystem::path& checkpointPath);

} // namespace seismic