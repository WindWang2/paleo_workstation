#include "Engine/LodBuilder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace seismic {
namespace engine {
namespace {

double SecondsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1e6;
}

bool Cancelled(const CancelToken* cancel) {
    return cancel != nullptr && cancel->IsCancelled();
}

std::uint32_t DivUp(std::uint32_t value, std::uint32_t divisor) {
    return divisor == 0 ? 0 : (value + divisor - 1) / divisor;
}

// Bounded single-producer/single-consumer queue for the LOD writer pipeline.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool Push(T&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
        if(closed_) {
            return false;
        }
        items_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    bool Pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if(items_.empty()) {
            return false;
        }
        item = std::move(items_.front());
        items_.pop_front();
        if(items_.size() + 1 >= capacity_) {
            notFull_.notify_one();
        }
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    std::size_t Depth() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<T> items_;
    std::size_t capacity_ = 1;
    bool closed_ = false;
};

struct ReadyLodChunk {
    std::uint32_t cs = 0;
    std::uint32_t ci = 0;
    std::uint32_t cx = 0;
    std::vector<float> values;
};

struct ChunkKey {
    std::uint32_t cs = 0;
    std::uint32_t ci = 0;
    std::uint32_t cx = 0;
    bool operator==(const ChunkKey& other) const {
        return cs == other.cs && ci == other.ci && cx == other.cx;
    }
};

struct ChunkKeyHash {
    std::size_t operator()(const ChunkKey& key) const {
        return (static_cast<std::size_t>(key.cs) * 73856093u) ^
               (static_cast<std::size_t>(key.ci) * 19349663u) ^
               (static_cast<std::size_t>(key.cx) * 83492791u);
    }
};

} // namespace

