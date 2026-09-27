#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "domain/seismic/sgyindex.h"
#include "domain/seismic/sgyio.h"

namespace seismic {

// One read-only SEG-Y handle plus its sample conversion.
//
// Rules:
//   * never shared between threads (segyio handles are not thread safe),
//   * opened once per task instead of once per trace,
//   * every rule-located trace is re-validated against its real header before
//     its samples are used.
class SgyReadSession {
public:
    SgyReadSession() = default;
    ~SgyReadSession();

    SgyReadSession(const SgyReadSession&) = delete;
    SgyReadSession& operator=(const SgyReadSession&) = delete;

    bool Open(const SgyIndexPtr& index, std::string& errorMessage);
    bool IsOpen() const { return handle_.Get() != nullptr; }
    int SampleCount() const { return index_ ? index_->sampleCount : 0; }

    bool ReadTrace(int traceIndex, std::vector<float>& samples, std::string& errorMessage);

private:
    bool ValidateRuleTrace(int traceIndex, std::string& errorMessage) const;

    SgyIndexPtr index_;
    sgyio::Handle handle_;
};

} // namespace seismic
