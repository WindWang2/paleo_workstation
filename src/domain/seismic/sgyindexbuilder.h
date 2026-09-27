#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

#include "domain/seismic/sgyindex.h"

namespace seismic {

struct SgyDeclaredRanges {
    bool valid = false;
    int inlineMin = 0;
    int inlineMax = 0;
    int xlineMin = 0;
    int xlineMax = 0;
};

struct SgyIndexBuildOptions {
    bool allowTextHeaderFallback = true;
    int progressInterval = 8192;
};

// Builds an immutable SgyIndex by scanning real trace headers. The scan can be
// cancelled through the progress callback and is safe to run on a worker
// thread: it never touches UI state and opens its own read-only handle.
class SgyIndexBuilder {
public:
    using ProgressFn = std::function<bool(int processed, int total)>;

    static bool Build(
        const std::filesystem::path& path,
        SgyIndexPtr& outIndex,
        std::string& errorMessage,
        const ProgressFn& progress = {},
        const SgyIndexBuildOptions& options = {});

    // Textual header helpers, also used by SgyFileReader for the fast summary.
    static std::string BuildAsciiTextHeader(const char* rawTextHeader, int size);
    static SgyDeclaredRanges ParseDeclaredRanges(const std::string& asciiTextHeader);

    // Sampled rule layout. complete=true (browsable) but ruleBased=true: every
    // located trace is re-validated against its header before samples are used.
    static SgyIndexPtr BuildRuleBasedSnapshot(
        const std::filesystem::path& path,
        int traceCount,
        int sampleCount,
        int sampleIntervalUs,
        int formatCode,
        int endianness,
        int encoding,
        const SgyRuleLayout& layout);

    // Temporary, unverified geometry from the textual header. complete=false and
    // fromTextHeader=true: usable for a frame/parameter display only.
    static SgyIndexPtr BuildDeclaredSnapshot(
        const std::filesystem::path& path,
        int traceCount,
        int sampleCount,
        int sampleIntervalUs,
        int formatCode,
        int endianness,
        int encoding,
        const SgyDeclaredRanges& declared);
};

} // namespace seismic