LodBuildResult BuildLodLevel(
    const std::filesystem::path& sourceBase,
    const std::filesystem::path& targetBase,
    const LodBuildOptions& options,
    CancelToken* cancel,
    const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress) {
    LodBuildResult result;
    const auto start = std::chrono::steady_clock::now();

    WorkspaceReader source;
    std::string error;
    if(!source.Open(sourceBase, error)) {
        result.status = Status::Error(StatusCode::IoError, error);
        return result;
    }
    const WorkspaceInfo& src = source.Info();
    const std::uint32_t factorS = std::max(1u, options.factorSamples);
    const std::uint32_t factorI = std::max(1u, options.factorInlines);
    const std::uint32_t factorX = std::max(1u, options.factorXlines);

    WorkspaceInfo info;
    info.samples = DivUp(src.samples, factorS);
    info.inlines = DivUp(src.inlines, factorI);
    info.xlines = DivUp(src.xlines, factorX);
    info.chunkSamples = options.chunkSamples;
    info.chunkInlines = options.chunkInlines;
    info.chunkXlines = options.chunkXlines;
    info.chunksPerShard = options.chunksPerShard;
    info.codec = 0;
    info.sampleIntervalUs = src.sampleIntervalUs * factorS;
    info.inlineMin = src.inlineMin;
    info.xlineMin = src.xlineMin;
    info.sourceIdentityHash = src.sourceIdentityHash;
    info.algorithmVersion = src.algorithmVersion;
    info.lodLevel = options.level;
    info.lodMethod = static_cast<std::uint32_t>(options.method);
    info.lodFactorSamples = factorS;
    info.lodFactorInlines = factorI;
    info.lodFactorXlines = factorX;
    info.lodAlgorithmVersion = kLodAlgorithmVersion;
    info.lodSourceHash = src.sourceIdentityHash ^
                         (static_cast<std::uint64_t>(src.samples) << 32) ^
                         (static_cast<std::uint64_t>(src.inlines) << 16) ^
                         static_cast<std::uint64_t>(src.xlines) ^
                         (static_cast<std::uint64_t>(src.lodLevel) << 24) ^
                         static_cast<std::uint64_t>(src.chunkSamples);
    result.info = info;

    // Resume only when the existing target was built from the same source with
    // the same LOD algorithm; otherwise invalidate it and rebuild (algorithm
    // changes must never be mixed into an old pyramid).
    bool resumeTarget = false;
    if(std::filesystem::exists(WorkspaceMetaPath(targetBase))) {
        WorkspaceReader existing;
        std::string existingError;
        if(existing.Open(targetBase, existingError)) {
            const WorkspaceInfo& old = existing.Info();
            resumeTarget = old.lodSourceHash == info.lodSourceHash &&
                           old.lodAlgorithmVersion == kLodAlgorithmVersion &&
                           old.lodLevel == info.lodLevel && old.lodMethod == info.lodMethod &&
                           old.samples == info.samples && old.inlines == info.inlines &&
                           old.xlines == info.xlines && old.chunkSamples == info.chunkSamples &&
                           old.chunkInlines == info.chunkInlines && old.chunkXlines == info.chunkXlines;
        }
        if(!resumeTarget) {
            std::error_code ec;
            std::filesystem::remove(WorkspaceMetaPath(targetBase), ec);
            for(std::uint32_t shard = 0; shard < 256; ++shard) {
                std::filesystem::remove(WorkspaceShardPath(targetBase, shard), ec);
            }
        }
    }
    WorkspaceWriter writer;
    if(!writer.Open(targetBase, info, resumeTarget, error)) {
        result.status = Status::Error(StatusCode::IoError, error);
        return result;
    }

    constexpr std::size_t kQueueCapacity = 4;
    BoundedQueue<ReadyLodChunk> queue(kQueueCapacity);
    std::atomic<std::uint64_t> chunksWrittenAtomic{0};
    std::atomic<bool> writerFailed{false};
    std::string writerError;
    std::mutex writerErrorMutex;
    std::size_t maxQueueDepth = 0;
    std::thread writerThread([&]() {
        ReadyLodChunk ready;
        while(queue.Pop(ready)) {
            if(!writer.WriteChunk(ready.cs, ready.ci, ready.cx, ready.values.data(),
                                  ready.values.size(), writerError)) {
                std::lock_guard<std::mutex> lock(writerErrorMutex);
                writerFailed.store(true);
                queue.Close();
                return;
            }
            chunksWrittenAtomic.fetch_add(1);
        }
    });

    std::vector<float> outChunk(static_cast<std::size_t>(info.ChunkBytes() / sizeof(float)),
                                std::numeric_limits<float>::quiet_NaN());
    // Bounded source chunk cache: the LOD build touches the source in a
    // different order than it was written, so a single-entry cache would thrash
    // and re-open the shard for almost every voxel.
    const std::size_t maxCachedChunks = 64;
    const std::size_t maxCachedBytes = 64ull * 1024ull * 1024ull;
    std::unordered_map<ChunkKey, std::vector<float>, ChunkKeyHash> chunkCache;
    std::vector<ChunkKey> cacheOrder;
    std::size_t cachedBytes = 0;
    const std::size_t sourceChunkBytes = static_cast<std::size_t>(source.Info().ChunkBytes());
    const auto fetchChunk = [&](const ChunkKey& key) -> const std::vector<float>* {
        const auto it = chunkCache.find(key);
        if(it != chunkCache.end()) {
            return &it->second;
        }
        while(!chunkCache.empty() &&
              (chunkCache.size() >= maxCachedChunks || cachedBytes + sourceChunkBytes > maxCachedBytes)) {
            const ChunkKey evict = cacheOrder.front();
            cacheOrder.erase(cacheOrder.begin());
            const auto evictIt = chunkCache.find(evict);
            if(evictIt != chunkCache.end()) {
                cachedBytes -= evictIt->second.size() * sizeof(float);
                chunkCache.erase(evictIt);
            }
        }
        std::vector<float> loaded;
        if(!source.ReadChunk(key.cs, key.ci, key.cx, loaded, error)) {
            return nullptr;
        }
        ++result.sourceChunksRead;
        cachedBytes += loaded.size() * sizeof(float);
        cacheOrder.push_back(key);
        const auto inserted = chunkCache.emplace(key, std::move(loaded));
        return &inserted.first->second;
    };
    const std::uint64_t totalChunks = info.ChunkCount();
    std::uint64_t doneChunks = 0;

    for(std::uint32_t cs = 0; cs < info.ChunksS(); ++cs) {
        for(std::uint32_t ci = 0; ci < info.ChunksI(); ++ci) {
            for(std::uint32_t cx = 0; cx < info.ChunksX(); ++cx) {
                if(writer.HasChunk(cs, ci, cx)) {
                    ++doneChunks;
                    continue;
                }
                if(Cancelled(cancel) || writerFailed.load()) {
                    queue.Close();
                    if(writerThread.joinable()) {
                        writerThread.join();
                    }
                    writer.Finalize(error);
                    result.status = Status::Error(StatusCode::Cancelled, "LOD build cancelled");
                    return result;
                }
                std::fill(outChunk.begin(), outChunk.end(), std::numeric_limits<float>::quiet_NaN());
                for(std::uint32_t ls = 0; ls < info.chunkSamples; ++ls) {
                    const std::uint32_t outSample = cs * info.chunkSamples + ls;
                    if(outSample >= info.samples) {
                        break;
                    }
                    for(std::uint32_t li = 0; li < info.chunkInlines; ++li) {
                        const std::uint32_t outInline = ci * info.chunkInlines + li;
                        if(outInline >= info.inlines) {
                            break;
                        }
                        for(std::uint32_t lx = 0; lx < info.chunkXlines; ++lx) {
                            const std::uint32_t outXline = cx * info.chunkXlines + lx;
                            if(outXline >= info.xlines) {
                                break;
                            }
                            double value = 0.0;
                            double sum = 0.0;
                            double sumSq = 0.0;
                            double maxAbs = 0.0;
                            int count = 0;
                            for(std::uint32_t ds = 0; ds < factorS; ++ds) {
                                const std::uint32_t srcSample = outSample * factorS + ds;
                                if(srcSample >= src.samples) {
                                    break;
                                }
                                for(std::uint32_t di = 0; di < factorI; ++di) {
                                    const std::uint32_t srcInline = outInline * factorI + di;
                                    if(srcInline >= src.inlines) {
                                        break;
                                    }
                                    for(std::uint32_t dx = 0; dx < factorX; ++dx) {
                                        const std::uint32_t srcXline = outXline * factorX + dx;
                                        if(srcXline >= src.xlines) {
                                            break;
                                        }
                                        const ChunkKey key{
                                            srcSample / src.chunkSamples,
                                            srcInline / src.chunkInlines,
                                            srcXline / src.chunkXlines};
                                        const std::vector<float>* cached = fetchChunk(key);
                                        if(cached == nullptr) {
                                            continue;
                                        }
                                        const std::size_t offset =
                                            (static_cast<std::size_t>(srcSample % src.chunkSamples) * src.chunkInlines +
                                             (srcInline % src.chunkInlines)) * src.chunkXlines +
                                            (srcXline % src.chunkXlines);
                                        const float sampleValue = (*cached)[offset];
                                        if(std::isnan(sampleValue)) {
                                            continue;
                                        }
                                        const double v = static_cast<double>(sampleValue);
                                        switch(options.method) {
                                            case LodMethod::Decimate:
                                                if(count == 0) {
                                                    value = v;
                                                }
                                                break;
                                            case LodMethod::Rms:
                                            case LodMethod::Envelope:
                                                break;
                                            default:
                                                sum += v;
                                                break;
                                        }
                                        sumSq += v * v;
                                        maxAbs = std::max(maxAbs, std::fabs(v));
                                        ++count;
                                    }
                                }
                            }
                            float out = std::numeric_limits<float>::quiet_NaN();
                            if(count > 0) {
                                switch(options.method) {
                                    case LodMethod::Decimate:
                                        out = static_cast<float>(value);
                                        break;
                                    case LodMethod::Rms:
                                        out = static_cast<float>(std::sqrt(sumSq / count));
                                        break;
                                    case LodMethod::Envelope:
                                        out = static_cast<float>(maxAbs);
                                        break;
                                    default:
                                        out = static_cast<float>(sum / count);
                                        break;
                                }
                            }
                            outChunk[(static_cast<std::size_t>(ls) * info.chunkInlines + li) *
                                        info.chunkXlines + lx] = out;
                        }
                    }
                }
                if(writerFailed.load()) {
                    queue.Close();
                    if(writerThread.joinable()) {
                        writerThread.join();
                    }
                    std::string message;
                    {
                        std::lock_guard<std::mutex> lock(writerErrorMutex);
                        message = writerError;
                    }
                    writer.Finalize(error);
                    result.status = Status::Error(StatusCode::IoError,
                                                  message.empty() ? "workspace write failed" : message);
                    return result;
                }
                ReadyLodChunk ready;
                ready.cs = cs;
                ready.ci = ci;
                ready.cx = cx;
                ready.values = std::move(outChunk);
                outChunk.assign(static_cast<std::size_t>(info.ChunkBytes() / sizeof(float)),
                                std::numeric_limits<float>::quiet_NaN());
                if(!queue.Push(std::move(ready))) {
                    queue.Close();
                    if(writerThread.joinable()) {
                        writerThread.join();
                    }
                    writer.Finalize(error);
                    result.status = Status::Error(StatusCode::Cancelled, "LOD build cancelled by caller");
                    return result;
                }
                maxQueueDepth = std::max(maxQueueDepth, queue.Depth());
                ++doneChunks;
                ++result.outputVoxels;
                if(progress && !progress(chunksWrittenAtomic.load(), totalChunks)) {
                    queue.Close();
                    if(writerThread.joinable()) {
                        writerThread.join();
                    }
                    writer.Finalize(error);
                    result.status = Status::Error(StatusCode::Cancelled, "LOD build cancelled by caller");
                    return result;
                }
            }
        }
    }
    queue.Close();
    if(writerThread.joinable()) {
        writerThread.join();
    }
    if(writerFailed.load()) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(writerErrorMutex);
            message = writerError;
        }
        writer.Finalize(error);
        result.status = Status::Error(StatusCode::IoError,
                                      message.empty() ? "workspace write failed" : message);
        return result;
    }
    if(!writer.Finalize(error)) {
        result.status = Status::Error(StatusCode::IoError, error);
        return result;
    }
    result.maxQueueDepth = maxQueueDepth;
    result.elapsedSeconds = SecondsSince(start);
    result.status = Status::Ok();
    return result;
}

const WorkspaceInfo* ChooseLodLevel(
    const std::vector<WorkspaceInfo>& levels,
    LodSelectPolicy policy,
    int requestedSamples) {
    if(levels.empty()) {
        return nullptr;
    }
    if(policy == LodSelectPolicy::Fastest) {
        const WorkspaceInfo* best = &levels.front();
        for(const WorkspaceInfo& level : levels) {
            if(level.samples < best->samples) {
                best = &level;
            }
        }
        return best;
    }
    // BestAvailable: the finest level that is not finer than the request; when
    // every level is finer than the request, the finest available level.
    const WorkspaceInfo* pick = nullptr;
    if(requestedSamples > 0) {
        for(const WorkspaceInfo& level : levels) {
            if(level.samples <= static_cast<std::uint32_t>(requestedSamples) &&
               (pick == nullptr || level.samples > pick->samples)) {
                pick = &level;
            }
        }
    }
    if(pick == nullptr) {
        for(const WorkspaceInfo& level : levels) {
            if(pick == nullptr || level.samples > pick->samples) {
                pick = &level;
            }
        }
    }
    return pick;
}

} // namespace engine
} // namespace seismic