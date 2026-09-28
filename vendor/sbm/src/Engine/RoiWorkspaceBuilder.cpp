#include "Engine/RoiWorkspaceBuilder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

#include <segyio/segy.h>

#include "Data/Sgy/SgyFileReader.h"
#include "Data/Sgy/SgySequentialScan.h"

namespace seismic {
namespace engine {
namespace {

double SecondsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1e6;
}

} // namespace

RoiBuildResult BuildRoiWorkspaceFromSegy(
    const std::filesystem::path& sgyPath,
    const std::filesystem::path& outFile,
    const RoiBuildOptions& options,
    CancelToken* cancel,
    const RoiBuildProgress& progress) {
    RoiBuildResult result;
    const auto start = std::chrono::steady_clock::now();

    if(options.inlineCount <= 0 || options.xlineCount <= 0 ||
       options.inlineStep <= 0 || options.xlineStep <= 0) {
        result.status = Status::Error(StatusCode::InvalidArgument, "the ROI box is empty or has a bad step");
        return result;
    }
    // The ROI is a real axis-value set, so a step-2 survey can be requested
    // without naming inlines that do not exist.
    const AxisDescriptor roiInlineAxis =
        AxisDescriptor::Uniform(options.firstInline, options.inlineStep, options.inlineCount);
    const AxisDescriptor roiXlineAxis =
        AxisDescriptor::Uniform(options.firstXline, options.xlineStep, options.xlineCount);

    SgyFileSummary summary;
    std::string summaryError;
    if(!SgyFileReader::ReadSummary(sgyPath, summary, summaryError)) {
        result.status = Status::Error(StatusCode::IoError,
                                      summaryError.empty() ? "cannot read the SEG-Y metadata" : summaryError);
        return result;
    }
    if(summary.formatSizeBytes != 4) {
        result.status = Status::Error(StatusCode::Unsupported,
                                      "the fused ROI builder needs 4-byte samples (IEEE float)");
        return result;
    }

    LayoutV5Info info;
    info.spec = options.spec;
    info.volumeInlines = static_cast<std::uint32_t>(options.inlineCount);
    info.volumeXlines = static_cast<std::uint32_t>(options.xlineCount);
    info.volumeSamples = static_cast<std::uint32_t>(summary.sampleCount);
    info.inlineMin = options.firstInline;
    info.xlineMin = options.firstXline;
    info.codec = options.codec;
    result.info = info;
    result.recordsExpected = static_cast<std::uint64_t>(options.inlineCount) * options.xlineCount;

    const std::uint32_t chunkI = std::max(1u, info.spec.chunkInline);
    const std::uint32_t chunkX = std::max(1u, info.spec.chunkXline);
    const std::uint32_t chunkT = std::max(1u, info.spec.chunkSample);
    const std::uint32_t chunksI = (info.volumeInlines + chunkI - 1) / chunkI;
    const std::uint32_t chunksX = (info.volumeXlines + chunkX - 1) / chunkX;
    const std::uint32_t chunksT = (info.volumeSamples + chunkT - 1) / chunkT;

    LayoutV5Writer writer;
    std::string writeError;
    if(!writer.Open(outFile, info, writeError)) {
        result.status = Status::Error(StatusCode::IoError, writeError);
        return result;
    }

    // Chunk accumulator for the current inline block: one buffer per (ci, cx, ct)
    // inside the block. Records arrive inline-major, so a whole block is filled
    // before it is flushed.
    const std::uint64_t bufferFloats = static_cast<std::uint64_t>(chunkI) * chunkX * chunkT;
    std::vector<std::vector<float>> buffers(static_cast<std::size_t>(chunksI) * chunksX * chunksT);
    std::vector<unsigned char> blockFilled(buffers.size(), 0);
    result.peakBufferBytes = buffers.size() * bufferFloats * sizeof(float) + (8u << 20);
    for(std::vector<float>& buffer : buffers) {
        buffer.assign(static_cast<std::size_t>(bufferFloats), std::numeric_limits<float>::quiet_NaN());
    }

    std::vector<float> sampleBuffer;
    int currentBlock = -1;
    auto flushBlock = [&]() -> bool {
        if(currentBlock < 0) {
            return true;
        }
        for(std::uint32_t ci = 0; ci < chunksI; ++ci) {
            for(std::uint32_t cx = 0; cx < chunksX; ++cx) {
                for(std::uint32_t ct = 0; ct < chunksT; ++ct) {
                    const std::size_t index =
                        (static_cast<std::size_t>(ci) * chunksX + cx) * chunksT + ct;
                    if(blockFilled[index] == 0) {
                        continue;
                    }
                    if(!writer.WriteChunk(ci, cx, ct, buffers[index].data(), writeError)) {
                        result.status = Status::Error(StatusCode::IoError, writeError);
                        return false;
                    }
                    ++result.chunksWritten;
                    std::fill(buffers[index].begin(), buffers[index].end(),
                              std::numeric_limits<float>::quiet_NaN());
                    blockFilled[index] = 0;
                }
            }
        }
        return true;
    };

    SgyScanOptions scan;
    scan.windowBytes = options.windowBytes;
    scan.readQueueDepth = options.readQueueDepth;
    scan.maxSourceBytes = options.maxSourceBytes;
    if(cancel != nullptr) {
        scan.shouldCancel = [cancel]() { return cancel->IsCancelled(); };
    }

    SgyScanResult scanResult;
    const bool scanned = ScanSegySequentially(
        sgyPath, scan,
        [&](int, const char* record, int recordBytes) {
            int inlineNo = 0;
            int xlineNo = 0;
            segy_get_tracefield_int(record, SEGY_TR_INLINE, &inlineNo);
            segy_get_tracefield_int(record, SEGY_TR_CROSSLINE, &xlineNo);
            int iIndex = 0;
            int xIndex = 0;
            if(!roiInlineAxis.ExactIndexOf(inlineNo, iIndex) ||
               !roiXlineAxis.ExactIndexOf(xlineNo, xIndex)) {
                return true;
            }
            const int block = iIndex / static_cast<int>(chunkI);
            if(block != currentBlock) {
                if(!flushBlock()) {
                    return false;
                }
                currentBlock = block;
            }
            const std::uint32_t ci = static_cast<std::uint32_t>(iIndex) / chunkI;
            const std::uint32_t cx = static_cast<std::uint32_t>(xIndex) / chunkX;
            const std::uint32_t li = static_cast<std::uint32_t>(iIndex) % chunkI;
            const std::uint32_t lx = static_cast<std::uint32_t>(xIndex) % chunkX;
            const int samples = (recordBytes - 240) / 4;
            const int usable = std::min(samples, static_cast<int>(info.volumeSamples));
            if(usable <= 0) {
                return true;
            }
            // The raw record bytes may be IBM float, little-endian or IEEE:
            // convert a copy in place with segyio so the workspace always stores
            // native IEEE floats.
            sampleBuffer.resize(static_cast<std::size_t>(usable));
            std::memcpy(sampleBuffer.data(), record + 240,
                        static_cast<std::size_t>(usable) * sizeof(float));
            if(segy_to_native(summary.formatCode, usable, sampleBuffer.data()) != SEGY_OK) {
                result.status = Status::Error(StatusCode::IoError, "sample conversion failed");
                return false;
            }
            const float* values = sampleBuffer.data();
            for(int t = 0; t < usable; ++t) {
                const std::uint32_t ct = static_cast<std::uint32_t>(t) / chunkT;
                const std::uint32_t lt = static_cast<std::uint32_t>(t) % chunkT;
                const std::size_t index =
                    (static_cast<std::size_t>(ci) * chunksX + cx) * chunksT + ct;
                std::vector<float>& buffer = buffers[index];
                buffer[(static_cast<std::size_t>(lt) * chunkX + lx) * chunkI + li] =
                    values[t];
                blockFilled[index] = 1;
            }
            ++result.recordsInRoi;
            if(progress && (result.recordsInRoi % 4096) == 0 &&
               !progress(static_cast<int>(result.recordsInRoi), static_cast<int>(result.recordsExpected))) {
                return false;
            }
            return true;
        },
        scanResult);

    if(cancel != nullptr && cancel->IsCancelled()) {
        result.status = Status::Error(StatusCode::Cancelled, "the fused ROI build was cancelled");
        return result;
    }
    if(!scanned && !scanResult.complete && !scanResult.cancelled && !scanResult.bounded) {
        result.status = Status::Error(StatusCode::IoError,
                                      scanResult.message.empty() ? "the sequential scan failed" : scanResult.message);
        return result;
    }
    result.sourceReadCalls = scanResult.stats.readCalls;
    result.sourceReadBytes = scanResult.stats.readBytes;

    if(!flushBlock()) {
        return result;
    }
    if(result.recordsInRoi == 0) {
        result.status = Status::Error(StatusCode::NotFound,
                                      "no trace inside the requested ROI box was found");
        return result;
    }
    result.complete = result.recordsInRoi == result.recordsExpected;
    if(!result.complete && !options.allowPartial) {
        // An incomplete ROI must not be published as a valid workspace: the
        // caller either fixes the axis values or asks for a diagnostic partial
        // build explicitly.
        result.status = Status::Error(
            StatusCode::InvalidArgument,
            "the ROI is not fully covered by the scanned range (" + std::to_string(result.recordsInRoi) +
                " of " + std::to_string(result.recordsExpected) +
                " records found); use allowPartial for a diagnostic build");
        return result;
    }
    if(!writer.Finalize(writeError)) {
        result.status = Status::Error(StatusCode::IoError, writeError);
        return result;
    }
    // A sidecar manifest records the coverage honestly; a partial build is never
    // silently a full workspace.
    {
        result.manifestPath = std::filesystem::path(outFile.wstring() + L".manifest.txt");
        std::ofstream manifest(result.manifestPath, std::ios::trunc);
        if(manifest) {
            manifest << "format=paged-v5-experimental\n"
                     << "complete=" << (result.complete ? "true" : "false") << "\n"
                     << "records=" << result.recordsInRoi << "\n"
                     << "recordsExpected=" << result.recordsExpected << "\n"
                     << "inlineAxis=" << roiInlineAxis.Describe() << "\n"
                     << "xlineAxis=" << roiXlineAxis.Describe() << "\n"
                     << "samples=" << info.volumeSamples << "\n"
                     << "codec=" << info.codec << "\n";
        }
    }
    result.wallSeconds = SecondsSince(start);
    result.status = Status::Ok();
    return result;
}

} // namespace engine
} // namespace seismic
