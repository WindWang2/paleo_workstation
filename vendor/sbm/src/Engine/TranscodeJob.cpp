#include "Engine/TranscodeJob.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

#include "Data/Sgy/SgyIndexCache.h"
#include "Data/Sgy/SgyReadSession.h"
#include "Engine/StorageProfile.h"
#include "Engine/SgyVolumeSource.h"

#ifdef SEISMIC_HAVE_ZSTD
#include <zstd.h>
#endif

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

// Bounded multi-producer/multi-consumer queue used by the transcode pipeline.
// Closing wakes every waiter; a closed-and-drained Pop returns false.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Returns false when the queue is closed (cancellation or downstream failure).
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

    // Returns false when the queue is closed and empty (consumer exit).
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

// A chunk travelling the pipeline: raw float values on the single-thread path,
// or an already-encoded payload when the parallel encode pool is active.
struct ReadyChunk {
    std::uint32_t cs = 0;
    std::uint32_t ci = 0;
    std::uint32_t cx = 0;
    std::vector<float> values;
    std::vector<unsigned char> payload; // populated when prepared == true
    bool prepared = false;
};

// P6 (paleo): per-trace quality accounting shared by both read passes. All
// mutations happen on the producer thread only.
struct TraceQuality {
    std::uint64_t missing = 0;   // (inline, xline) absent from the source
    std::uint64_t damaged = 0;   // source read failed -> NaN-filled
    std::vector<std::pair<int, int>> damagedSample; // first 32, (inline, xline)
    float valueMin = std::numeric_limits<float>::infinity();
    float valueMax = -std::numeric_limits<float>::infinity();

    void NoteMissing() { ++missing; }

    void NoteDamaged(int inlineNo, int xlineNo) {
        ++damaged;
        if(damagedSample.size() < 32) {
            damagedSample.emplace_back(inlineNo, xlineNo);
        }
    }

    void NoteTrace(const std::vector<float>& samples) {
        for(const float v : samples) {
            if(std::isnan(v)) {
                continue;
            }
            if(v < valueMin) {
                valueMin = v;
            }
            if(v > valueMax) {
                valueMax = v;
            }
        }
    }

    void Finish(TranscodeResult& result) const {
        result.missingTraceCount = missing;
        result.damagedTraceCount = damaged;
        result.damagedTraceSample = damagedSample;
        if(valueMin != std::numeric_limits<float>::infinity()) {
            result.valueMin = valueMin;
        }
        if(valueMax != -std::numeric_limits<float>::infinity()) {
            result.valueMax = valueMax;
        }
    }
};

} // namespace

