#include "Data/Sgy/SgyReadSession.h"

#include <array>
#include <QDebug>
#include "Data/Sgy/SgySampleSanitizer.h"
#include <cstdint>
#include <sstream>

#include <segyio/segy.h>

#include "Data/Sgy/SgyIo.h"

namespace seismic {
namespace {

bool Check(int err, const char* op, std::string& errorMessage) {
    if(err == SEGY_OK) {
        return true;
    }
    std::ostringstream oss;
    oss << op << " failed, segyio error code = " << err;
    errorMessage = oss.str();
    return false;
}

} // namespace

SgyReadSession::~SgyReadSession() = default;

bool SgyReadSession::Open(const SgyIndexPtr& index, std::string& errorMessage) {
    lastSanitizedSamples_ = 0;
    sanitizedSampleReads_ = 0;
    index_.reset();
    handle_.Reset();
    errorMessage.clear();
    if(!index) {
        errorMessage = "SGY index is missing.";
        return false;
    }

    sgyio::Handle handle(sgyio::OpenReadOnly(index->path));
    if(!handle) {
        errorMessage = "segy_open failed while opening a read session.";
        return false;
    }
    if(!Check(segy_collect_metadata(handle.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    index_ = index;
    handle_ = std::move(handle);
    return true;
}

bool SgyReadSession::ValidateRuleTrace(int traceIndex, std::string& errorMessage) const {
    if(!index_ || !index_->ruleBased) {
        return true;
    }
    int expectedInline = 0;
    int expectedXline = 0;
    if(!index_->rule.ExpectedInlineXline(traceIndex, expectedInline, expectedXline)) {
        errorMessage = "rule layout validation failed: trace index out of the sampled layout";
        return false;
    }
    std::array<char, SEGY_TRACE_HEADER_SIZE> header{};
    if(segy_read_standard_traceheader(handle_.Get(), traceIndex, header.data()) != SEGY_OK) {
        errorMessage = "rule layout validation failed: trace header is unreadable";
        return false;
    }
    // P5: same trace-key words (incl. the paleo @8/@20 fallback) the layout was
    // probed with, so validation never contradicts the probe.
    const sgyio::SgyTraceKeyWords words = sgyio::ReadTraceKeyWords(header.data());
    if(words.inlineNo != expectedInline || words.xlineNo != expectedXline) {
        std::ostringstream oss;
        oss << "rule layout validation failed at trace " << traceIndex << ": expected " << expectedInline << "/"
            << expectedXline << " but found " << words.inlineNo << "/" << words.xlineNo
            << ". Use the full index instead.";
        errorMessage = oss.str();
        return false;
    }
    return true;
}

bool SgyReadSession::ReadTrace(int traceIndex, std::vector<float>& samples, std::string& errorMessage) {
    lastSanitizedSamples_ = 0;
    errorMessage.clear();
    if(!IsOpen() || !index_) {
        errorMessage = "Read session is not open.";
        return false;
    }
    if(traceIndex < 0 || traceIndex >= index_->traceCount) {
        errorMessage = "Trace index is out of range.";
        return false;
    }
    if(!ValidateRuleTrace(traceIndex, errorMessage)) {
        return false;
    }

    const int sampleCount = index_->sampleCount;
    const int formatCode = index_->formatCode;
    const int formatSizeBytes = index_->formatSizeBytes;
    samples.assign(static_cast<std::size_t>(sampleCount), 0.0f);

    if(formatSizeBytes == 4 && (formatCode == SEGY_IEEE_FLOAT_4_BYTE || formatCode == SEGY_IBM_FLOAT_4_BYTE)) {
        if(!Check(segy_readtrace(handle_.Get(), traceIndex, samples.data()), "segy_readtrace", errorMessage)) {
            return false;
        }
        if(!Check(segy_to_native(formatCode, sampleCount, samples.data()), "segy_to_native", errorMessage)) {
            return false;
        }
        lastSanitizedSamples_ = SanitizeSgySamples(samples.data(), samples.size());
        sanitizedSampleReads_ += lastSanitizedSamples_;
        if(lastSanitizedSamples_ > 0) {
            qWarning().nospace() << "PALEO-SEGY-SANITIZED trace=" << traceIndex
                                 << " samples=" << lastSanitizedSamples_;
        }
        return true;
    }

    if(formatSizeBytes == 2) {
        std::vector<std::int16_t> raw(static_cast<std::size_t>(sampleCount));
        if(!Check(segy_readtrace(handle_.Get(), traceIndex, raw.data()), "segy_readtrace", errorMessage)) {
            return false;
        }
        if(!Check(segy_to_native(formatCode, sampleCount, raw.data()), "segy_to_native", errorMessage)) {
            return false;
        }
        for(int i = 0; i < sampleCount; ++i) {
            samples[static_cast<std::size_t>(i)] = static_cast<float>(raw[static_cast<std::size_t>(i)]);
        }
        return true;
    }

    errorMessage = "Unsupported SGY sample format.";
    return false;
}

} // namespace seismic
