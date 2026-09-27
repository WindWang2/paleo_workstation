#include "domain/seismic/sgyfilereader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string>

#include <segyio/segy.h>

#include "domain/seismic/sgyindexbuilder.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace seismic {
namespace {

class SgyHandle {
public:
    explicit SgyHandle(segy_datasource* handle) : handle_(handle) {}
    ~SgyHandle() {
        if(handle_ != nullptr) {
            segy_close(handle_);
        }
    }

    SgyHandle(const SgyHandle&) = delete;
    SgyHandle& operator=(const SgyHandle&) = delete;

    segy_datasource* get() const { return handle_; }

private:
    segy_datasource* handle_ = nullptr;
};

bool CheckSegyError(int err, const char* operation, std::string& errorMessage) {
    if(err == SEGY_OK) {
        return true;
    }

    std::ostringstream oss;
    oss << operation << " failed, segyio error code = " << err;
    errorMessage = oss.str();
    return false;
}

std::string ToUtf8Path(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring widePath = path.wstring();
    const int size = WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(size > 0) {
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, result.data(), size, nullptr, nullptr);
        if(!result.empty() && result.back() == '\0') {
            result.pop_back();
        }
        return result;
    }
#endif
    return path.string();
}

std::string ToAnsiPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring widePath = path.wstring();
    const int size = WideCharToMultiByte(CP_ACP, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(size > 0) {
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_ACP, 0, widePath.c_str(), -1, result.data(), size, nullptr, nullptr);
        if(!result.empty() && result.back() == '\0') {
            result.pop_back();
        }
        return result;
    }
#endif
    return path.string();
}

struct OpenSegyResult {
    segy_datasource* handle = nullptr;
};

OpenSegyResult OpenSegyFile(const std::filesystem::path& path) {
    // segyio 2.x on Windows converts the input from UTF-8 to UTF-16 and opens
    // the file with _wfopen, so Chinese and other Unicode paths work directly.
    if(auto* file = segy_open(ToUtf8Path(path).c_str(), "rb")) {
        return { file };
    }

    // Older segyio builds call fopen() directly and expect an ANSI path.
    if(auto* file = segy_open(ToAnsiPath(path).c_str(), "rb")) {
        return { file };
    }

    // Never copy or hard-link the source file to work around path issues: the
    // test datasets are tens of gigabytes and must stay read-only in place.
    return {};
}

std::string BuildTextHeaderPreview(const std::array<char, SEGY_TEXT_HEADER_SIZE + 1>& textHeader) {
    std::string preview(textHeader.data(), textHeader.data() + SEGY_TEXT_HEADER_SIZE);
    for(char& ch : preview) {
        const auto value = static_cast<unsigned char>(ch);
        if(ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        } else if(value < 32 || value > 126) {
            ch = ' ';
        }
    }

    auto compactEnd = std::unique(preview.begin(), preview.end(), [](char a, char b) {
        return std::isspace(static_cast<unsigned char>(a)) &&
               std::isspace(static_cast<unsigned char>(b));
    });
    preview.erase(compactEnd, preview.end());

    const auto first = std::find_if_not(preview.begin(), preview.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch));
    });
    const auto last = std::find_if_not(preview.rbegin(), preview.rend(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch));
    }).base();

    if(first >= last) {
        return {};
    }

    preview = std::string(first, last);
    constexpr std::size_t maxPreviewLength = 240;
    if(preview.size() > maxPreviewLength) {
        preview.resize(maxPreviewLength);
        preview += "...";
    }
    return preview;
}

int GetFormatSizeBytes(int formatCode) {
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

} // namespace

