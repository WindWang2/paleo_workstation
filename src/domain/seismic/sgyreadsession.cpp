#include "domain/seismic/sgyreadsession.h"

#include <array>
#include <cstdint>
#include <sstream>

#include <segyio/segy.h>

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
    int actualInline = 0;
    int actualXline = 0;
    segy_get_tracefield_int(header.data(), SEGY_TR_INLINE, &actualInline);
    segy_get_tracefield_int(header.data(), SEGY_TR_CROSSLINE, &actualXline);
    if(actualInline != expectedInline || actualXline != expectedXline) {
        std::ostringstream oss;
        oss << "rule layout validation failed at trace " << traceIndex << ": expected " << expectedInline << "/"
            << expectedXline << " but found " << actualInline << "/" << actualXline
            << ". Use the full index instead.";
        errorMessage = oss.str();
        return false;
    }
    return true;
}

bool SgyReadSession::ReadTrace(int traceIndex, std::vector<float>& samples, std::string& errorMessage) {
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
        return Check(segy_to_native(formatCode, sampleCount, samples.data()), "segy_to_native", errorMessage);
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
