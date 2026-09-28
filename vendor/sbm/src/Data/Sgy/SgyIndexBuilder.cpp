#include "Data/Sgy/SgyIndexBuilder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include <segyio/segy.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

int FormatSizeBytes(int formatCode) {
    switch(formatCode) {
        case SEGY_IBM_FLOAT_4_BYTE:
        case SEGY_SIGNED_INTEGER_4_BYTE:
        case SEGY_FIXED_POINT_WITH_GAIN_4_BYTE:
        case SEGY_IEEE_FLOAT_4_BYTE:
        case SEGY_UNSIGNED_INTEGER_4_BYTE:
            return 4;
        case SEGY_SIGNED_SHORT_2_BYTE:
        case SEGY_UNSIGNED_SHORT_2_BYTE:
            return 2;
        case SEGY_IEEE_FLOAT_8_BYTE:
        case SEGY_SIGNED_INTEGER_8_BYTE:
        case SEGY_UNSIGNED_INTEGER_8_BYTE:
            return 8;
        case SEGY_SIGNED_CHAR_1_BYTE:
        case SEGY_UNSIGNED_CHAR_1_BYTE:
            return 1;
        default:
            return -1;
    }
}

std::string SanitizeText(std::string text) {
    for(char& ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        if(ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        } else if(value < 32 || value > 126) {
            ch = ' ';
        }
    }
    return text;
}

bool TryBuildTraceIndexFromTextHeader(
    const std::string& textHeader,
    int traceCount,
    SgyIndex& index) {
    const SgyDeclaredRanges declared = SgyIndexBuilder::ParseDeclaredRanges(textHeader);
    if(!declared.valid) {
        return false;
    }

    const int firstInline = declared.inlineMin;
    const int lastInline = declared.inlineMax;
    const int firstXline = declared.xlineMin;
    const int lastXline = declared.xlineMax;
    const int inlineStep = firstInline <= lastInline ? 1 : -1;
    const int xlineStep = firstXline <= lastXline ? 1 : -1;
    const int inlineCount = std::abs(lastInline - firstInline) + 1;
    const int xlineCount = std::abs(lastXline - firstXline) + 1;
    if(inlineCount <= 0 || xlineCount <= 0 || inlineCount * xlineCount != traceCount) {
        return false;
    }

    index.traces.clear();
    index.traceByInlineXline.clear();
    index.traces.reserve(static_cast<std::size_t>(traceCount));
    std::set<int> inlineSet;
    std::set<int> xlineSet;

    int trace = 0;
    for(int i = 0; i < inlineCount; ++i) {
        const int inlineNo = firstInline + i * inlineStep;
        for(int x = 0; x < xlineCount; ++x) {
            const int xlineNo = firstXline + x * xlineStep;
            index.traces.push_back({ inlineNo, xlineNo, trace });
            index.traceByInlineXline[SgyIndex::MakeKey(inlineNo, xlineNo)] = trace;
            inlineSet.insert(inlineNo);
            xlineSet.insert(xlineNo);
            ++trace;
        }
    }

    index.inlineMin = std::min(firstInline, lastInline);
    index.inlineMax = std::max(firstInline, lastInline);
    index.xlineMin = std::min(firstXline, lastXline);
    index.xlineMax = std::max(firstXline, lastXline);
    index.inlineValues.assign(inlineSet.begin(), inlineSet.end());
    index.xlineValues.assign(xlineSet.begin(), xlineSet.end());
    return true;
}

} // namespace

std::string SgyIndexBuilder::BuildAsciiTextHeader(const char* rawTextHeader, int size) {
    if(rawTextHeader == nullptr || size <= 0) {
        return {};
    }
    std::string text = SanitizeText(std::string(rawTextHeader, rawTextHeader + size));
    if(text.find("First inline") != std::string::npos || text.find("First xline") != std::string::npos) {
        return text;
    }

#ifdef _WIN32
    const int wideSize = MultiByteToWideChar(500, 0, rawTextHeader, size, nullptr, 0);
    if(wideSize > 0) {
        std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
        MultiByteToWideChar(500, 0, rawTextHeader, size, wide.data(), wideSize);
        const int utf8Size = WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
        if(utf8Size > 0) {
            std::string decoded(static_cast<std::size_t>(utf8Size), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, wide.data(), wideSize, decoded.data(), utf8Size, nullptr, nullptr);
            decoded = SanitizeText(decoded);
            if(decoded.find("First inline") != std::string::npos ||
                decoded.find("First xline") != std::string::npos) {
                return decoded;
            }
        }
    }
#endif

    return text;
}

