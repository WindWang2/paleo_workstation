#include "Data/Sgy/SgyIndexJob.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

#include "Data/Sgy/SgyIndexBuilder.h"
#include "Data/Sgy/SgyIndexCache.h"
#include "Data/Sgy/SgyIndexService.h"

namespace seismic {

namespace {
std::atomic<std::uint64_t> g_nextJobId{1};
}

struct SgyIndexJob::Impl {
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> running{false};
    std::atomic<bool> resultReady{false};
    std::atomic<int> processed{0};
    std::atomic<int> total{0};
    std::uint64_t id = 0;

    std::mutex resultMutex;
    SgyIndexPtr result;
    std::string error;
    std::string cacheNote;
    std::string strategy;
    bool fromCache = false;
    std::uint64_t readCalls = 0;
    std::uint64_t readBytes = 0;
};

SgyIndexJob::SgyIndexJob() : impl_(std::make_unique<Impl>()) {
    impl_->id = g_nextJobId.fetch_add(1);
}

SgyIndexJob::~SgyIndexJob() {
    Cancel();
    if(impl_ && impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void SgyIndexJob::Start(const std::filesystem::path& path, bool saveCache) {
    SgyIndexBuildRequest request;
    request.path = path;
    request.saveCache = saveCache;
    request.checkpointPath = SgyIndexCache::CacheDirectory() / "scan-checkpoints" /
                             (path.filename().u8string() + ".ckpt");
    Start(request);
}

void SgyIndexJob::Start(const SgyIndexBuildRequest& request) {
    if(!impl_ || impl_->running.load()) {
        return;
    }

    impl_->cancel.store(false);
    impl_->resultReady.store(false);
    impl_->processed.store(0);
    impl_->total.store(0);
    {
        std::lock_guard<std::mutex> lock(impl_->resultMutex);
        impl_->result.reset();
        impl_->error.clear();
        impl_->cacheNote.clear();
    }

    impl_->running.store(true);
    SgyIndexJob::Impl* impl = impl_.get();
    const SgyIndexBuildRequest jobRequest = request;
    impl->worker = std::thread([impl, jobRequest]() {
        // The shared production index service owns the strategy decision
        // (cache -> sequential scan -> unsupported-only strided fallback).
        const SgyIndexBuildOutcome outcome = BuildSgyIndexAuto(
            jobRequest,
            [impl](int processed, int total) {
                impl->processed.store(processed);
                impl->total.store(total);
                return !impl->cancel.load();
            },
            [impl]() { return impl->cancel.load(); });

        {
            std::lock_guard<std::mutex> lock(impl->resultMutex);
            if(outcome.index) {
                impl->result = outcome.index;
            } else {
                impl->error = outcome.message.empty() ? "SGY indexing failed." : outcome.message;
            }
            impl->cacheNote = outcome.cacheNote;
            impl->strategy = outcome.strategy;
            impl->fromCache = outcome.fromCache;
            impl->readCalls = outcome.scanStats.readCalls;
            impl->readBytes = outcome.scanStats.readBytes;
        }
        impl->running.store(false);
        impl->resultReady.store(true);
    });
}

void SgyIndexJob::Cancel() {
    if(impl_) {
        impl_->cancel.store(true);
    }
}

bool SgyIndexJob::Running() const {
    return impl_ && impl_->running.load();
}

int SgyIndexJob::Processed() const {
    return impl_ ? impl_->processed.load() : 0;
}

int SgyIndexJob::Total() const {
    return impl_ ? impl_->total.load() : 0;
}

std::uint64_t SgyIndexJob::Id() const {
    return impl_ ? impl_->id : 0;
}

const std::string& SgyIndexJob::Strategy() const {
    static const std::string empty;
    return impl_ ? impl_->strategy : empty;
}

bool SgyIndexJob::FromCache() const {
    return impl_ && impl_->fromCache;
}

std::uint64_t SgyIndexJob::ReadCalls() const {
    return impl_ ? impl_->readCalls : 0;
}

std::uint64_t SgyIndexJob::ReadBytes() const {
    return impl_ ? impl_->readBytes : 0;
}

bool SgyIndexJob::TakeResult(SgyIndexPtr& index, std::string& errorMessage) {
    if(!impl_ || !impl_->resultReady.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->resultMutex);
        index = std::move(impl_->result);
        errorMessage = impl_->error;
        impl_->result.reset();
        impl_->error.clear();
    }
    impl_->resultReady.store(false);
    return true;
}

std::string SgyIndexJob::CacheNote() const {
    if(!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    return impl_->cacheNote;
}

} // namespace seismic
