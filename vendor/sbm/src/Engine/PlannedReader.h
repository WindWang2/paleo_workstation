#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/ReadPlan.h"
#include "Engine/Types.h"

namespace seismic {
namespace engine {

// Bounded pool of reusable per-trace sample buffers.
class TraceBufferPool {
public:
    explicit TraceBufferPool(std::size_t budgetBytes = 8ull * 1024 * 1024);

    std::vector<float> Acquire(int sampleCount);
    void Release(std::vector<float>&& buffer);

    std::size_t BuffersInUse() const;
    std::size_t BudgetBytes() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::vector<float>> free_;
    std::size_t budgetBytes_ = 0;
    std::size_t inUse_ = 0;
};

struct PlannedTraceData {
    int traceIndex = -1;
    bool valid = false;
    std::vector<float> samples;
};

struct PlannedReadStats {
    std::uint64_t tracesRead = 0;
    std::uint64_t gapTracesRead = 0;
    std::uint64_t readRanges = 0;
    std::uint64_t bytesRead = 0;
    double ioMicros = 0.0;
    double decodeMicros = 0.0;
};

// Executes a read plan with ONE read session, in ascending trace order.
// out is indexed by unique-trace slot (see ReadPlan::uniqueTraces).
class PlannedReader {
public:
    PlannedReader();
    ~PlannedReader();

    PlannedReader(const PlannedReader&) = delete;
    PlannedReader& operator=(const PlannedReader&) = delete;

    bool Open(const SgyIndexPtr& index, std::string& errorMessage);

    bool Execute(
        const ReadPlan& plan,
        std::vector<PlannedTraceData>& out,
        CancelToken* cancel,
        std::string& errorMessage,
        const std::function<bool(int processed, int total)>& progress = {});

    const PlannedReadStats& Stats() const { return stats_; }
    void SetBufferPool(std::shared_ptr<TraceBufferPool> pool) { pool_ = std::move(pool); }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<TraceBufferPool> pool_;
    PlannedReadStats stats_;
};

} // namespace engine
} // namespace seismic