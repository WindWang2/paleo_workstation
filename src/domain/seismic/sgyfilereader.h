#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace seismic {

struct SgyFileSummary {
    std::filesystem::path path;
    std::uintmax_t fileSize = 0;
    int traceCount = 0;
    int sampleCount = 0;
    int sampleIntervalUs = 0;
    int formatCode = 0;
    int formatSizeBytes = 0;
    int endianness = -1;
    int encoding = -1;
    int firstInline = 0;
    int firstCrossline = 0;
    std::string textHeaderPreview;

    // Geometry declared by the textual header. NOT verified against real trace
    // headers: use it for a temporary frame only and label it as unverified.
    bool declaredRangeValid = false;
    int declaredInlineMin = 0;
    int declaredInlineMax = 0;
    int declaredXlineMin = 0;
    int declaredXlineMax = 0;
};

class SgyFileReader {
public:
    static bool ReadSummary(const std::filesystem::path& path,
                            SgyFileSummary& summary,
                            std::string& errorMessage);
};

std::string DescribeSgyFormat(int formatCode);
std::string DescribeSgyEndianness(int endianness);
std::string DescribeSgyEncoding(int encoding);

} // namespace seismic
