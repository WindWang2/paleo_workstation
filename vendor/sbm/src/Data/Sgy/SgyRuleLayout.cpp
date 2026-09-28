#include "Data/Sgy/SgyRuleLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <vector>

#include <segyio/segy.h>

#include "Data/Sgy/SgyIo.h"

namespace seismic {
namespace {

struct TraceKey {
    int inlineNo = 0;
    int xlineNo = 0;
    bool valid = false;
};

bool ReadTraceKey(segy_datasource* file, int traceIndex, TraceKey& key) {
    std::array<char, SEGY_TRACE_HEADER_SIZE> header{};
    if(segy_read_standard_traceheader(file, traceIndex, header.data()) != SEGY_OK) {
        return false;
    }
    // P5: includes the paleo field record @8 / CDP @20 fallback for the
    // (inlineNo == 0 && xlineNo == 0) case.
    const sgyio::SgyTraceKeyWords words = sgyio::ReadTraceKeyWords(header.data());
    key.inlineNo = words.inlineNo;
    key.xlineNo = words.xlineNo;
    key.valid = !(words.inlineNo == 0 && words.xlineNo == 0);
    return key.valid;
}

} // namespace

int SgyRuleLayout::TraceIndexFor(int inlineNo, int xlineNo) const {
    if(!valid || inlineStep == 0 || xlineStep == 0) {
        return -1;
    }
    const int inlineOffset = (inlineNo - firstInline);
    const int xlineOffset = (xlineNo - firstXline);
    if(inlineOffset % inlineStep != 0 || xlineOffset % xlineStep != 0) {
        return -1;
    }
    const int i = inlineOffset / inlineStep;
    const int j = xlineOffset / xlineStep;
    if(i < 0 || i >= inlineCount || j < 0 || j >= xlineCount) {
        return -1;
    }
    const int traceIndex = inlineMajor ? (i * xlineCount + j) : (j * inlineCount + i);
    return traceIndex >= 0 && traceIndex < traceCount ? traceIndex : -1;
}

bool SgyRuleLayout::ExpectedInlineXline(int traceIndex, int& inlineNo, int& xlineNo) const {
    if(!valid || traceIndex < 0 || traceIndex >= traceCount || xlineCount <= 0 || inlineCount <= 0) {
        return false;
    }
    int i = 0;
    int j = 0;
    if(inlineMajor) {
        i = traceIndex / xlineCount;
        j = traceIndex % xlineCount;
    } else {
        j = traceIndex / inlineCount;
        i = traceIndex % inlineCount;
    }
    inlineNo = firstInline + i * inlineStep;
    xlineNo = firstXline + j * xlineStep;
    return true;
}

std::string SgyRuleLayout::Describe() const {
    std::ostringstream oss;
    if(!valid) {
        oss << "rule layout not verified";
        if(!rejectionReason.empty()) {
            oss << " (" << rejectionReason << ")";
        }
        return oss.str();
    }
    oss << "rule layout sampled: inline " << firstInline << ".." << lastInline
        << " step " << inlineStep << " (" << inlineCount << "), xline " << firstXline << ".."
        << lastXline << " step " << xlineStep << " (" << xlineCount << "), "
        << (inlineMajor ? "inline-major" : "xline-major") << ", probes " << matchedProbes << "/"
        << probeCount;
    return oss.str();
}

bool ProbeSgyRuleLayout(
    const std::filesystem::path& path,
    int traceCount,
    SgyRuleLayout& outLayout,
    std::string& errorMessage,
    int probeBudget) {
    outLayout = SgyRuleLayout{};
    errorMessage.clear();

    if(traceCount < 4) {
        outLayout.rejectionReason = "too few traces to derive a layout";
        return false;
    }

    sgyio::Handle file(sgyio::OpenReadOnly(path));
    if(!file) {
        errorMessage = "segy_open failed while probing the layout.";
        return false;
    }
    if(segy_collect_metadata(file.Get(), -1, -1, 0) != SEGY_OK) {
        errorMessage = "segy_collect_metadata failed while probing the layout.";
        return false;
    }

    TraceKey first;
    TraceKey second;
    TraceKey third;
    TraceKey last;
    if(!ReadTraceKey(file.Get(), 0, first) ||
        !ReadTraceKey(file.Get(), 1, second) ||
        !ReadTraceKey(file.Get(), 2, third) ||
        !ReadTraceKey(file.Get(), traceCount - 1, last)) {
        outLayout.rejectionReason = "trace headers at the head/tail are unreadable";
        return false;
    }

    // Which axis runs fastest? Consecutive traces must differ on exactly one
    // axis for a regular layout.
    int dInline = second.inlineNo - first.inlineNo;
    int dXline = second.xlineNo - first.xlineNo;
    bool inlineMajor = false;
    if(dXline != 0 && dInline == 0) {
        inlineMajor = true;
    } else if(dInline != 0 && dXline == 0) {
        inlineMajor = false;
    } else {
        const int dInline2 = third.inlineNo - second.inlineNo;
        const int dXline2 = third.xlineNo - second.xlineNo;
        if(dXline2 != 0 && dInline2 == 0) {
            inlineMajor = true;
            dInline = dInline2;
            dXline = dXline2;
        } else if(dInline2 != 0 && dXline2 == 0) {
            inlineMajor = false;
            dInline = dInline2;
            dXline = dXline2;
        } else {
            outLayout.rejectionReason = "consecutive traces do not follow a single axis step";
            return false;
        }
    }

    const int fastestStep = inlineMajor ? dXline : dInline;
    if(fastestStep == 0) {
        outLayout.rejectionReason = "fastest axis step is zero";
        return false;
    }

    // Ranges come from real head/tail headers; the counts and the slow-axis
    // step are derived from them plus the trace count.
    const int firstInline = first.inlineNo;
    const int firstXline = first.xlineNo;
    const int lastInline = last.inlineNo;
    const int lastXline = last.xlineNo;

    SgyRuleLayout layout;
    layout.firstInline = firstInline;
    layout.lastInline = lastInline;
    layout.firstXline = firstXline;
    layout.lastXline = lastXline;
    layout.inlineMajor = inlineMajor;
    layout.traceCount = traceCount;

    if(inlineMajor) {
        const int xlineCount = std::abs(lastXline - firstXline) / std::abs(fastestStep) + 1;
        if(xlineCount <= 0 || traceCount % xlineCount != 0) {
            outLayout.rejectionReason = "trace count is not a whole number of xline runs";
            return false;
        }
        const int inlineCount = traceCount / xlineCount;
        if(inlineCount <= 0) {
            outLayout.rejectionReason = "inline count is invalid";
            return false;
        }
        layout.xlineStep = fastestStep;
        layout.xlineCount = xlineCount;
        layout.inlineCount = inlineCount;
        layout.inlineStep = inlineCount > 1 ? (lastInline - firstInline) / (inlineCount - 1) : 0;
    } else {
        const int inlineCount = std::abs(lastInline - firstInline) / std::abs(fastestStep) + 1;
        if(inlineCount <= 0 || traceCount % inlineCount != 0) {
            outLayout.rejectionReason = "trace count is not a whole number of inline runs";
            return false;
        }
        const int xlineCount = traceCount / inlineCount;
        if(xlineCount <= 0) {
            outLayout.rejectionReason = "xline count is invalid";
            return false;
        }
        layout.inlineStep = fastestStep;
        layout.inlineCount = inlineCount;
        layout.xlineCount = xlineCount;
        layout.xlineStep = xlineCount > 1 ? (lastXline - firstXline) / (xlineCount - 1) : 0;
    }

    if(layout.inlineStep == 0 || layout.xlineStep == 0 ||
        static_cast<long long>(layout.inlineCount) * static_cast<long long>(layout.xlineCount) != traceCount) {
        outLayout.rejectionReason = "derived steps are inconsistent";
        return false;
    }

    layout.valid = true;

    // Verify the candidate against deterministic probes spread over the file.
    const int probes = std::max(4, std::min(probeBudget, traceCount));
    int matched = 0;
    for(int p = 0; p < probes; ++p) {
        const int traceIndex = static_cast<int>(
            (static_cast<long long>(p) * (traceCount - 1)) / std::max(1, probes - 1));
        TraceKey key;
        if(!ReadTraceKey(file.Get(), traceIndex, key)) {
            outLayout.rejectionReason = "probe trace header is unreadable";
            return false;
        }
        int expectedInline = 0;
        int expectedXline = 0;
        if(!layout.ExpectedInlineXline(traceIndex, expectedInline, expectedXline)) {
            outLayout.rejectionReason = "candidate layout cannot predict probe positions";
            return false;
        }
        if(expectedInline != key.inlineNo || expectedXline != key.xlineNo) {
            std::ostringstream oss;
            oss << "probe " << p << " at trace " << traceIndex << " expected " << expectedInline << "/"
                << expectedXline << " but found " << key.inlineNo << "/" << key.xlineNo;
            outLayout.rejectionReason = oss.str();
            return false;
        }
        ++matched;
    }

    layout.probeCount = probes;
    layout.matchedProbes = matched;
    outLayout = layout;
    return true;
}

} // namespace seismic