SgyDeclaredRanges SgyIndexBuilder::ParseDeclaredRanges(const std::string& asciiTextHeader) {
    SgyDeclaredRanges ranges;
    if(asciiTextHeader.empty()) {
        return ranges;
    }

    auto parseAxis = [&asciiTextHeader](const char* axisName, int& first, int& last) {
        const std::regex pattern(
            std::string("First\\s+") + axisName + R"(\s*:\s*(-?\d+)\s+Last\s+)" + axisName + R"(\s*:\s*(-?\d+))",
            std::regex_constants::icase);
        std::smatch match;
        if(!std::regex_search(asciiTextHeader, match, pattern) || match.size() < 3) {
            return false;
        }
        try {
            first = std::stoi(match[1].str());
            last = std::stoi(match[2].str());
        } catch(...) {
            return false;
        }
        return true;
    };

    int firstInline = 0;
    int lastInline = 0;
    int firstXline = 0;
    int lastXline = 0;
    if(!parseAxis("inline", firstInline, lastInline) || !parseAxis("xline", firstXline, lastXline)) {
        return ranges;
    }

    ranges.valid = true;
    ranges.inlineMin = std::min(firstInline, lastInline);
    ranges.inlineMax = std::max(firstInline, lastInline);
    ranges.xlineMin = std::min(firstXline, lastXline);
    ranges.xlineMax = std::max(firstXline, lastXline);
    return ranges;
}

SgyIndexPtr SgyIndexBuilder::BuildRuleBasedSnapshot(
    const std::filesystem::path& path,
    int traceCount,
    int sampleCount,
    int sampleIntervalUs,
    int formatCode,
    int endianness,
    int encoding,
    const SgyRuleLayout& layout) {
    auto index = std::make_shared<SgyIndex>();
    index->path = std::filesystem::absolute(path);
    std::error_code ec;
    index->fileSize = std::filesystem::file_size(index->path, ec);
    if(ec) {
        index->fileSize = 0;
    }
    index->traceCount = traceCount;
    index->scannedTraceCount = 0;
    index->sampleCount = sampleCount;
    index->sampleIntervalUs = sampleIntervalUs;
    index->formatCode = formatCode;
    index->formatSizeBytes = FormatSizeBytes(formatCode);
    index->endianness = endianness;
    index->encoding = encoding;
    index->ruleBased = layout.valid;
    index->rule = layout;
    index->complete = layout.valid;
    index->fromTextHeader = false;
    if(layout.valid) {
        index->inlineMin = std::min(layout.firstInline, layout.lastInline);
        index->inlineMax = std::max(layout.firstInline, layout.lastInline);
        index->xlineMin = std::min(layout.firstXline, layout.lastXline);
        index->xlineMax = std::max(layout.firstXline, layout.lastXline);
        index->inlineValues.reserve(static_cast<std::size_t>(layout.inlineCount));
        for(int i = 0; i < layout.inlineCount; ++i) {
            index->inlineValues.push_back(layout.firstInline + i * layout.inlineStep);
        }
        index->xlineValues.reserve(static_cast<std::size_t>(layout.xlineCount));
        for(int j = 0; j < layout.xlineCount; ++j) {
            index->xlineValues.push_back(layout.firstXline + j * layout.xlineStep);
        }
    }
    return index;
}

