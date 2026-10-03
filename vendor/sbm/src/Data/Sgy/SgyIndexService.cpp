#include "Data/Sgy/SgyIndexService.h"

#include <chrono>
#include <iostream>

#include "Data/Sgy/SgyFileReader.h"
#include "Data/Sgy/SgyIndexBuilder.h"
#include "Data/Sgy/SgyIndexCache.h"

namespace seismic {
namespace {

double MsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1000.0;
}

std::string StorageNote(const SgyIndexBuildRequest& request) {
    return "window=" + std::to_string(request.windowBytes) +
           " queue=" + std::to_string(request.readQueueDepth) +
           " checkpoint=" + (request.checkpointPath.empty() ? std::string("off")
                                                            : request.checkpointPath.u8string());
}

} // namespace

SgyIndexBuildOutcome BuildSgyIndexAuto(const SgyIndexBuildRequest& request,
                                       const std::function<bool(int, int)>& progress,
                                       const std::function<bool()>& shouldCancel) {
    SgyIndexBuildOutcome outcome;
    const auto start = std::chrono::steady_clock::now();

    // 1) Persistent index (only FullyScanned indexes are published there).
    if(request.useCache) {
        std::string cacheReason;
        SgyIndexPtr cached = SgyIndexCache::Load(request.path, cacheReason);
        if(cached && cached->FullyScanned()) {
            outcome.index = std::move(cached);
            outcome.fromCache = true;
            outcome.strategy = "cache";
            outcome.wallMs = MsSince(start);
            if(request.verbose) {
                std::cout << "[index] strategy=cache traces=" << outcome.index->traces.size()
                          << " wall=" << outcome.wallMs << " ms | " << StorageNote(request) << std::endl;
            }
            return outcome;
        }
        if(request.verbose && !cacheReason.empty()) {
            std::cout << "[index] cache miss: " << cacheReason << std::endl;
        }
    }

    // 2) Sequential scan when the file has a fixed-length trace layout.
    SgyFileSummary summary;
    std::string summaryError;
    bool sequentialSupported = SgyFileReader::ReadSummary(request.path, summary, summaryError);
    std::string unsupportedReason;
    if(!sequentialSupported) {
        unsupportedReason = summaryError.empty() ? "cannot read the SEG-Y metadata" : summaryError;
    } else {
        const std::uint64_t dataStart = 3600;
        const std::uint64_t bytesPerTrace = 240 +
            static_cast<std::uint64_t>(summary.sampleCount) * static_cast<std::uint64_t>(summary.formatSizeBytes);
        if(summary.traceCount <= 0 || summary.sampleCount <= 0 || summary.formatSizeBytes <= 0) {
            sequentialSupported = false;
            unsupportedReason = "invalid SEG-Y metadata";
        } else if(dataStart + static_cast<std::uint64_t>(summary.traceCount) * bytesPerTrace >
                  static_cast<std::uint64_t>(summary.fileSize)) {
            sequentialSupported = false;
            unsupportedReason = "the file is smaller than its declared fixed trace layout";
        }
    }

    auto runStridedFallback = [&]() -> SgyIndexBuildOutcome {
        SgyIndexBuildOutcome fallback;
        fallback.strategy = "strided-fallback";
        SgyIndexPtr index;
        std::string error;
        const bool ok = SgyIndexBuilder::Build(
            request.path, index, error,
            [&progress](int processed, int total) {
                return !progress || progress(processed, total);
            });
        if(!ok) {
            fallback.message = error.empty() ? "the strided indexer failed" : error;
            fallback.wallMs = MsSince(start);
            if(request.verbose) {
                std::cout << "[index] strategy=strided-fallback failed: " << fallback.message << std::endl;
            }
            return fallback;
        }
        if(request.saveCache) {
            std::string saveError;
            fallback.cacheNote = SgyIndexCache::Save(index, saveError)
                ? "index cache published"
                : ("index cache not published: " + saveError);
        }
        fallback.index = std::move(index);
        fallback.wallMs = MsSince(start);
        if(request.verbose) {
            std::cout << "[index] strategy=strided-fallback traces=" << fallback.index->traces.size()
                      << " wall=" << fallback.wallMs << " ms | " << StorageNote(request) << std::endl;
        }
        return fallback;
    };

    if(!sequentialSupported) {
        if(request.allowStridedFallback) {
            if(request.verbose) {
                std::cout << "[index] sequential layout unsupported (" << unsupportedReason
                          << "); using the strided indexer" << std::endl;
            }
            return runStridedFallback();
        }
        outcome.message = "sequential layout unsupported: " + unsupportedReason;
        outcome.strategy = "sequential";
        outcome.wallMs = MsSince(start);
        return outcome;
    }

    SgyScanOptions options;
    options.windowBytes = request.windowBytes;
    options.readQueueDepth = request.readQueueDepth;
    options.checkpointPath = request.checkpointPath;
    options.checkpointIntervalBytes = request.checkpointPath.empty() ? 0 : request.checkpointIntervalBytes;
    if(shouldCancel) {
        options.shouldCancel = shouldCancel;
    }

    SgyScanResult scanResult;
    const bool scanned = ScanSegySequentially(
        request.path, options,
        [&progress, &summary](int, const char*, int) {
            if(!progress) {
                return true;
            }
            // Progress is reported in record units so byte/record progress maps
            // onto one unified (processed, total) contract.
            static thread_local int counter = 0;
            if((++counter % 4096) != 0) {
                return true;
            }
            return progress(counter, summary.traceCount);
        },
        scanResult);

    outcome.scanStats = scanResult.stats;
    outcome.resumedFromRecord = scanResult.stats.resumedFromRecord;
    outcome.checkpointNote = scanResult.checkpointNote;
    outcome.strategy = "sequential";

    if(scanResult.cancelled) {
        // A user cancellation must never trigger a fallback rescan.
        outcome.cancelled = true;
        outcome.message = "index scan cancelled by the caller";
        outcome.wallMs = MsSince(start);
        if(request.verbose) {
            std::cout << "[index] strategy=sequential cancelled after "
                      << scanResult.stats.recordsParsed << " records | " << StorageNote(request) << std::endl;
        }
        return outcome;
    }
    if(!scanned || !scanResult.complete || !scanResult.index) {
        // A bounded diagnostic limit is not an error path for production, but
        // any other failure is classified: layout/stride problems may fall back.
        const bool layoutProblem =
            scanResult.message.find("layout") != std::string::npos ||
            scanResult.message.find("stride") != std::string::npos ||
            scanResult.message.find("smaller than") != std::string::npos;
        if(layoutProblem && request.allowStridedFallback) {
            if(request.verbose) {
                std::cout << "[index] sequential scan reported '" << scanResult.message
                          << "'; using the strided indexer" << std::endl;
            }
            return runStridedFallback();
        }
        outcome.message = scanResult.message.empty() ? "the sequential scan failed" : scanResult.message;
        outcome.wallMs = MsSince(start);
        if(request.verbose) {
            std::cout << "[index] strategy=sequential failed: " << outcome.message << std::endl;
        }
        return outcome;
    }

    if(request.saveCache) {
        std::string saveError;
        outcome.cacheNote = SgyIndexCache::Save(scanResult.index, saveError)
            ? "index cache published"
            : ("index cache not published: " + saveError);
    }
    outcome.index = std::move(scanResult.index);
    outcome.wallMs = MsSince(start);
    if(request.verbose) {
        std::cout << "[index] strategy=sequential traces=" << outcome.index->traces.size()
                  << " readCalls=" << scanResult.stats.readCalls
                  << " readBytes=" << scanResult.stats.readBytes
                  << " windows=" << scanResult.stats.sequentialWindows
                  << " resumedFrom=" << scanResult.stats.resumedFromRecord
                  << " wall=" << outcome.wallMs << " ms"
                  << (outcome.cacheNote.empty() ? "" : (" | " + outcome.cacheNote))
                  << (outcome.checkpointNote.empty()
                          ? ""
                          : (" | checkpoint disabled: " + outcome.checkpointNote))
                  << " | " << StorageNote(request) << std::endl;
    }
    return outcome;
}

} // namespace seismic