bool SgyFileReader::ReadSummary(const std::filesystem::path& path,
                                SgyFileSummary& summary,
                                std::string& errorMessage) {
    summary = {};
    errorMessage.clear();

    if(!std::filesystem::exists(path)) {
        errorMessage = "SGY file does not exist.";
        return false;
    }

    const auto normalizedPath = std::filesystem::absolute(path);
    auto openResult = OpenSegyFile(normalizedPath);
    SgyHandle file(openResult.handle);
    if(file.get() == nullptr) {
        errorMessage = "segy_open failed. Check that the file exists, is readable, and is a SEG-Y file.";
        return false;
    }

    if(!CheckSegyError(segy_collect_metadata(file.get(), -1, -1, 0),
                       "segy_collect_metadata",
                       errorMessage)) {
        return false;
    }

    std::array<char, SEGY_BINARY_HEADER_SIZE> binaryHeader{};
    if(!CheckSegyError(segy_binheader(file.get(), binaryHeader.data()), "segy_binheader", errorMessage)) {
        return false;
    }

    int traceCount = 0;
    if(!CheckSegyError(segy_traces(file.get(), &traceCount), "segy_traces", errorMessage)) {
        return false;
    }

    int endianness = -1;
    if(!CheckSegyError(segy_endianness(file.get(), &endianness), "segy_endianness", errorMessage)) {
        return false;
    }

    int encoding = -1;
    if(!CheckSegyError(segy_encoding(file.get(), &encoding), "segy_encoding", errorMessage)) {
        return false;
    }

    int sampleIntervalUs = 0;
    segy_get_binfield_int(binaryHeader.data(), SEGY_BIN_INTERVAL, &sampleIntervalUs);

    std::array<char, SEGY_TEXT_HEADER_SIZE + 1> textHeader{};
    if(segy_read_textheader(file.get(), textHeader.data()) != SEGY_OK) {
        textHeader.fill(0);
    }

    int firstInline = 0;
    int firstCrossline = 0;
    if(traceCount > 0) {
        std::array<char, SEGY_TRACE_HEADER_SIZE> traceHeader{};
        if(segy_read_standard_traceheader(file.get(), 0, traceHeader.data()) == SEGY_OK) {
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_INLINE, &firstInline);
            segy_get_tracefield_int(traceHeader.data(), SEGY_TR_CROSSLINE, &firstCrossline);
        }
    }

    summary.path = normalizedPath;
    std::error_code sizeError;
    summary.fileSize = std::filesystem::file_size(normalizedPath, sizeError);
    if(sizeError) {
        summary.fileSize = 0;
    }
    summary.traceCount = traceCount;
    summary.sampleCount = segy_samples(binaryHeader.data());
    summary.sampleIntervalUs = sampleIntervalUs;
    summary.formatCode = segy_format(binaryHeader.data());
    summary.formatSizeBytes = GetFormatSizeBytes(summary.formatCode);
    summary.endianness = endianness;
    summary.encoding = encoding;
    summary.firstInline = firstInline;
    summary.firstCrossline = firstCrossline;
    summary.textHeaderPreview = BuildTextHeaderPreview(textHeader);

    const SgyDeclaredRanges declared = SgyIndexBuilder::ParseDeclaredRanges(
        SgyIndexBuilder::BuildAsciiTextHeader(textHeader.data(), SEGY_TEXT_HEADER_SIZE));
    summary.declaredRangeValid = declared.valid;
    summary.declaredInlineMin = declared.inlineMin;
    summary.declaredInlineMax = declared.inlineMax;
    summary.declaredXlineMin = declared.xlineMin;
    summary.declaredXlineMax = declared.xlineMax;
    return true;
}

std::string DescribeSgyFormat(int formatCode) {
    switch(formatCode) {
        case SEGY_IBM_FLOAT_4_BYTE:
            return "IBM 4-byte float";
        case SEGY_SIGNED_INTEGER_4_BYTE:
            return "4-byte signed integer";
        case SEGY_SIGNED_SHORT_2_BYTE:
            return "2-byte signed integer";
        case SEGY_FIXED_POINT_WITH_GAIN_4_BYTE:
            return "4-byte fixed point with gain";
        case SEGY_IEEE_FLOAT_4_BYTE:
            return "IEEE 4-byte float";
        case SEGY_IEEE_FLOAT_8_BYTE:
            return "IEEE 8-byte float";
        case SEGY_SIGNED_INTEGER_8_BYTE:
            return "8-byte signed integer";
        case SEGY_UNSIGNED_INTEGER_4_BYTE:
            return "4-byte unsigned integer";
        case SEGY_UNSIGNED_SHORT_2_BYTE:
            return "2-byte unsigned integer";
        case SEGY_UNSIGNED_INTEGER_8_BYTE:
            return "8-byte unsigned integer";
        default:
            return "Unknown / unsupported";
    }
}

std::string DescribeSgyEndianness(int endianness) {
    switch(endianness) {
        case SEGY_MSB:
            return "Big endian";
        case SEGY_LSB:
            return "Little endian";
        default:
            return "Unknown";
    }
}

std::string DescribeSgyEncoding(int encoding) {
    switch(encoding) {
        case SEGY_EBCDIC:
            return "EBCDIC";
        case SEGY_ASCII:
            return "ASCII";
        default:
            return "Unknown";
    }
}

} // namespace seismic
