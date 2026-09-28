#include "Engine/QuickOpen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>

#include "Data/Sgy/SgyFileReader.h"
#include "Data/Sgy/SgyIndexBuilder.h"
#include "Data/Sgy/SgyReadSession.h"

namespace seismic {
namespace engine {
namespace {

double MsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1000.0;
}

bool Cancelled(const CancelToken* cancel) {
    return cancel != nullptr && cancel->IsCancelled();
}

} // namespace

QuickOpenResult QuickOpenSegyPreview(const std::filesystem::path& path,
                                     int maxColumns,
                                     CancelToken* cancel) {
    QuickOpenResult result;
    const auto start = std::chrono::steady_clock::now();

    if(Cancelled(cancel)) {
        result.status = Status::Error(StatusCode::Cancelled, "request cancelled");
        return result;
    }
    maxColumns = std::clamp(maxColumns, 1, 4096);

    // 1) Metadata: text header + binary header + declared ranges. Bounded reads.
    SgyFileSummary summary;
    {
        const auto stage = std::chrono::steady_clock::now();
        std::string error;
        if(!SgyFileReader::ReadSummary(path, summary, error)) {
            result.status = Status::Error(StatusCode::IoError,
                                          error.empty() ? "cannot read the SEG-Y metadata" : error);
            return result;
        }
        result.times.metadataMs = MsSince(stage);
        result.metadata.name = summary.path.filename().u8string();
        result.metadata.path = summary.path;
        result.metadata.fileSize = summary.fileSize;
        result.metadata.traceCount = summary.traceCount;
        result.metadata.sampleCount = summary.sampleCount;
        result.metadata.sampleIntervalUs = summary.sampleIntervalUs;
        result.metadata.inlineMin = summary.declaredInlineMin;
        result.metadata.inlineMax = summary.declaredInlineMax;
        result.metadata.xlineMin = summary.declaredXlineMin;
        result.metadata.xlineMax = summary.declaredXlineMax;
        result.metadata.ruleBased = false;
        result.metadata.indexComplete = false;
        result.times.readCalls += 2; // text + binary header
    }
    {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        const std::int64_t mtime = ec ? 0 : static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::filesystem::last_write_time(path, ec).time_since_epoch()).count());
        result.identity = path.u8string() + "|" + std::to_string(size) + "|" + std::to_string(mtime);
    }

    const std::uint64_t recordBytes = 240 +
        static_cast<std::uint64_t>(std::max(0, summary.sampleCount)) *
            static_cast<std::uint64_t>(std::max(1, summary.formatSizeBytes));

    // 2) Rule-grid probe: a bounded number of header reads, never a full scan.
    {
        const auto stage = std::chrono::steady_clock::now();
        std::string probeError;
        if(!ProbeSgyRuleLayout(path, summary.traceCount, result.layout, probeError)) {
            result.times.probeMs = MsSince(stage);
            result.ruleVerified = false;
            result.fallbackReason = probeError.empty() ? "rule layout not verified" : probeError;
            result.verificationSummary = "fallback (no preview): " + result.fallbackReason;
            result.times.totalMs = MsSince(start);
            result.status = Status::Ok();
            return result;
        }
        result.times.probeMs = MsSince(stage);
        result.probeMatched = result.layout.matchedProbes;
        result.probeTotal = result.layout.probeCount;
        result.times.readCalls += static_cast<std::uint64_t>(std::max(1, result.layout.probeCount)) + 8;
        result.metadata.ruleBased = true;
        result.metadata.inlineMin = result.layout.firstInline;
        result.metadata.inlineMax = result.layout.lastInline;
        result.metadata.xlineMin = result.layout.firstXline;
        result.metadata.xlineMax = result.layout.lastXline;
        result.metadata.indexComplete = false; // provisional until the scan finishes
    }

    // In-memory rule snapshot: provides the read session API without touching
    // the persistent index cache. Every located trace is header-validated.
    const SgyIndexPtr snapshot = SgyIndexBuilder::BuildRuleBasedSnapshot(
        summary.path, summary.traceCount, summary.sampleCount, summary.sampleIntervalUs,
        summary.formatCode, summary.endianness, summary.encoding, result.layout);

    SgyReadSession session;
    std::string error;
    if(!session.Open(snapshot, error)) {
        result.ruleVerified = false;
        result.fallbackReason = error.empty() ? "cannot open a read session" : error;
        result.verificationSummary = "fallback (no preview): " + result.fallbackReason;
        result.times.totalMs = MsSince(start);
        result.status = Status::Ok();
        return result;
    }

    // 3) Boundary verification: first / middle / last trace and the last inline
    //    corner are read back and validated against the rule headers.
    {
        const auto stage = std::chrono::steady_clock::now();
        int verified = 0;
        int attempted = 0;
        std::string firstFailure;
        int firstFailureOrdinal = -1;
        std::vector<std::pair<int, int>> boundary;
        int midInline = result.layout.firstInline;
        int midXline = result.layout.firstXline;
        const int midOrdinal = std::max(0, result.layout.traceCount / 2);
        if(!result.layout.ExpectedInlineXline(midOrdinal, midInline, midXline)) {
            midInline = result.layout.firstInline;
            midXline = result.layout.firstXline;
        }
        boundary.emplace_back(result.layout.firstInline, result.layout.firstXline);
        boundary.emplace_back(midInline, midXline);
        boundary.emplace_back(result.layout.lastInline, result.layout.lastXline);
        boundary.emplace_back(result.layout.lastInline, result.layout.firstXline);
        std::vector<float> samples;
        for(const auto& point : boundary) {
            if(Cancelled(cancel)) {
                result.status = Status::Error(StatusCode::Cancelled, "request cancelled");
                return result;
            }
            ++attempted;
            const int ordinal = result.layout.TraceIndexFor(point.first, point.second);
            if(ordinal < 0) {
                if(firstFailure.empty()) {
                    firstFailure = "no trace at " + std::to_string(point.first) + "/" +
                                   std::to_string(point.second);
                }
                continue;
            }
            ++result.times.readCalls;
            result.times.requestedBytes += recordBytes;
            if(session.ReadTrace(ordinal, samples, error)) {
                ++verified;
            } else if(firstFailure.empty()) {
                firstFailure = error;
                firstFailureOrdinal = ordinal;
            }
        }
        result.times.verifyMs = MsSince(stage);
        result.ruleVerified = attempted > 0 && verified == attempted;
        {
            std::ostringstream text;
            text << "boundary " << verified << "/" << attempted << " verified";
            result.verificationSummary = text.str();
        }
        if(!result.ruleVerified) {
            result.fallbackReason = "boundary verification failed at trace " +
                                    std::to_string(firstFailureOrdinal) + ": " + firstFailure;
            result.verificationSummary += " -> fallback";
            result.times.totalMs = MsSince(start);
            result.status = Status::Ok();
            return result;
        }
    }

    // 4) First real slice: bounded central-inline preview. Columns that are not
    //    read stay NaN and are rendered as pending, never as zero amplitude.
    {
        const auto stage = std::chrono::steady_clock::now();
        // Snap the central location to the grid: the arithmetic middle of a
        // stepped axis is not necessarily a real trace.
        int inlineNo = (result.layout.firstInline + result.layout.lastInline) / 2;
        int selectedXline = (result.layout.firstXline + result.layout.lastXline) / 2;
        if(!result.layout.ExpectedInlineXline(std::max(0, result.layout.traceCount / 2),
                                              inlineNo, selectedXline)) {
            inlineNo = result.layout.firstInline;
            selectedXline = result.layout.firstXline;
        }
        result.selectedInline = inlineNo;
        result.selectedXline = selectedXline;
        const int xlineCount = std::max(1, result.layout.xlineCount);
        const int step = std::max(1, (xlineCount + maxColumns - 1) / maxColumns);
        result.columnsTotal = xlineCount;
        const int sampleCount = std::max(0, summary.sampleCount);
        std::vector<float> values(static_cast<std::size_t>(xlineCount) * sampleCount,
                                  std::numeric_limits<float>::quiet_NaN());
        std::vector<float> samples;
        int columnsRead = 0;
        for(int column = 0; column < xlineCount; column += step) {
            if(Cancelled(cancel)) {
                result.status = Status::Error(StatusCode::Cancelled, "request cancelled");
                return result;
            }
            const int xlineNo = result.layout.firstXline + column * result.layout.xlineStep;
            const int ordinal = result.layout.TraceIndexFor(inlineNo, xlineNo);
            if(ordinal < 0) {
                continue;
            }
            ++result.times.readCalls;
            result.times.requestedBytes += recordBytes;
            if(!session.ReadTrace(ordinal, samples, error)) {
                continue;
            }
            for(int s = 0; s < sampleCount && s < static_cast<int>(samples.size()); ++s) {
                const int row = sampleCount - 1 - s;
                values[static_cast<std::size_t>(row) * xlineCount + column] = samples[static_cast<std::size_t>(s)];
            }
            ++columnsRead;
        }
        result.times.firstReadMs = MsSince(stage);
        result.columnsRead = columnsRead;

        const auto decodeStart = std::chrono::steady_clock::now();
        result.preview.width = xlineCount;
        result.preview.height = sampleCount;
        result.preview.values = std::move(values);
        SgyVolume::Recolorize(result.preview);
        result.times.decodeMs = MsSince(decodeStart);
    }

    // 5) Publish: hand the preview over the way the Viewer does (shared copy).
    {
        const auto stage = std::chrono::steady_clock::now();
        auto published = std::make_shared<const SgySliceImage>(result.preview);
        volatile std::size_t publishedBytes = published->values.size();
        (void)publishedBytes;
        result.times.publishMs = MsSince(stage);
    }

    result.times.totalMs = MsSince(start);
    result.status = Status::Ok();
    return result;
}

} // namespace engine
} // namespace seismic