TranscodeResult TranscodeSegyToWorkspace(
    const std::filesystem::path& segyPath,
    const std::filesystem::path& workspaceBase,
    const TranscodeOptions& options,
    CancelToken* cancel,
    const std::function<bool(const TranscodeProgress&)>& progress) {
    TranscodeResult result;
    const auto start = std::chrono::steady_clock::now();

    // P6: the (potentially long) index/open stage is a distinct phase for the
    // caller's aggregated progress; the header doc promised "scanning".
    if(progress) {
        TranscodeProgress scan;
        scan.phase = "scanning";
        scan.elapsedSeconds = 0.0;
        if(!progress(scan)) {
            result.status = Status::Error(StatusCode::Cancelled, "the transcode was cancelled");
            return result;
        }
    }

    Status openStatus;
    std::unique_ptr<SgyVolumeSource> source = SgyVolumeSource::Open(segyPath, openStatus);
    if(!source || !openStatus.ok()) {
        result.status = Status::Error(StatusCode::IoError,
                                      openStatus.message.empty() ? "cannot open the SEG-Y" : openStatus.message);
        return result;
    }
    const SgyIndexPtr index = source->Volume().Index();
    if(!index || !index->complete) {
        result.status = Status::Error(StatusCode::NotIndexed, "the SEG-Y index is not complete");
        return result;
    }

    const int volumeInlineMin = index->inlineMin;
    const int volumeInlineMax = index->inlineMax;
    const int volumeXlineMin = index->xlineMin;
    const int volumeXlineMax = index->xlineMax;
    const int volumeSamples = index->sampleCount;
    if(volumeSamples <= 0) {
        result.status = Status::Error(StatusCode::InvalidArgument, "the SEG-Y has no samples");
        return result;
    }

    const int inlineBegin = options.inlineBegin > 0 ? options.inlineBegin : volumeInlineMin;
    const int inlineCount = options.inlineCount > 0
        ? options.inlineCount
        : std::max(0, volumeInlineMax - inlineBegin + 1);
    const int xlineBegin = options.xlineBegin > 0 ? options.xlineBegin : volumeXlineMin;
    const int xlineCount = options.xlineCount > 0
        ? options.xlineCount
        : std::max(0, volumeXlineMax - xlineBegin + 1);
    const int sampleBegin = options.sampleBegin > 0 ? options.sampleBegin : 0;
    const int sampleCount = options.sampleCount > 0
        ? options.sampleCount
        : std::max(0, volumeSamples - sampleBegin);
    if(inlineCount <= 0 || xlineCount <= 0 || sampleCount <= 0 ||
       inlineBegin < volumeInlineMin || xlineBegin < volumeXlineMin || sampleBegin < 0 ||
       inlineBegin + inlineCount - 1 > volumeInlineMax ||
       xlineBegin + xlineCount - 1 > volumeXlineMax ||
       sampleBegin + sampleCount > volumeSamples) {
        result.status = Status::Error(StatusCode::InvalidArgument, "the ROI is outside the SEG-Y geometry");
        return result;
    }

    WorkspaceInfo info;
    info.samples = static_cast<std::uint32_t>(sampleCount);
    info.inlines = static_cast<std::uint32_t>(inlineCount);
    info.xlines = static_cast<std::uint32_t>(xlineCount);
    info.chunkSamples = options.chunkSamples;
    info.chunkInlines = options.chunkInlines;
    info.chunkXlines = options.chunkXlines;
    info.chunksPerShard = options.chunksPerShard;
    info.codec = options.codec;
    info.codecLevel = options.codecLevel;
    info.sampleIntervalUs = static_cast<std::uint32_t>(std::max(0, index->sampleIntervalUs));
    info.inlineMin = inlineBegin;
    info.xlineMin = xlineBegin;
    info.sourceIdentityHash =
        static_cast<std::uint64_t>(index->fileSize) ^ (static_cast<std::uint64_t>(index->modifiedTimeTicks) << 1);
    info.algorithmVersion = SgyIndexCache::kAlgorithmVersion;
    info.lodLevel = 0;
    info.lodMethod = 0;
    info.lodFactorSamples = 1;
    info.lodFactorInlines = 1;
    info.lodFactorXlines = 1;
    info.lodAlgorithmVersion = 0;
    info.lodSourceHash = info.sourceIdentityHash;
    result.info = info;

    // Device-adaptive write batching (HDD: large batches, NVMe: small).
    const engine::IoProfile workspaceProfile =
        ResolveStorageProfiles(segyPath, workspaceBase, SgyIndexCache::CacheDirectory()).workspace;
    WorkspaceWriter writer;
    writer.SetWriteBatchBytes(workspaceProfile.writeBatchBytes);
    std::string error;
    bool resumeExisting = std::filesystem::exists(WorkspaceMetaPath(workspaceBase));
    if(resumeExisting) {
        // A stale or incompatible workspace (old format version, different ROI)
        // must be rebuilt, not resumed.
        WorkspaceReader probe;
        std::string probeError;
        if(!probe.Open(workspaceBase, probeError)) {
            std::error_code ec;
            std::filesystem::remove(WorkspaceMetaPath(workspaceBase), ec);
            for(std::uint32_t shard = 0; shard < 1024; ++shard) {
                std::filesystem::remove(WorkspaceShardPath(workspaceBase, shard), ec);
            }
            resumeExisting = false;
        }
    }
    if(!writer.Open(workspaceBase, info, resumeExisting, error)) {
        result.status = Status::Error(StatusCode::IoError, error);
        return result;
    }

    SgyReadSession session;
    if(!session.Open(index, error)) {
        result.status = Status::Error(StatusCode::IoError, error);
        return result;
    }

    const std::uint64_t chunksTotal = info.ChunkCount();
    TranscodeProgress state;
    state.phase = "transcoding";
    state.chunksTotal = chunksTotal;

    // Pipeline topology (P6): with writerThreads > 1 and an encoding codec the
    // producer hands raw chunks to an encode pool; encoded chunks flow to the
    // single writer thread (shard order + resumable layout unchanged).
    // Otherwise the producer pushes directly and the writer encodes inline
    // (upstream single-thread behaviour).
    const bool parallelEncode =
        options.writerThreads > 1 && info.codec == kCodecZstd;
#ifdef SEISMIC_HAVE_ZSTD
    const std::uint32_t encoderCount = parallelEncode
        ? std::clamp(options.writerThreads, 1u, 4u)
        : 0;
#else
    const std::uint32_t encoderCount = 0;
#endif
    BoundedQueue<ReadyChunk> encodeQueue(encoderCount > 0 ? 8 : 4);
    BoundedQueue<ReadyChunk> writeQueue(4);
    std::atomic<std::uint64_t> chunksWrittenAtomic{0};
    std::atomic<std::uint64_t> tracesReadAtomic{0};
    std::atomic<std::uint64_t> bytesWrittenAtomic{0};
    std::atomic<bool> writerFailed{false};
    std::string writerError;
    std::mutex writerErrorMutex;
    std::size_t maxQueueDepth = 0;
    TraceQuality quality;
    std::atomic<int> encodersAlive{static_cast<int>(encoderCount)};

    auto failPipeline = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(writerErrorMutex);
            if(writerError.empty()) {
                writerError = message;
            }
        }
        writerFailed.store(true);
        encodeQueue.Close();
        writeQueue.Close();
    };

    std::vector<std::thread> encoderThreads;
    for(std::uint32_t e = 0; e < encoderCount; ++e) {
        encoderThreads.emplace_back([&]() {
            ReadyChunk raw;
            while(true) {
                if(writerFailed.load() || Cancelled(cancel)) {
                    // Unblock the producer (Push waits while the queue is full).
                    encodeQueue.Close();
                    break;
                }
                if(!encodeQueue.Pop(raw)) {
                    break; // closed and drained: normal shutdown
                }
                ReadyChunk encoded;
                encoded.cs = raw.cs;
                encoded.ci = raw.ci;
                encoded.cx = raw.cx;
                encoded.prepared = true;
#ifdef SEISMIC_HAVE_ZSTD
                encoded.payload.resize(ZSTD_compressBound(raw.values.size() * sizeof(float)));
                const std::size_t written = ZSTD_compress(
                    encoded.payload.data(), encoded.payload.size(),
                    raw.values.data(), raw.values.size() * sizeof(float),
                    static_cast<int>(std::max(1u, info.codecLevel)));
                if(ZSTD_isError(written)) {
                    failPipeline(std::string("zstd compress failed: ") + ZSTD_getErrorName(written));
                    break;
                }
                encoded.payload.resize(written);
#else
                (void)raw;
                failPipeline("this build has no zstd support");
                break;
#endif
                if(!writeQueue.Push(std::move(encoded))) {
                    encodeQueue.Close(); // downstream closed: release producer
                    break;
                }
            }
            if(encodersAlive.fetch_sub(1) == 1) {
                writeQueue.Close(); // last encoder out releases the writer
            }
        });
    }

    std::thread writerThread([&]() {
        ReadyChunk ready;
        while(writeQueue.Pop(ready)) {
            bool ok = false;
            if(ready.prepared) {
                ok = writer.WriteChunkPrepared(ready.cs, ready.ci, ready.cx,
                                               ready.payload.data(), ready.payload.size(),
                                               writerError);
                if(ok) {
                    bytesWrittenAtomic.fetch_add(ready.payload.size());
                }
            } else {
                ok = writer.WriteChunk(ready.cs, ready.ci, ready.cx, ready.values.data(),
                                       ready.values.size(), writerError);
                if(ok) {
                    bytesWrittenAtomic.fetch_add(info.ChunkBytes());
                }
            }
            if(!ok) {
                failPipeline(writerError.empty() ? "workspace write failed" : writerError);
                return;
            }
            chunksWrittenAtomic.fetch_add(1);
        }
    });

    // Queue the producer pushes into: the encode pool when active, otherwise
    // the writer's queue directly.
    auto pushChunk = [&](ReadyChunk&& chunk) -> bool {
        if(encoderCount > 0) {
            maxQueueDepth = std::max(maxQueueDepth, encodeQueue.Depth());
            return encodeQueue.Push(std::move(chunk));
        }
        maxQueueDepth = std::max(maxQueueDepth, writeQueue.Depth());
        return writeQueue.Push(std::move(chunk));
    };

    std::vector<float> chunk(static_cast<std::size_t>(info.ChunkBytes() / sizeof(float)),
                             std::numeric_limits<float>::quiet_NaN());
    std::vector<float> traceSamples;
    bool cancelled = false;

    // Trace-major pass: when the whole sample axis fits the chunk-buffer budget,
    // read every trace ONCE per (inline, xline) block and scatter its samples
    // into all sample-chunks. This removes the chunk-major redundancy (a trace
    // was read once per sample chunk).
    const std::uint64_t bufferedBytes =
        static_cast<std::uint64_t>(info.ChunksS()) * info.ChunkBytes();
    constexpr std::uint64_t kBufferBudgetBytes = 64ull * 1024ull * 1024ull;
    if(bufferedBytes > 0 && bufferedBytes <= kBufferBudgetBytes) {
        const std::size_t chunkFloats = static_cast<std::size_t>(info.ChunkBytes() / sizeof(float));
        std::vector<std::vector<float>> buffers(
            info.ChunksS(), std::vector<float>(chunkFloats, std::numeric_limits<float>::quiet_NaN()));
        for(std::uint32_t ci = 0; ci < info.ChunksI() && !cancelled; ++ci) {
            for(std::uint32_t cx = 0; cx < info.ChunksX() && !cancelled; ++cx) {
                // Resume fast path: skip the whole block when every sample chunk
                // of this (inline, xline) block is already present.
                bool blockComplete = true;
                for(std::uint32_t cs = 0; cs < info.ChunksS(); ++cs) {
                    if(!writer.HasChunk(cs, ci, cx)) {
                        blockComplete = false;
                        break;
                    }
                }
                if(blockComplete) {
                    result.chunksSkipped += info.ChunksS();
                    state.chunksSkipped += info.ChunksS();
                    state.chunksDone += info.ChunksS();
                    continue;
                }
                for(std::vector<float>& buffer : buffers) {
                    std::fill(buffer.begin(), buffer.end(), std::numeric_limits<float>::quiet_NaN());
                }
                for(std::uint32_t li = 0; li < info.chunkInlines && !cancelled; ++li) {
                    const std::uint32_t globalIl = ci * info.chunkInlines + li;
                    if(globalIl >= info.inlines) {
                        break;
                    }
                    for(std::uint32_t lx = 0; lx < info.chunkXlines; ++lx) {
                        const std::uint32_t globalXl = cx * info.chunkXlines + lx;
                        if(globalXl >= info.xlines) {
                            break;
                        }
                        if(Cancelled(cancel) || writerFailed.load()) {
                            cancelled = true;
                            break;
                        }
                        const int inlineNo = inlineBegin + static_cast<int>(globalIl);
                        const int xlineNo = xlineBegin + static_cast<int>(globalXl);
                        const int traceIndex = source->Volume().FindTraceIndex(
                            source->Volume().FindNearestInlineValue(static_cast<float>(inlineNo)),
                            source->Volume().FindNearestXlineValue(static_cast<float>(xlineNo)));
                        if(traceIndex < 0) {
                            quality.NoteMissing(); // missing trace stays NaN
                            continue;
                        }
                        if(!session.ReadTrace(traceIndex, traceSamples, error)) {
                            quality.NoteDamaged(inlineNo, xlineNo); // damaged trace stays NaN
                            continue;
                        }
                        tracesReadAtomic.fetch_add(1);
                        quality.NoteTrace(traceSamples);
                        for(std::uint32_t cs = 0; cs < info.ChunksS(); ++cs) {
                            std::vector<float>& buffer = buffers[cs];
                            if(writer.HasChunk(cs, ci, cx)) {
                                continue; // resumed chunk: keep it
                            }
                            for(std::uint32_t ls = 0; ls < info.chunkSamples; ++ls) {
                                const std::uint64_t globalSample =
                                    static_cast<std::uint64_t>(cs) * info.chunkSamples + ls;
                                if(globalSample >= info.samples) {
                                    break;
                                }
                                const int sample = sampleBegin + static_cast<int>(globalSample);
                                if(sample < 0 || sample >= static_cast<int>(traceSamples.size())) {
                                    continue;
                                }
                                const std::size_t offset =
                                    (static_cast<std::size_t>(ls) * info.chunkInlines + li) *
                                        info.chunkXlines + lx;
                                buffer[offset] = traceSamples[static_cast<std::size_t>(sample)];
                            }
                        }
                    }
                }
                if(cancelled) {
                    break;
                }
                for(std::uint32_t cs = 0; cs < info.ChunksS(); ++cs) {
                    if(writer.HasChunk(cs, ci, cx)) {
                        ++result.chunksSkipped;
                        ++state.chunksSkipped;
                        ++state.chunksDone;
                        continue;
                    }
                    ReadyChunk ready;
                    ready.cs = cs;
                    ready.ci = ci;
                    ready.cx = cx;
                    ready.values = std::move(buffers[cs]);
                    buffers[cs] = std::vector<float>(chunkFloats, std::numeric_limits<float>::quiet_NaN());
                    if(!pushChunk(std::move(ready))) {
                        cancelled = true;
                        break;
                    }
                    state.chunksDone = chunksWrittenAtomic.load();
                    state.bytesWritten = bytesWrittenAtomic.load();
                    state.elapsedSeconds = SecondsSince(start);
                    if(progress && !progress(state)) {
                        cancelled = true;
                        break;
                    }
                }
            }
        }
        state.phase = "finalizing";
        state.chunksDone = chunksWrittenAtomic.load();
        state.bytesWritten = bytesWrittenAtomic.load();
        state.elapsedSeconds = SecondsSince(start);
        if(progress) {
            progress(state);
        }
        encodeQueue.Close();
        if(encoderCount == 0) {
            writeQueue.Close(); // single-thread path: producer releases the writer
        }
        // parallel path: the LAST encoder closes writeQueue after draining, so
        // chunks still in the encode queue are never dropped by an early close.
        if(writerThread.joinable()) {
            writerThread.join();
        }
        for(std::thread& encoder : encoderThreads) {
            encoder.join();
        }
        if(writerFailed.load()) {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(writerErrorMutex);
                message = writerError;
            }
            writer.Finalize(error);
            quality.Finish(result);
            result.status = Status::Error(StatusCode::IoError,
                                          message.empty() ? "workspace write failed" : message);
            return result;
        }
        if(!writer.Finalize(error)) {
            quality.Finish(result);
            result.status = Status::Error(StatusCode::IoError, error);
            return result;
        }
        result.chunksWritten = chunksWrittenAtomic.load();
        result.tracesRead = tracesReadAtomic.load();
        result.bytesWritten = bytesWrittenAtomic.load();
        result.maxQueueDepth = maxQueueDepth;
        result.writeCalls = writer.WriteCalls();
        result.elapsedSeconds = SecondsSince(start);
        quality.Finish(result);
        result.status = cancelled
            ? Status::Error(StatusCode::Cancelled, "the transcode was cancelled; the workspace is resumable")
            : Status::Ok();
        return result;
    }

    for(std::uint32_t cs = 0; cs < info.ChunksS() && !cancelled; ++cs) {
        for(std::uint32_t ci = 0; ci < info.ChunksI() && !cancelled; ++ci) {
            for(std::uint32_t cx = 0; cx < info.ChunksX() && !cancelled; ++cx) {
                if(writer.HasChunk(cs, ci, cx)) {
                    ++result.chunksSkipped;
                    ++state.chunksSkipped;
                    ++state.chunksDone;
                    continue;
                }
                std::fill(chunk.begin(), chunk.end(), std::numeric_limits<float>::quiet_NaN());
                // Read every trace ONCE per chunk and scatter its samples: the
                // trace loop is outer, the sample loop is inner.
                for(std::uint32_t li = 0; li < info.chunkInlines && !cancelled; ++li) {
                    const int inlineNo = inlineBegin + static_cast<int>(ci * info.chunkInlines + li);
                    if(ci * info.chunkInlines + li >= info.inlines) {
                        break;
                    }
                    for(std::uint32_t lx = 0; lx < info.chunkXlines; ++lx) {
                        const int xlineNo = xlineBegin + static_cast<int>(cx * info.chunkXlines + lx);
                        if(cx * info.chunkXlines + lx >= info.xlines) {
                            break;
                        }
                        if(Cancelled(cancel)) {
                            cancelled = true;
                            break;
                        }
                        const int nearestInline = source->Volume().FindNearestInlineValue(
                            static_cast<float>(inlineNo));
                        const int nearestXline = source->Volume().FindNearestXlineValue(
                            static_cast<float>(xlineNo));
                        const int traceIndex = source->Volume().FindTraceIndex(nearestInline, nearestXline);
                        if(traceIndex < 0) {
                            quality.NoteMissing(); // missing trace stays NaN
                            continue;
                        }
                        if(!session.ReadTrace(traceIndex, traceSamples, error)) {
                            quality.NoteDamaged(inlineNo, xlineNo); // damaged trace stays NaN
                            continue;
                        }
                        for(std::uint32_t ls = 0; ls < info.chunkSamples; ++ls) {
                            if(cs * info.chunkSamples + ls >= info.samples) {
                                break;
                            }
                            const int sample = sampleBegin + static_cast<int>(cs * info.chunkSamples + ls);
                            if(sample < 0 || sample >= static_cast<int>(traceSamples.size())) {
                                continue;
                            }
                            const std::size_t offset =
                                (static_cast<std::size_t>(ls) * info.chunkInlines + li) *
                                    info.chunkXlines + lx;
                            chunk[offset] = traceSamples[static_cast<std::size_t>(sample)];
                        }
                        tracesReadAtomic.fetch_add(1);
                        quality.NoteTrace(traceSamples);
                    }
                }
                if(cancelled || writerFailed.load()) {
                    cancelled = true;
                    break;
                }
                ReadyChunk ready;
                ready.cs = cs;
                ready.ci = ci;
                ready.cx = cx;
                ready.values = std::move(chunk);
                chunk.assign(static_cast<std::size_t>(info.ChunkBytes() / sizeof(float)),
                             std::numeric_limits<float>::quiet_NaN());
                if(!pushChunk(std::move(ready))) {
                    cancelled = true;
                    break;
                }
                state.chunksDone = chunksWrittenAtomic.load();
                state.bytesWritten = bytesWrittenAtomic.load();
                state.elapsedSeconds = SecondsSince(start);
                if(progress && !progress(state)) {
                    cancelled = true;
                }
            }
        }
    }

    encodeQueue.Close();
    if(encoderCount == 0) {
        writeQueue.Close(); // single-thread path: producer releases the writer
    }
    // parallel path: the LAST encoder closes writeQueue after draining, so
    // chunks still in the encode queue are never dropped by an early close.
    if(writerThread.joinable()) {
        writerThread.join();
    }
    for(std::thread& encoder : encoderThreads) {
        encoder.join();
    }
    if(writerFailed.load()) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(writerErrorMutex);
            message = writerError;
        }
        writer.Finalize(error); // keep what was written: resumable
        quality.Finish(result);
        result.status = Status::Error(StatusCode::IoError,
                                      message.empty() ? "workspace write failed" : message);
        return result;
    }
    state.phase = "finalizing";
    state.chunksDone = chunksWrittenAtomic.load();
    state.bytesWritten = bytesWrittenAtomic.load();
    state.elapsedSeconds = SecondsSince(start);
    if(progress) {
        progress(state);
    }
    if(!writer.Finalize(error)) {
        quality.Finish(result);
        result.status = Status::Error(StatusCode::IoError, error);
        return result;
    }
    result.chunksWritten = chunksWrittenAtomic.load();
    result.tracesRead = tracesReadAtomic.load();
    result.bytesWritten = bytesWrittenAtomic.load();
    result.maxQueueDepth = maxQueueDepth;
    result.writeCalls = writer.WriteCalls();
    result.elapsedSeconds = SecondsSince(start);
    quality.Finish(result);
    result.status = cancelled
        ? Status::Error(StatusCode::Cancelled, "the transcode was cancelled; the workspace is resumable")
        : Status::Ok();
    return result;
}

} // namespace engine
} // namespace seismic
