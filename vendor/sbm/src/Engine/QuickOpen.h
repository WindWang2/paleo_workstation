#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "Data/Sgy/SgyRuleLayout.h"
#include "Data/Sgy/SgyVolume.h"
#include "Engine/Types.h"

namespace seismic {
namespace engine {

// Stage J1: quick open / fast first real slice.
//
// Reads metadata, probes the rule grid with a bounded number of header reads,
// verifies the grid at the file boundaries and immediately reads a bounded real
// preview of the central inline. No complete index is required and no index
// cache is written or deleted. If any verification step fails the result is a
// safe fallback (ruleVerified == false) and the caller must use the normal
// index flow.
struct QuickOpenTimings {
    double metadataMs = 0.0;
    double probeMs = 0.0;
    double verifyMs = 0.0;
    double firstReadMs = 0.0;
    double decodeMs = 0.0;
    double publishMs = 0.0;
    double totalMs = 0.0;
    std::uint64_t readCalls = 0;   // header + trace reads issued
    std::uint64_t requestedBytes = 0; // readCalls * trace record bytes (requested, not OS-level)
};

struct QuickOpenResult {
    Status status;
    DatasetMetadata metadata;
    SgyRuleLayout layout;
    bool ruleVerified = false;
    std::string fallbackReason;
    std::string identity; // path|fileSize|mtimeTicks
    std::string verificationSummary;
    int probeMatched = 0;
    int probeTotal = 0;
    int selectedInline = 0;
    int selectedXline = 0;
    int columnsRead = 0;
    int columnsTotal = 0;
    SgySliceImage preview; // real amplitudes; unread columns are NaN
    QuickOpenTimings times;
};

// maxColumns bounds the real preview reads (128 matches the Viewer preview).
QuickOpenResult QuickOpenSegyPreview(const std::filesystem::path& path,
                                     int maxColumns = 128,
                                     CancelToken* cancel = nullptr);

} // namespace engine
} // namespace seismic