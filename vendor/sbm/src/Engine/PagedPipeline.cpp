#include "Engine/PagedPipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <system_error>
#include <vector>

#include "Data/Sgy/SgyReadSession.h"
#include "Engine/SgyVolumeSource.h"

namespace seismic {
namespace engine {
namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(const Clock::time_point& start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool IsCancelled(CancelToken* cancel) {
    return cancel != nullptr && cancel->IsCancelled();
}

Status CancelledStatus() {
    return Status::Error(StatusCode::Cancelled, "paged workspace build cancelled");
}

std::int64_t MtimeTicks(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

AxisDescriptor SliceAxis(const AxisDescriptor& source, int begin, int count) {
    std::vector<int> values;
    values.reserve(static_cast<std::size_t>(count));
    for(int i = 0; i < count; ++i) {
        values.push_back(source.ValueAt(begin + i));
    }
    return AxisDescriptor::FromValues(std::move(values));
}

AxisDescriptor DecimateAxis(const AxisDescriptor& source, std::uint32_t factor) {
    factor = std::max<std::uint32_t>(1, factor);
    std::vector<int> values;
    for(int i = 0; i < source.count; i += static_cast<int>(factor)) {
        values.push_back(source.ValueAt(i));
    }
    return AxisDescriptor::FromValues(std::move(values));
}

bool SameAxis(const AxisDescriptor& a, const AxisDescriptor& b) {
    if(a.count != b.count) {
        return false;
    }
    for(int i = 0; i < a.count; ++i) {
        if(a.ValueAt(i) != b.ValueAt(i)) {
            return false;
        }
    }
    return true;
}

bool ReuseComplete(const std::filesystem::path& path, const PagedWorkspaceInfo& expected,
                   PagedWorkspaceInfo& actual) {
    std::error_code ec;
    if(!std::filesystem::exists(path, ec) || ec) {
        return false;
    }
    PagedWorkspaceReader reader;
    std::string error;
    if(!reader.Open(path, error)) {
        return false;
    }
    actual = reader.Info();
    return actual.complete && actual.version == expected.version &&
           actual.algorithmVersion == expected.algorithmVersion &&
           actual.samples == expected.samples && actual.sampleIntervalUs == expected.sampleIntervalUs &&
           actual.sourceSize == expected.sourceSize &&
           actual.sourceMtimeTicks == expected.sourceMtimeTicks &&
           actual.sourceFingerprint == expected.sourceFingerprint &&
           actual.codec == expected.codec && actual.lodLevel == expected.lodLevel &&
           actual.lodFactorInlines == expected.lodFactorInlines &&
           actual.lodFactorXlines == expected.lodFactorXlines &&
           actual.lodFactorSamples == expected.lodFactorSamples &&
           SameAxis(actual.inlineAxis, expected.inlineAxis) &&
           SameAxis(actual.xlineAxis, expected.xlineAxis);
}

bool Report(const PagedProgressFn& callback, const PagedPipelineProgress& value,
            CancelToken* cancel) {
    if(IsCancelled(cancel)) {
        return false;
    }
    return !callback || callback(value);
}

float Reduce(const std::vector<float>& source, int sourceI, int sourceX, int sourceT,
             int i0, int i1, int x0, int x1, int t0, int t1, LodMethod method) {
    double sum = 0.0;
    double squares = 0.0;
    float peak = std::numeric_limits<float>::quiet_NaN();
    std::uint64_t count = 0;
    for(int t = t0; t < t1; ++t) {
        for(int x = x0; x < x1; ++x) {
            for(int i = i0; i < i1; ++i) {
                const std::size_t offset = (static_cast<std::size_t>(t) * sourceX + x) * sourceI + i;
                const float value = source[offset];
                if(!std::isfinite(value)) {
                    continue;
                }
                if(method == LodMethod::Decimate) {
                    return value;
                }
                // The production pyramid uses Average. Avoid computing RMS and
                // envelope accumulators for every source voxel as well: on a
                // 124GB volume that would add tens of billions of unnecessary
                // multiply/abs operations during L1/L2 construction.
                if(method == LodMethod::Rms) {
                    squares += static_cast<double>(value) * value;
                } else if(method == LodMethod::Envelope) {
                    if(!std::isfinite(peak) || std::abs(value) > std::abs(peak)) {
                        peak = value;
                    }
                } else {
                    sum += value;
                }
                ++count;
            }
        }
    }
    if(count == 0) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    if(method == LodMethod::Rms) {
        return static_cast<float>(std::sqrt(squares / static_cast<double>(count)));
    }
    if(method == LodMethod::Envelope) {
        return peak;
    }
    return static_cast<float>(sum / static_cast<double>(count));
}

PagedBuildResult ErrorResult(const std::filesystem::path& path, StatusCode code,
                             const std::string& message, const Clock::time_point& start) {
    PagedBuildResult result;
    result.path = path;
    result.status = Status::Error(code, message);
    result.elapsedSeconds = SecondsSince(start);
    return result;
}

} // namespace

std::filesystem::path PagedLodPath(const std::filesystem::path& l0Path, int level) {
    const std::string extension = l0Path.extension().u8string();
    const std::string stem = l0Path.stem().u8string();
    return l0Path.parent_path() /
           std::filesystem::u8path(stem + ".l" + std::to_string(level) + extension);
}

PagedBuildResult TranscodeSegyToPagedWorkspace(
    const std::filesystem::path& segyPath,
    const std::filesystem::path& targetPath,
    const PagedPipelineOptions& options,
    CancelToken* cancel,
    const PagedProgressFn& progress) {
    const auto start = Clock::now();
    PagedBuildResult result;
    result.path = targetPath;

    Status openStatus;
    std::unique_ptr<SgyVolumeSource> source = SgyVolumeSource::Open(segyPath, openStatus);
    if(!source) {
        return ErrorResult(targetPath, openStatus.code, openStatus.message, start);
    }
    const DatasetMetadata& meta = source->Metadata();
    const SgyIndexPtr index = source->Volume().Index();
    if(!index || !index->complete) {
        return ErrorResult(targetPath, StatusCode::NotIndexed,
                           "a complete SEG-Y index is required for paged transcode", start);
    }

    const int beginI = options.inlineCount > 0 ? options.inlineBegin : 0;
    const int beginX = options.xlineCount > 0 ? options.xlineBegin : 0;
    const int countI = options.inlineCount > 0 ? options.inlineCount : meta.inlineAxis.count;
    const int countX = options.xlineCount > 0 ? options.xlineCount : meta.xlineAxis.count;
    if(beginI < 0 || beginX < 0 || countI <= 0 || countX <= 0 ||
       beginI + countI > meta.inlineAxis.count || beginX + countX > meta.xlineAxis.count) {
        return ErrorResult(targetPath, StatusCode::InvalidArgument,
                           "paged transcode ROI is outside the SEG-Y axes", start);
    }

    std::error_code ec;
    const std::uintmax_t sourceSize = std::filesystem::file_size(segyPath, ec);
    if(ec) {
        return ErrorResult(targetPath, StatusCode::IoError,
                           "cannot query SEG-Y file size: " + ec.message(), start);
    }
    PagedWorkspaceInfo info;
    info.inlineAxis = SliceAxis(meta.inlineAxis, beginI, countI);
    info.xlineAxis = SliceAxis(meta.xlineAxis, beginX, countX);
    info.samples = static_cast<std::uint32_t>(meta.sampleCount);
    info.sampleIntervalUs = static_cast<std::uint32_t>(std::max(0, meta.sampleIntervalUs));
    info.sourcePath = std::filesystem::absolute(segyPath, ec).u8string();
    if(ec) {
        info.sourcePath = segyPath.u8string();
    }
    info.sourceSize = sourceSize;
    info.sourceMtimeTicks = MtimeTicks(segyPath);
    info.sourceFingerprint = PagedWorkspaceFingerprint(segyPath, sourceSize);
    info.codec = options.codec;
    info.codecLevel = options.codecLevel;
    info.coverageInlineMin = info.inlineAxis.ValueAt(0);
    info.coverageInlineMax = info.inlineAxis.ValueAt(info.inlineAxis.count - 1);
    info.coverageXlineMin = info.xlineAxis.ValueAt(0);
    info.coverageXlineMax = info.xlineAxis.ValueAt(info.xlineAxis.count - 1);
    info.buildGeneration = options.buildGeneration;

    PagedWorkspaceInfo reusedInfo;
    if(ReuseComplete(targetPath, info, reusedInfo)) {
        result.status = Status::Ok();
        result.info = reusedInfo;
        result.reused = true;
        result.chunksSkipped = reusedInfo.ChunkCount();
        result.elapsedSeconds = SecondsSince(start);
        return result;
    }

    PagedWorkspaceWriter writer;
    std::string error;
    if(!writer.Open(targetPath, info, options.resume, error)) {
        return ErrorResult(targetPath, StatusCode::IoError, error, start);
    }
    const std::uint64_t initiallyComplete = writer.ChunksWritten();
    const std::size_t chunkFloats = static_cast<std::size_t>(info.chunkInlines) *
                                    info.chunkXlines * info.chunkSamples;
    std::vector<std::vector<float>> chunks(info.ChunksT(),
        std::vector<float>(chunkFloats, std::numeric_limits<float>::quiet_NaN()));
    std::vector<float> trace;
    SgyReadSession readSession;
    if(!readSession.Open(index, error)) {
        return ErrorResult(targetPath, StatusCode::IoError, error, start);
    }

    PagedPipelineProgress state;
    state.phase = "l0-transcode";
    state.chunksTotal = info.ChunkCount();
    state.chunksDone = initiallyComplete;
    state.chunksSkipped = initiallyComplete;

    // P9 (paleo): damaged source traces are skipped and NaN-filled instead of
    // failing the whole pyramid; the first 32 are recorded for the report.
    std::uint64_t missingTraces = 0;
    std::uint64_t damagedTraces = 0;
    std::uint64_t sanitizedSamples = 0;
    std::uint64_t sanitizedTraces = 0;
    std::vector<std::pair<int, int>> damagedSample;
    float valueMin = std::numeric_limits<float>::infinity();
    float valueMax = -std::numeric_limits<float>::infinity();
    const auto noteTrace = [&valueMin, &valueMax](const std::vector<float>& trace) {
        for(const float v : trace) {
            if(!std::isfinite(v)) {
                continue;
            }
            if(v < valueMin) {
                valueMin = v;
            }
            if(v > valueMax) {
                valueMax = v;
            }
        }
    };
    const auto applyQuality = [&](PagedBuildResult& r) {
        r.missingTraceCount = missingTraces;
        r.damagedTraceCount = damagedTraces;
        r.sanitizedSampleCount = sanitizedSamples;
        r.sanitizedTraceCount = sanitizedTraces;
        r.damagedTraceSample = damagedSample;
        if(valueMin != std::numeric_limits<float>::infinity()) {
            r.valueMin = valueMin;
        }
        if(valueMax != -std::numeric_limits<float>::infinity()) {
            r.valueMax = valueMax;
        }
    };
    if(!Report(progress, state, cancel)) {
        result = ErrorResult(targetPath, StatusCode::Cancelled, "paged workspace build cancelled", start);
        result.info = info;
        return result;
    }

    for(std::uint32_t ci = 0; ci < info.ChunksI(); ++ci) {
        for(std::uint32_t cx = 0; cx < info.ChunksX(); ++cx) {
            bool allComplete = true;
            for(std::uint32_t ct = 0; ct < info.ChunksT(); ++ct) {
                if(!writer.Complete(ci, cx, ct)) {
                    allComplete = false;
                    std::fill(chunks[ct].begin(), chunks[ct].end(),
                              std::numeric_limits<float>::quiet_NaN());
                }
            }
            if(allComplete) {
                continue;
            }
            for(std::uint32_t li = 0; li < info.chunkInlines; ++li) {
                const int globalI = static_cast<int>(ci * info.chunkInlines + li);
                if(globalI >= info.inlineAxis.count) {
                    break;
                }
                const int inlineNo = info.inlineAxis.ValueAt(globalI);
                for(std::uint32_t lx = 0; lx < info.chunkXlines; ++lx) {
                    const int globalX = static_cast<int>(cx * info.chunkXlines + lx);
                    if(globalX >= info.xlineAxis.count) {
                        break;
                    }
                    if(IsCancelled(cancel)) {
                        result = ErrorResult(targetPath, StatusCode::Cancelled,
                                             "paged workspace build cancelled", start);
                        result.info = info;
                        result.tracesRead = state.tracesRead;
                        applyQuality(result);
                        return result;
                    }
                    const int xlineNo = info.xlineAxis.ValueAt(globalX);
                    const int traceIndex = index->FindTraceIndex(inlineNo, xlineNo);
                    if(traceIndex < 0) {
                        ++missingTraces;
                        continue;
                    }
                    if(!readSession.ReadTrace(traceIndex, trace, error)) {
                        ++damagedTraces;
                        if(damagedSample.size() < 32) {
                            damagedSample.emplace_back(inlineNo, xlineNo);
                        }
                        continue; // damaged trace stays NaN
                    }
                    sanitizedSamples += readSession.LastSanitizedSampleCount();
                    sanitizedTraces += readSession.LastSanitizedSampleCount() > 0;
                    ++state.tracesRead;
                    noteTrace(trace);
                    for(std::uint32_t sample = 0; sample < info.samples; ++sample) {
                        const std::uint32_t ct = sample / info.chunkSamples;
                        if(writer.Complete(ci, cx, ct)) {
                            continue;
                        }
                        const std::uint32_t lt = sample % info.chunkSamples;
                        chunks[ct][(static_cast<std::size_t>(lt) * info.chunkXlines + lx) *
                                   info.chunkInlines + li] = trace[sample];
                    }
                }
            }
            for(std::uint32_t ct = 0; ct < info.ChunksT(); ++ct) {
                if(writer.Complete(ci, cx, ct)) {
                    continue;
                }
                if(!writer.WriteChunk(ci, cx, ct, chunks[ct].data(), error)) {
                    result = ErrorResult(targetPath, StatusCode::IoError, error, start);
                    result.info = info;
                    result.tracesRead = state.tracesRead;
                    applyQuality(result);
                    return result;
                }
                ++state.chunksDone;
                state.bytesWritten = writer.BytesWritten();
                state.elapsedSeconds = SecondsSince(start);
                if(!Report(progress, state, cancel)) {
                    result = ErrorResult(targetPath, StatusCode::Cancelled,
                                         "paged workspace build cancelled", start);
                    result.info = info;
                    result.tracesRead = state.tracesRead;
                    applyQuality(result);
                    result.bytesWritten = state.bytesWritten;
                    return result;
                }
            }
        }
    }
    state.phase = "finalizing";
    Report(progress, state, cancel);
    if(IsCancelled(cancel)) {
        result = ErrorResult(targetPath, StatusCode::Cancelled, "paged workspace build cancelled", start);
        result.info = info;
        applyQuality(result);
        return result;
    }
    const std::uint64_t bytesWritten = writer.BytesWritten();
    const std::uint64_t finalChunks = writer.ChunksWritten();
    if(!writer.Finalize(error)) {
        result = ErrorResult(targetPath, StatusCode::IoError, error, start);
        result.info = info;
        result.tracesRead = state.tracesRead;
        applyQuality(result);
        return result;
    }
    result.status = Status::Ok();
    info.complete = true;
    result.info = info;
    result.chunksWritten = finalChunks - initiallyComplete;
    result.chunksSkipped = initiallyComplete;
    result.tracesRead = state.tracesRead;
    result.bytesWritten = bytesWritten;
    result.elapsedSeconds = SecondsSince(start);
    applyQuality(result);
    return result;
}

PagedBuildResult BuildPagedLodFromL0(
    const std::filesystem::path& l0Path,
    const std::filesystem::path& targetPath,
    std::uint32_t level,
    std::uint32_t factorInlines,
    std::uint32_t factorXlines,
    std::uint32_t factorSamples,
    LodMethod method,
    const PagedPipelineOptions& options,
    CancelToken* cancel,
    const PagedProgressFn& progress) {
    const auto start = Clock::now();
    factorInlines = std::max<std::uint32_t>(1, factorInlines);
    factorXlines = std::max<std::uint32_t>(1, factorXlines);
    factorSamples = std::max<std::uint32_t>(1, factorSamples);

    PagedWorkspaceReader source;
    std::string error;
    if(!source.Open(l0Path, error)) {
        return ErrorResult(targetPath, StatusCode::IoError, error, start);
    }
    const PagedWorkspaceInfo sourceInfo = source.Info();
    if(!sourceInfo.complete || sourceInfo.lodLevel != 0) {
        return ErrorResult(targetPath, StatusCode::InvalidArgument,
                           "LOD source must be a complete L0 paged workspace", start);
    }
    source.SetPageCache(std::make_shared<ChunkCache>(256ull * 1024ull * 1024ull));

    PagedWorkspaceInfo info = sourceInfo;
    info.complete = false;
    info.inlineAxis = DecimateAxis(sourceInfo.inlineAxis, factorInlines);
    info.xlineAxis = DecimateAxis(sourceInfo.xlineAxis, factorXlines);
    info.samples = (sourceInfo.samples + factorSamples - 1) / factorSamples;
    info.lodLevel = level;
    info.lodMethod = static_cast<std::uint32_t>(method);
    info.lodFactorInlines = factorInlines;
    info.lodFactorXlines = factorXlines;
    info.lodFactorSamples = factorSamples;
    info.lodAlgorithmVersion = kLodAlgorithmVersion;
    info.lodSourceHash = sourceInfo.sourceFingerprint;
    info.codec = options.codec;
    info.codecLevel = options.codecLevel;
    info.coverageInlineMin = info.inlineAxis.ValueAt(0);
    info.coverageInlineMax = info.inlineAxis.ValueAt(info.inlineAxis.count - 1);
    info.coverageXlineMin = info.xlineAxis.ValueAt(0);
    info.coverageXlineMax = info.xlineAxis.ValueAt(info.xlineAxis.count - 1);
    info.buildGeneration = options.buildGeneration + level;

    PagedWorkspaceInfo reusedInfo;
    if(ReuseComplete(targetPath, info, reusedInfo)) {
        PagedBuildResult result;
        result.status = Status::Ok();
        result.path = targetPath;
        result.info = reusedInfo;
        result.reused = true;
        result.chunksSkipped = reusedInfo.ChunkCount();
        result.elapsedSeconds = SecondsSince(start);
        return result;
    }

    PagedWorkspaceWriter writer;
    if(!writer.Open(targetPath, info, options.resume, error)) {
        return ErrorResult(targetPath, StatusCode::IoError, error, start);
    }
    const std::uint64_t initiallyComplete = writer.ChunksWritten();
    const std::size_t chunkFloats = static_cast<std::size_t>(info.chunkInlines) *
                                    info.chunkXlines * info.chunkSamples;
    std::vector<float> target(chunkFloats, std::numeric_limits<float>::quiet_NaN());
    PagedPipelineProgress state;
    state.phase = "l" + std::to_string(level) + "-build";
    state.chunksTotal = info.ChunkCount();
    state.chunksDone = initiallyComplete;
    state.chunksSkipped = initiallyComplete;

    for(std::uint32_t ci = 0; ci < info.ChunksI(); ++ci) {
        for(std::uint32_t cx = 0; cx < info.ChunksX(); ++cx) {
            for(std::uint32_t ct = 0; ct < info.ChunksT(); ++ct) {
                if(writer.Complete(ci, cx, ct)) {
                    continue;
                }
                if(IsCancelled(cancel)) {
                    PagedBuildResult result = ErrorResult(targetPath, StatusCode::Cancelled,
                                                          "paged LOD build cancelled", start);
                    result.info = info;
                    return result;
                }
                std::fill(target.begin(), target.end(), std::numeric_limits<float>::quiet_NaN());
                const int outI0 = static_cast<int>(ci * info.chunkInlines);
                const int outX0 = static_cast<int>(cx * info.chunkXlines);
                const int outT0 = static_cast<int>(ct * info.chunkSamples);
                const int outICount = std::min<int>(info.chunkInlines, info.inlineAxis.count - outI0);
                const int outXCount = std::min<int>(info.chunkXlines, info.xlineAxis.count - outX0);
                const int outTCount = std::min<int>(info.chunkSamples, static_cast<int>(info.samples) - outT0);
                const int srcI0 = outI0 * static_cast<int>(factorInlines);
                const int srcX0 = outX0 * static_cast<int>(factorXlines);
                const int srcT0 = outT0 * static_cast<int>(factorSamples);
                const int srcICount = std::min(sourceInfo.inlineAxis.count - srcI0,
                                               outICount * static_cast<int>(factorInlines));
                const int srcXCount = std::min(sourceInfo.xlineAxis.count - srcX0,
                                               outXCount * static_cast<int>(factorXlines));
                const int srcTCount = std::min(static_cast<int>(sourceInfo.samples) - srcT0,
                                               outTCount * static_cast<int>(factorSamples));
                std::vector<float> sourceBox;
                if(!source.ReadBox(sourceInfo.inlineAxis.ValueAt(srcI0),
                                   sourceInfo.xlineAxis.ValueAt(srcX0), srcT0,
                                   srcICount, srcXCount, srcTCount, sourceBox, error)) {
                    return ErrorResult(targetPath, StatusCode::IoError, error, start);
                }
                for(int t = 0; t < outTCount; ++t) {
                    for(int x = 0; x < outXCount; ++x) {
                        for(int i = 0; i < outICount; ++i) {
                            const int si0 = i * static_cast<int>(factorInlines);
                            const int sx0 = x * static_cast<int>(factorXlines);
                            const int st0 = t * static_cast<int>(factorSamples);
                            const float value = Reduce(sourceBox, srcICount, srcXCount, srcTCount,
                                si0, std::min(srcICount, si0 + static_cast<int>(factorInlines)),
                                sx0, std::min(srcXCount, sx0 + static_cast<int>(factorXlines)),
                                st0, std::min(srcTCount, st0 + static_cast<int>(factorSamples)), method);
                            target[(static_cast<std::size_t>(t) * info.chunkXlines + x) *
                                   info.chunkInlines + i] = value;
                        }
                    }
                }
                if(!writer.WriteChunk(ci, cx, ct, target.data(), error)) {
                    return ErrorResult(targetPath, StatusCode::IoError, error, start);
                }
                ++state.chunksDone;
                state.bytesWritten = writer.BytesWritten();
                state.elapsedSeconds = SecondsSince(start);
                if(!Report(progress, state, cancel)) {
                    PagedBuildResult result = ErrorResult(targetPath, StatusCode::Cancelled,
                                                          "paged LOD build cancelled", start);
                    result.info = info;
                    return result;
                }
            }
        }
    }
    const std::uint64_t bytesWritten = writer.BytesWritten();
    const std::uint64_t finalChunks = writer.ChunksWritten();
    if(!writer.Finalize(error)) {
        return ErrorResult(targetPath, StatusCode::IoError, error, start);
    }
    PagedBuildResult result;
    result.status = Status::Ok();
    result.path = targetPath;
    info.complete = true;
    result.info = info;
    result.chunksWritten = finalChunks - initiallyComplete;
    result.chunksSkipped = initiallyComplete;
    result.bytesWritten = bytesWritten;
    result.elapsedSeconds = SecondsSince(start);
    return result;
}

PagedPyramidResult BuildPagedPyramid(
    const std::filesystem::path& segyPath,
    const std::filesystem::path& l0Path,
    const PagedPipelineOptions& options,
    CancelToken* cancel,
    const PagedProgressFn& progress) {
    PagedPyramidResult result;
    result.l0 = TranscodeSegyToPagedWorkspace(segyPath, l0Path, options, cancel, progress);
    if(!result.l0.status.ok()) {
        result.status = result.l0.status;
        return result;
    }
    if(options.buildLod1) {
        result.l1 = BuildPagedLodFromL0(l0Path, PagedLodPath(l0Path, 1), 1, 4, 4, 1,
                                             LodMethod::Average, options, cancel, progress);
        if(!result.l1.status.ok()) {
            result.status = result.l1.status;
            return result;
        }
    }
    if(options.buildLod2) {
        result.l2 = BuildPagedLodFromL0(l0Path, PagedLodPath(l0Path, 2), 2, 8, 8, 1,
                                             LodMethod::Average, options, cancel, progress);
        if(!result.l2.status.ok()) {
            result.status = result.l2.status;
            return result;
        }
    }
    result.status = Status::Ok();
    return result;
}

} // namespace engine
} // namespace seismic
