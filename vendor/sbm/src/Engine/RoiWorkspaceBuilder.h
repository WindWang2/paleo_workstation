#pragma once

// Stage K/L fusion: build a local (ROI) workspace in ONE sequential pass over
// the SEG-Y, consuming the samples while the scan streams (strategy D). No
// complete index is built first: records are filtered by their own trace
// headers and written into a v5 layout with the chosen chunk/page geometry.
//
// The output is an experimental v5 container in its own file; the stable
// workspace format and the source file are never touched.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "Engine/LayoutV5.h"
#include "Engine/Types.h"

namespace seismic {
namespace engine {

struct RoiBuildOptions {
    // The ROI is a set of real axis values: first + k * step for k in
    // [0, count). A step-2 survey must pass inlineStep=2, otherwise the request
    // names inlines that do not exist.
    int firstInline = 0;
    int inlineCount = 1;
    int inlineStep = 1;
    int firstXline = 0;
    int xlineCount = 1;
    int xlineStep = 1;
    // Diagnostic only: publish an incomplete ROI (bounded scan) and mark it in
    // the manifest. The default refuses to publish an incomplete ROI.
    bool allowPartial = false;
    LayoutV5Spec spec;                 // default: b8x8x256 with 8x8x16 pages
    std::uint32_t codec = 0;           // 0 = raw pages, 1 = zstd pages
    std::size_t windowBytes = 8u << 20;
    int readQueueDepth = 1;
    // Diagnostic bound: stop after covering this many source bytes (0 = whole
    // file). A bounded scan can only contain records inside the covered range.
    std::uint64_t maxSourceBytes = 0;
};

struct RoiBuildResult {
    Status status;
    LayoutV5Info info;
    std::uint64_t recordsInRoi = 0;
    std::uint64_t recordsExpected = 0;
    std::uint64_t sourceReadCalls = 0;
    std::uint64_t sourceReadBytes = 0;
    std::uint64_t chunksWritten = 0;
    std::uint64_t peakBufferBytes = 0;
    double wallSeconds = 0.0;
    // True only when every requested axis value was found and written.
    bool complete = false;
    std::filesystem::path manifestPath;
};

using RoiBuildProgress = std::function<bool(int recordsInRoi, int recordsExpected)>;

// Streams the SEG-Y once and writes every trace inside the ROI box into the v5
// container. Fails (with a status) when the source format is not 4-byte IEEE
// float or when the ROI box is empty.
RoiBuildResult BuildRoiWorkspaceFromSegy(
    const std::filesystem::path& sgyPath,
    const std::filesystem::path& outFile,
    const RoiBuildOptions& options,
    CancelToken* cancel = nullptr,
    const RoiBuildProgress& progress = {});

} // namespace engine
} // namespace seismic
