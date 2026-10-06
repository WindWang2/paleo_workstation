#include "Engine/PlannedReader.h"

#include <algorithm>
#include <chrono>

#include "Data/Sgy/SgyReadSession.h"

namespace seismic {
namespace engine {

TraceBufferPool::TraceBufferPool(std::size_t budgetBytes) : budgetBytes_(budgetBytes) {}

std::vector<float> TraceBufferPool::Acquire(int sampleCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t wanted = static_cast<std::size_t>(std::max(0, sampleCount));
    for(auto it = free_.begin(); it != free_.end(); ++it) {
        if(it->capacity() >= wanted) {
            std::vector<float> buffer = std::move(*it);
            free_.erase(it);
            ++inUse_;
            buffer.resize(wanted);
            return buffer;
        }
    }
    ++inUse_;
    return std::vector<float>(wanted);
}

void TraceBufferPool::Release(std::vector<float>&& buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(inUse_ > 0) {
        --inUse_;
    }
    const std::size_t bytes = buffer.capacity() * sizeof(float);
    std::size_t held = 0;
    for(const std::vector<float>& pooled : free_) {
        held += pooled.capacity() * sizeof(float);
    }
    if(held + bytes <= budgetBytes_) {
        buffer.clear();
        free_.push_back(std::move(buffer));
    }
}

std::size_t TraceBufferPool::BuffersInUse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inUse_;
}

std::size_t TraceBufferPool::BudgetBytes() const {
    return budgetBytes_;
}

class PlannedReader::Impl {
public:
    SgyReadSession session;
};

PlannedReader::PlannedReader() : impl_(std::make_unique<Impl>()) {}
PlannedReader::~PlannedReader() = default;

bool PlannedReader::Open(const SgyIndexPtr& index, std::string& errorMessage) {
    return impl_->session.Open(index, errorMessage);
}

bool PlannedReader::Execute(
    const ReadPlan& plan,
    std::vector<PlannedTraceData>& out,
    CancelToken* cancel,
    std::string& errorMessage,
    const std::function<bool(int processed, int total)>& progress) {
    stats_ = PlannedReadStats{};
    out.assign(plan.uniqueTraces.size(), PlannedTraceData{});
    for(std::size_t i = 0; i < plan.uniqueTraces.size(); ++i) {
        out[i].traceIndex = plan.uniqueTraces[i];
    }
    if(!plan.planned) {
        return true;
    }
    if(!impl_->session.IsOpen()) {
        errorMessage = "the planned reader has no open read session";
        return false;
    }

    std::vector<int> slotOfTrace;
    slotOfTrace.reserve(plan.traces.size());
    {
        std::size_t slot = 0;
        for(std::size_t i = 0; i < plan.traces.size(); ++i) {
            if(slot < plan.uniqueTraces.size() && plan.traces[i] == plan.uniqueTraces[slot]) {
                slotOfTrace.push_back(static_cast<int>(slot));
                ++slot;
            } else {
                slotOfTrace.push_back(-1); // gap fill
            }
        }
    }

    const int totalTraces = static_cast<int>(plan.traces.size());
    int processed = 0;
    for(const PlannedRange& range : plan.ranges) {
        ++stats_.readRanges;
        for(int i = 0; i < range.traceCount; ++i) {
            const int plannedIndex = range.firstSlot + i;
            const int traceIndex = plan.traces[static_cast<std::size_t>(plannedIndex)];
            if(cancel != nullptr && cancel->IsCancelled()) {
                errorMessage = "read plan cancelled";
                return false;
            }
            if(progress && !progress(processed, totalTraces)) {
                errorMessage = "read plan cancelled by caller";
                return false;
            }
            const auto ioStart = std::chrono::steady_clock::now();
            std::vector<float> samples = pool_ ? pool_->Acquire(plan.stats.samplesRead)
                                               : std::vector<float>();
            std::string readError;
            const bool ok = impl_->session.ReadTrace(traceIndex, samples, readError);
            const double ioMicros = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - ioStart).count());
            ++processed;
            if(!ok) {
                if(pool_) {
                    pool_->Release(std::move(samples));
                }
                continue; // unreadable trace: its columns stay NaN
            }
            stats_.sanitizedSampleReads += impl_->session.LastSanitizedSampleCount();
            ++stats_.tracesRead;
            stats_.ioMicros += ioMicros;
            stats_.bytesRead += static_cast<std::uint64_t>(plan.stats.bytesPerTrace);
            const int slot = slotOfTrace[static_cast<std::size_t>(plannedIndex)];
            if(slot >= 0) {
                out[static_cast<std::size_t>(slot)].valid = true;
                out[static_cast<std::size_t>(slot)].samples = std::move(samples);
            } else {
                ++stats_.gapTracesRead;
                if(pool_) {
                    pool_->Release(std::move(samples));
                }
            }
        }
    }
    if(progress && !progress(totalTraces, totalTraces)) {
        errorMessage = "read plan cancelled by caller";
        return false;
    }
    return true;
}

} // namespace engine
} // namespace seismic