SgyIndexPtr SgyIndexBuilder::BuildDeclaredSnapshot(
    const std::filesystem::path& path,
    int traceCount,
    int sampleCount,
    int sampleIntervalUs,
    int formatCode,
    int endianness,
    int encoding,
    const SgyDeclaredRanges& declared) {
    auto index = std::make_shared<SgyIndex>();
    index->path = std::filesystem::absolute(path);
    std::error_code ec;
    index->fileSize = std::filesystem::file_size(index->path, ec);
    if(ec) {
        index->fileSize = 0;
    }
    index->traceCount = traceCount;
    index->scannedTraceCount = 0;
    index->sampleCount = sampleCount;
    index->sampleIntervalUs = sampleIntervalUs;
    index->formatCode = formatCode;
    index->formatSizeBytes = FormatSizeBytes(formatCode);
    index->endianness = endianness;
    index->encoding = encoding;
    if(declared.valid) {
        index->inlineMin = declared.inlineMin;
        index->inlineMax = declared.inlineMax;
        index->xlineMin = declared.xlineMin;
        index->xlineMax = declared.xlineMax;
    }
    index->complete = false;
    index->fromTextHeader = true;
    return index;
}

bool SgyIndexBuilder::Build(
    const std::filesystem::path& path,
    SgyIndexPtr& outIndex,
    std::string& errorMessage,
    const ProgressFn& progress,
    const SgyIndexBuildOptions& options) {
    outIndex.reset();
    errorMessage.clear();

    const std::filesystem::path absolutePath = std::filesystem::absolute(path);
    std::error_code existsError;
    if(!std::filesystem::exists(absolutePath, existsError) || existsError) {
        errorMessage = "SGY file does not exist.";
        return false;
    }

    auto index = std::make_shared<SgyIndex>();
    index->path = absolutePath;
    std::error_code sizeError;
    index->fileSize = std::filesystem::file_size(absolutePath, sizeError);
    if(sizeError) {
        index->fileSize = 0;
    }
    std::error_code timeError;
    const auto writeTime = std::filesystem::last_write_time(absolutePath, timeError);
    if(!timeError) {
        index->modifiedTimeTicks = static_cast<std::int64_t>(writeTime.time_since_epoch().count());
    }

    sgyio::Handle file(sgyio::OpenReadOnly(absolutePath));
    if(!file) {
        errorMessage = "segy_open failed for SGY volume. Check that the file exists and is readable.";
        return false;
    }
    if(!Check(segy_collect_metadata(file.Get(), -1, -1, 0), "segy_collect_metadata", errorMessage)) {
        return false;
    }

    std::array<char, SEGY_BINARY_HEADER_SIZE> binaryHeader{};
    if(!Check(segy_binheader(file.Get(), binaryHeader.data()), "segy_binheader", errorMessage)) {
        return false;
    }
    int traceCount = 0;
    if(!Check(segy_traces(file.Get(), &traceCount), "segy_traces", errorMessage)) {
        return false;
    }

    index->traceCount = traceCount;
    index->sampleCount = segy_samples(binaryHeader.data());
    index->formatCode = segy_format(binaryHeader.data());
    index->formatSizeBytes = FormatSizeBytes(index->formatCode);
    int sampleIntervalUs = 0;
    segy_get_binfield_int(binaryHeader.data(), SEGY_BIN_INTERVAL, &sampleIntervalUs);
    index->sampleIntervalUs = sampleIntervalUs;
    int endianness = -1;
    if(segy_endianness(file.Get(), &endianness) == SEGY_OK) {
        index->endianness = endianness;
    }
    int encoding = -1;
    if(segy_encoding(file.Get(), &encoding) == SEGY_OK) {
        index->encoding = encoding;
    }

    if(traceCount <= 0 || index->sampleCount <= 0 || index->formatSizeBytes <= 0) {
        errorMessage = "Invalid SGY metadata.";
        return false;
    }

    index->traces.reserve(static_cast<std::size_t>(traceCount));
    index->traceByInlineXline.reserve(static_cast<std::size_t>(traceCount));
    std::set<int> inlineSet;
    std::set<int> xlineSet;

    index->inlineMin = index->xlineMin = std::numeric_limits<int>::max();
    index->inlineMax = index->xlineMax = std::numeric_limits<int>::min();

    const int progressInterval = std::max(1, options.progressInterval);
    const int coordinateSampleInterval = std::max(1, traceCount / 2048);
    std::array<char, SEGY_TRACE_HEADER_SIZE> traceHeader{};
    for(int trace = 0; trace < traceCount; ++trace) {
        if(progress && trace % progressInterval == 0 && !progress(trace, traceCount)) {
            errorMessage = "SGY index scan cancelled by caller.";
            return false;
        }
        if(segy_read_standard_traceheader(file.Get(), trace, traceHeader.data()) != SEGY_OK) {
            continue;
        }

        // P5: paleo @8/@20 fallback keeps the production convention indexable.
        const sgyio::SgyTraceKeyWords keyWords = sgyio::ReadTraceKeyWords(traceHeader.data());
        const int inlineNo = keyWords.inlineNo;
        const int xlineNo = keyWords.xlineNo;
        if(inlineNo == 0 && xlineNo == 0) {
            continue;
        }

        index->traces.push_back({ inlineNo, xlineNo, trace });
        index->traceByInlineXline[SgyIndex::MakeKey(inlineNo, xlineNo)] = trace;
        inlineSet.insert(inlineNo);
        xlineSet.insert(xlineNo);

        if(trace % coordinateSampleInterval == 0 &&
            index->coordinateSamples.size() < 4096) {
            int cdpX = 0;
            int cdpY = 0;
            int srcX = 0;
            int srcY = 0;
            int coordUnits = 0;
            int sourceGroupScalar = 0;
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_CDP_X, &cdpX);
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_CDP_Y, &cdpY);
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_SOURCE_X, &srcX);
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_SOURCE_Y, &srcY);
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_SOURCE_GROUP_SCALAR, &sourceGroupScalar);
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_COORD_UNITS, &coordUnits);
            const int rawX = cdpX != 0 ? cdpX : srcX;
            const int rawY = cdpY != 0 ? cdpY : srcY;
            if(rawX != 0 || rawY != 0) {
                // SOURCE_GROUP_SCALAR is the multiplier: > 0 multiply,
                // < 0 divide by |value|, 0 means 1. COORD_UNITS is a separate
                // interpretation code and must not scale anything.
                const double scale = sourceGroupScalar < 0
                    ? 1.0 / static_cast<double>(-sourceGroupScalar)
                    : static_cast<double>(sourceGroupScalar > 0 ? sourceGroupScalar : 1);
                SgyCoordinateSample sample;
                sample.traceIndex = trace;
                sample.inlineNo = inlineNo;
                sample.xlineNo = xlineNo;
                sample.x = static_cast<double>(rawX) * scale;
                sample.y = static_cast<double>(rawY) * scale;
                index->coordinateSamples.push_back(sample);
                if(sourceGroupScalar != 0) {
                    index->coordinateScaleFactor = sourceGroupScalar;
                }
                if(coordUnits != 0) {
                    index->coordinateUnits = coordUnits;
                }
            }
        }
        index->inlineMin = std::min(index->inlineMin, inlineNo);
        index->inlineMax = std::max(index->inlineMax, inlineNo);
        index->xlineMin = std::min(index->xlineMin, xlineNo);
        index->xlineMax = std::max(index->xlineMax, xlineNo);
        index->scannedTraceCount = trace + 1;
    }

    if(progress && !progress(traceCount, traceCount)) {
        errorMessage = "SGY index scan cancelled by caller.";
        return false;
    }
    index->scannedTraceCount = traceCount;

    if(index->traces.empty()) {
        if(!options.allowTextHeaderFallback) {
            errorMessage = "No valid inline/crossline trace headers found.";
            return false;
        }
        std::array<char, SEGY_TEXT_HEADER_SIZE + 1> textHeader{};
        if(segy_read_textheader(file.Get(), textHeader.data()) != SEGY_OK ||
            !TryBuildTraceIndexFromTextHeader(
                BuildAsciiTextHeader(textHeader.data(), SEGY_TEXT_HEADER_SIZE),
                traceCount,
                *index)) {
            errorMessage = "No valid inline/crossline trace headers found.";
            return false;
        }
        index->fromTextHeader = true;
    } else {
        index->inlineValues.assign(inlineSet.begin(), inlineSet.end());
        index->xlineValues.assign(xlineSet.begin(), xlineSet.end());
    }

    index->coordinateFieldsPresent = index->coordinateSamples.size() >= 6;
    index->complete = true;
    outIndex = std::move(index);
    return true;
}

} // namespace seismic
