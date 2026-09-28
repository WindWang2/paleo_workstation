// 层：数据
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "domain/seismic/axisdescriptor.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/seismic/sgyrulelayout.h"

namespace seismic {

struct SgyTraceRef {
    int inlineNo = 0;
    int xlineNo = 0;
    int traceIndex = -1;
};

// Sampled trace coordinates used to fit the inline/xline -> X/Y mapping.
// Only a bounded number of traces is sampled; this is enough for an affine fit
// but it is NOT a per-trace coordinate table.
struct SgyCoordinateSample {
    int traceIndex = 0;
    int inlineNo = 0;
    int xlineNo = 0;
    double x = 0.0;
    double y = 0.0;
};

// Immutable geometry index of one SEG-Y file.
//
// The index is published as std::shared_ptr<const SgyIndex> and shared between
// the data source entry, every view session and the active volume, so a large
// survey is indexed once instead of once per copy.
//
// complete == false means the index is a partial / declared snapshot:
//   * fromTextHeader == true : geometry was parsed from the textual header and
//     has NOT been verified against real trace headers yet.
// Such an index may be used for a temporary frame only; slice extraction must
// not treat missing traces as zero-amplitude data.
struct SgyIndex {
    std::filesystem::path path;
    std::uintmax_t fileSize = 0;
    std::int64_t modifiedTimeTicks = 0;

    int traceCount = 0;
    int scannedTraceCount = 0;
    int sampleCount = 0;
    int sampleIntervalUs = 0;
    int formatCode = 0;
    int formatSizeBytes = 0;
    int endianness = -1;
    int encoding = -1;

    int inlineMin = 0;
    int inlineMax = 0;
    int xlineMin = 0;
    int xlineMax = 0;

    std::vector<SgyTraceRef> traces;
    std::unordered_map<std::uint64_t, int> traceByInlineXline;
    std::vector<int> inlineValues;
    std::vector<int> xlineValues;

    bool complete = false;
    bool fromTextHeader = false;

    // Sampled rule layout. ruleBased == true means the geometry was derived
    // from a bounded number of real headers and validated with probes, NOT by
    // scanning every trace. Every trace located through the rule is
    // re-validated against its real header before its samples are used.
    bool ruleBased = false;
    SgyRuleLayout rule;

    // Trace coordinates (CDP_X/CDP_Y preferred, SOURCE_X/Y fallback) sampled
    // during the scan.
    //
    // coordinateScaleFactor is the raw SOURCE_GROUP_SCALAR (trace header bytes
    // 71-72): > 0 multiplies, < 0 divides by |value|, 0 means 1.
    // coordinateUnits is the separate COORD_UNITS code (bytes 89-90, 1=length,
    // 2=seconds, ...) and is NOT a multiplier.
    bool coordinateFieldsPresent = false;
    int coordinateScaleFactor = 0;
    int coordinateUnits = 0;
    std::vector<SgyCoordinateSample> coordinateSamples;

    bool FullyScanned() const { return complete && !fromTextHeader && !ruleBased; }

    static std::uint64_t MakeKey(int inlineNo, int xlineNo);

    int FindTraceIndex(int inlineNo, int xlineNo) const;

    // Axis models derived from the index: a verified rule grid becomes a uniform
    // origin/step/count axis (a step-2 survey keeps step 2), anything else uses
    // the explicit sorted line values.
    AxisDescriptor InlineAxis() const;
    AxisDescriptor XlineAxis() const;
    int FindNearestInlineValue(float inlineNo) const;
    int FindNearestXlineValue(float xlineNo) const;
    int InlineCount() const { return static_cast<int>(inlineValues.size()); }
    int XlineCount() const { return static_cast<int>(xlineValues.size()); }

    // Rough memory footprint of the index structures in bytes.
    std::size_t MemoryBytes() const;
};

using SgyIndexPtr = std::shared_ptr<const SgyIndex>;

} // namespace seismic
