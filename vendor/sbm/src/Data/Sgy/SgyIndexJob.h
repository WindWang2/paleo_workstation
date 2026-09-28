#pragma once

#include <cstdint>
#include <filesystem>

#include "Data/Sgy/SgyIndexService.h"
#include <memory>
#include <string>

#include "Data/Sgy/SgyIndex.h"

namespace seismic {

// Background index builder. One job scans one SEG-Y file on a worker thread and
// publishes the immutable index back to the UI thread through TakeResult().
//
// Safety rules implemented here:
//   * the worker only touches its own read-only handle and its own index,
//   * progress is exposed through atomics (no locks around disk I/O),
//   * Cancel() is checked inside the scan and returns quickly,
//   * the destructor cancels and joins so shutdown cannot leak a thread,
//   * results are only handed out once, so a stale job cannot overwrite a
//     newer one if the caller tracks the job id.
class SgyIndexJob {
public:
    SgyIndexJob();
    ~SgyIndexJob();

    SgyIndexJob(const SgyIndexJob&) = delete;
    SgyIndexJob& operator=(const SgyIndexJob&) = delete;

    // saveCache publishes the finished index to the on-disk cache (atomic).
    void Start(const std::filesystem::path& path, bool saveCache = true);
    // Production entry point: the caller supplies the device-adaptive request
    // (window/queue/checkpoint) and the job runs the shared index service.
    void Start(const SgyIndexBuildRequest& request);
    void Cancel();

    bool Running() const;
    int Processed() const;
    int Total() const;
    std::uint64_t Id() const;
    // Production index-service outcome, for the first-open diagnostics.
    const std::string& Strategy() const;
    bool FromCache() const;
    std::uint64_t ReadCalls() const;
    std::uint64_t ReadBytes() const;

    // Returns true once, when the scan finished or failed. On success `index`
    // is set; otherwise `errorMessage` describes the failure or cancellation.
    bool TakeResult(SgyIndexPtr& index, std::string& errorMessage);

    // Human-readable note about the cache publish step (empty when not saved).
    std::string CacheNote() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace seismic
