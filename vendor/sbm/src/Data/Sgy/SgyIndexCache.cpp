#include "Data/Sgy/SgyIndexCache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "Data/Sgy/SgyIo.h"

namespace seismic {
namespace {

constexpr char kMagic[8] = { 'S', 'F', '3', 'V', 'S', 'G', 'Y', 'I' };
constexpr std::uint64_t kMaxTraceRefs = 200ull * 1000ull * 1000ull;
constexpr std::uint64_t kMaxLineValues = 10ull * 1000ull * 1000ull;
constexpr std::size_t kFingerprintChunk = 32 * 1024;

std::uint64_t Fnv1a(const void* data, std::size_t size, std::uint64_t hash = 1469598103934665603ull) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for(std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void AppendU32(std::vector<unsigned char>& buffer, std::uint32_t value) {
    buffer.push_back(static_cast<unsigned char>(value & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
}

void AppendU64(std::vector<unsigned char>& buffer, std::uint64_t value) {
    for(int i = 0; i < 8; ++i) {
        buffer.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFF));
    }
}

void AppendI32(std::vector<unsigned char>& buffer, int value) {
    AppendU32(buffer, static_cast<std::uint32_t>(value));
}

void AppendU8(std::vector<unsigned char>& buffer, unsigned char value) {
    buffer.push_back(value);
}

void AppendBytes(std::vector<unsigned char>& buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
}

class Reader {
public:
    Reader(const std::vector<unsigned char>& data) : data_(data) {}

    bool ReadU32(std::uint32_t& value) {
        if(offset_ + 4 > data_.size()) {
            return false;
        }
        value = static_cast<std::uint32_t>(data_[offset_]) |
                (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8) |
                (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16) |
                (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;
        return true;
    }

    bool ReadU64(std::uint64_t& value) {
        if(offset_ + 8 > data_.size()) {
            return false;
        }
        value = 0;
        for(int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
        }
        offset_ += 8;
        return true;
    }

    bool ReadI32(int& value) {
        std::uint32_t raw = 0;
        if(!ReadU32(raw)) {
            return false;
        }
        value = static_cast<int>(raw);
        return true;
    }

    bool ReadU8(unsigned char& value) {
        if(offset_ + 1 > data_.size()) {
            return false;
        }
        value = data_[offset_++];
        return true;
    }

    bool ReadBytes(void* target, std::size_t size) {
        if(offset_ + size > data_.size()) {
            return false;
        }
        std::memcpy(target, data_.data() + offset_, size);
        offset_ += size;
        return true;
    }

    std::size_t Remaining() const { return data_.size() - offset_; }

private:
    const std::vector<unsigned char>& data_;
    std::size_t offset_ = 0;
};

std::string NormalizePathKey(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path canonical = ec ? absolute : std::filesystem::weakly_canonical(absolute, ec);
    return sgyio::ToUtf8Path(ec ? absolute : canonical);
}

std::int64_t FileTimeTicks(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if(ec) {
        return -1;
    }
    return static_cast<std::int64_t>(time.time_since_epoch().count());
}

std::uintmax_t FileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

bool WriteAll(const std::filesystem::path& path, const std::vector<unsigned char>& bytes, std::string& errorMessage) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out) {
        errorMessage = "Cannot create cache file.";
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if(!out) {
        errorMessage = "Writing the cache file failed (disk full or permission denied).";
        return false;
    }
    return true;
}

// A publish replaces one path with another; transient OS states (Windows
// sharing violations, POSIX EBUSY) are worth a bounded retry, and the failure
// must always carry the OS detail — a bare "publishing failed" left decades of
// field reports undiagnosable.
constexpr int kReplaceAttempts = 3;
constexpr int kReplaceRetryBackoffMs = 5;

bool ReplaceFileAtomically(const std::filesystem::path& from, const std::filesystem::path& to, std::string& errorMessage) {
    const std::filesystem::path targetDir = to.parent_path();
    if(!targetDir.empty()) {
        std::error_code dirError;
        std::filesystem::create_directories(targetDir, dirError);
        std::error_code existsError;
        if(!std::filesystem::exists(targetDir, existsError)) {
            errorMessage = "The cache directory is unusable (" + targetDir.u8string() +
                           ": " + dirError.message() + ").";
            return false;
        }
    }
    std::string detail;
    for(int attempt = 0; attempt < kReplaceAttempts; ++attempt) {
#ifdef _WIN32
        if(MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
        detail = "MoveFileEx error " + std::to_string(static_cast<unsigned long>(GetLastError()));
#else
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if(!ec) {
            return true;
        }
        detail = ec.message();
#endif
        std::error_code stillThere;
        if(!std::filesystem::exists(from, stillThere)) {
            break; // the source was consumed elsewhere; retrying cannot help
        }
        if(attempt + 1 < kReplaceAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReplaceRetryBackoffMs));
        }
    }
    errorMessage = "Publishing the cache file failed (" + detail + ").";
    return false;
}

} // namespace

std::filesystem::path SgyIndexCache::CacheDirectory() {
    if(const char* overrideDir = std::getenv("SEISMIC_INDEX_CACHE_DIR")) {
        if(overrideDir[0] != '\0') {
            return std::filesystem::path(overrideDir);
        }
    }
#ifdef _WIN32
    wchar_t buffer[32768] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if(length > 0 && length < std::size(buffer)) {
        return std::filesystem::path(buffer) / "paleo_workstation" / "index-cache";
    }
#endif
#ifndef _WIN32
    // paleo P11：POSIX 走每用户缓存目录（XDG），不再落共享的 /tmp——多用户机器上
    // 别人可预置/篡改索引文件。$XDG_CACHE_HOME → $HOME/.cache → tmp/paleo_workstation-<uid>。
    if(const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        if(xdg[0] == '/') {
            return std::filesystem::path(xdg) / "paleo_workstation" / "index-cache";
        }
    }
    if(const char* home = std::getenv("HOME")) {
        if(home[0] == '/') {
            return std::filesystem::path(home) / ".cache" / "paleo_workstation" / "index-cache";
        }
    }
#endif
    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
#ifndef _WIN32
    const std::string perUser = "paleo_workstation-" + std::to_string(static_cast<unsigned long>(getuid()));
#else
    const std::string perUser = "paleo_workstation";
#endif
    return (ec ? std::filesystem::path(".") : temp) / perUser / "index-cache";
}

std::filesystem::path SgyIndexCache::CachePathFor(const std::filesystem::path& sgyPath) {
    const std::string key = NormalizePathKey(sgyPath);
    const std::uint64_t hash = Fnv1a(key.data(), key.size());
    char name[32] = {};
    std::snprintf(name, sizeof(name), "segyidx_%016llx.bin", static_cast<unsigned long long>(hash));
    return CacheDirectory() / name;
}

std::filesystem::path SgyIndexCache::CompanionPathFor(const std::filesystem::path& sgyPath) {
    return sgyPath.string() + ".sgyidx";
}

std::uint64_t SgyIndexCache::FingerprintFile(const std::filesystem::path& path, std::uintmax_t fileSize) {
    std::uint64_t hash = 1469598103934665603ull;
    hash = Fnv1a(&fileSize, sizeof(fileSize), hash);

    std::ifstream in(path, std::ios::binary);
    if(!in) {
        return hash;
    }

    std::array<unsigned char, kFingerprintChunk> chunk{};
    const std::size_t headSize = static_cast<std::size_t>(
        std::min<std::uintmax_t>(fileSize, kFingerprintChunk));
    in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(headSize));
    const std::streamsize headRead = in.gcount();
    if(headRead > 0) {
        hash = Fnv1a(chunk.data(), static_cast<std::size_t>(headRead), hash);
    }

    if(fileSize > kFingerprintChunk) {
        const std::uintmax_t tailOffset = fileSize > kFingerprintChunk
            ? fileSize - kFingerprintChunk
            : 0;
        in.clear();
        in.seekg(static_cast<std::streamoff>(tailOffset), std::ios::beg);
        if(in) {
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(kFingerprintChunk));
            const std::streamsize tailRead = in.gcount();
            if(tailRead > 0) {
                hash = Fnv1a(chunk.data(), static_cast<std::size_t>(tailRead), hash);
            }
        }
    }

    return hash;
}

SgyIndexPtr SgyIndexCache::Load(const std::filesystem::path& sgyPath, std::string& reason) {
    reason.clear();
    const std::filesystem::path companionPath = CompanionPathFor(sgyPath);
    std::error_code ec;
    if(std::filesystem::exists(companionPath, ec) && !ec) {
        return LoadFromPath(companionPath, sgyPath, reason);
    }
    const std::filesystem::path cachePath = CachePathFor(sgyPath);
    return LoadFromPath(cachePath, sgyPath, reason);
}

SgyIndexPtr SgyIndexCache::LoadFromPath(const std::filesystem::path& cachePath,
                                        const std::filesystem::path& sgyPath,
                                        std::string& reason) {
    reason.clear();
    std::error_code ec;
    if(!std::filesystem::exists(cachePath, ec) || ec) {
        reason = "no cache file";
        return nullptr;
    }

    std::ifstream in(cachePath, std::ios::binary);
    if(!in) {
        reason = "cache file is unreadable";
        return nullptr;
    }
    std::vector<unsigned char> data(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if(data.size() < 32) {
        reason = "cache file is truncated";
        return nullptr;
    }

    Reader reader(data);
    char magic[8] = {};
    if(!reader.ReadBytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        reason = "cache magic mismatch";
        return nullptr;
    }
    std::uint32_t cacheVersion = 0;
    std::uint32_t algorithmVersion = 0;
    if(!reader.ReadU32(cacheVersion) || !reader.ReadU32(algorithmVersion)) {
        reason = "cache header is truncated";
        return nullptr;
    }
    if(cacheVersion != kCacheFormatVersion) {
        reason = "cache format version " + std::to_string(cacheVersion) + " is not supported";
        return nullptr;
    }
    if(algorithmVersion != kAlgorithmVersion) {
        reason = "cache was written by index algorithm version " + std::to_string(algorithmVersion);
        return nullptr;
    }

    std::uint64_t storedSize = 0;
    std::uint64_t storedFingerprint = 0;
    std::uint64_t storedMtime = 0;
    std::uint32_t pathLength = 0;
    std::string storedPath;
    if(!reader.ReadU64(storedSize) || !reader.ReadU64(storedFingerprint) || !reader.ReadU64(storedMtime) ||
        !reader.ReadU32(pathLength) || pathLength > 32768) {
        reason = "cache identity block is truncated";
        return nullptr;
    }
    storedPath.resize(pathLength);
    if(pathLength > 0 && !reader.ReadBytes(storedPath.data(), pathLength)) {
        reason = "cache path is truncated";
        return nullptr;
    }

    const std::string currentPathKey = NormalizePathKey(sgyPath);
    if(storedPath != currentPathKey) {
        reason = "cache belongs to a different path";
        return nullptr;
    }
    const std::uintmax_t currentSize = FileSize(sgyPath);
    if(currentSize == 0) {
        reason = "source file is missing";
        return nullptr;
    }
    if(storedSize != currentSize) {
        reason = "source file size changed";
        return nullptr;
    }
    if(storedMtime != static_cast<std::uint64_t>(FileTimeTicks(sgyPath))) {
        reason = "source file modification time changed";
        return nullptr;
    }
    if(storedFingerprint != FingerprintFile(sgyPath, currentSize)) {
        reason = "source file content fingerprint changed";
        return nullptr;
    }

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
    unsigned char flags = 0;
    if(!reader.ReadI32(traceCount) || !reader.ReadI32(scannedTraceCount) || !reader.ReadI32(sampleCount) ||
        !reader.ReadI32(sampleIntervalUs) || !reader.ReadI32(formatCode) || !reader.ReadI32(formatSizeBytes) ||
        !reader.ReadI32(endianness) || !reader.ReadI32(encoding) || !reader.ReadI32(inlineMin) ||
        !reader.ReadI32(inlineMax) || !reader.ReadI32(xlineMin) || !reader.ReadI32(xlineMax) ||
        !reader.ReadU8(flags)) {
        reason = "cache metadata block is truncated";
        return nullptr;
    }
    if(traceCount <= 0 || sampleCount <= 0 || formatSizeBytes <= 0) {
        reason = "cache metadata is invalid";
        return nullptr;
    }

    auto index = std::make_shared<SgyIndex>();
    index->path = std::filesystem::absolute(sgyPath);
    index->fileSize = currentSize;
    index->modifiedTimeTicks = static_cast<std::int64_t>(storedMtime);
    index->traceCount = traceCount;
    index->scannedTraceCount = scannedTraceCount;
    index->sampleCount = sampleCount;
    index->sampleIntervalUs = sampleIntervalUs;
    index->formatCode = formatCode;
    index->formatSizeBytes = formatSizeBytes;
    index->endianness = endianness;
    index->encoding = encoding;
    index->inlineMin = inlineMin;
    index->inlineMax = inlineMax;
    index->xlineMin = xlineMin;
    index->xlineMax = xlineMax;
    index->complete = (flags & 0x01u) != 0;
    index->fromTextHeader = (flags & 0x02u) != 0;
    index->ruleBased = (flags & 0x04u) != 0;

    if(index->ruleBased) {
        int firstInline = 0;
        int lastInline = 0;
        int inlineStep = 0;
        int firstXline = 0;
        int lastXline = 0;
        int xlineStep = 0;
        int inlineCount = 0;
        int xlineCount = 0;
        unsigned char major = 0;
        int ruleTraceCount = 0;
        int probeCount = 0;
        int matchedProbes = 0;
        if(!reader.ReadI32(firstInline) || !reader.ReadI32(lastInline) || !reader.ReadI32(inlineStep) ||
            !reader.ReadI32(firstXline) || !reader.ReadI32(lastXline) || !reader.ReadI32(xlineStep) ||
            !reader.ReadI32(inlineCount) || !reader.ReadI32(xlineCount) || !reader.ReadU8(major) ||
            !reader.ReadI32(ruleTraceCount) || !reader.ReadI32(probeCount) || !reader.ReadI32(matchedProbes)) {
            reason = "cache rule block is truncated";
            return nullptr;
        }
        index->rule.valid = true;
        index->rule.firstInline = firstInline;
        index->rule.lastInline = lastInline;
        index->rule.inlineStep = inlineStep;
        index->rule.firstXline = firstXline;
        index->rule.lastXline = lastXline;
        index->rule.xlineStep = xlineStep;
        index->rule.inlineCount = inlineCount;
        index->rule.xlineCount = xlineCount;
        index->rule.inlineMajor = major != 0;
        index->rule.traceCount = ruleTraceCount;
        index->rule.probeCount = probeCount;
        index->rule.matchedProbes = matchedProbes;
    }

    unsigned char coordinatePresent = 0;
    int coordinateScaleFactor = 0;
    int coordinateUnits = 0;
    std::uint64_t coordinateSampleCount = 0;
    if(!reader.ReadU8(coordinatePresent) || !reader.ReadI32(coordinateScaleFactor) ||
        !reader.ReadI32(coordinateUnits) ||
        !reader.ReadU64(coordinateSampleCount) || coordinateSampleCount > 65536) {
        reason = "cache coordinate block is invalid";
        return nullptr;
    }
    index->coordinateFieldsPresent = coordinatePresent != 0;
    index->coordinateScaleFactor = coordinateScaleFactor;
    index->coordinateUnits = coordinateUnits;
    index->coordinateSamples.resize(static_cast<std::size_t>(coordinateSampleCount));
    for(std::uint64_t i = 0; i < coordinateSampleCount; ++i) {
        SgyCoordinateSample& sample = index->coordinateSamples[static_cast<std::size_t>(i)];
        std::uint64_t rawX = 0;
        std::uint64_t rawY = 0;
        if(!reader.ReadI32(sample.traceIndex) || !reader.ReadI32(sample.inlineNo) ||
            !reader.ReadI32(sample.xlineNo) || !reader.ReadU64(rawX) || !reader.ReadU64(rawY)) {
            reason = "cache coordinate samples are truncated";
            return nullptr;
        }
        double x = 0.0;
        double y = 0.0;
        std::memcpy(&x, &rawX, sizeof(x));
        std::memcpy(&y, &rawY, sizeof(y));
        sample.x = x;
        sample.y = y;
    }

    std::uint64_t payloadOffset = 0;
    if(!reader.ReadU64(payloadOffset) || payloadOffset < 16 || payloadOffset > data.size() - 8) {
        reason = "cache payload offset is invalid";
        return nullptr;
    }

    std::uint64_t traceRefCount = 0;
    if(!reader.ReadU64(traceRefCount) || traceRefCount > kMaxTraceRefs) {
        reason = "cache trace table length is invalid";
        return nullptr;
    }
    index->traces.resize(static_cast<std::size_t>(traceRefCount));
    for(std::uint64_t i = 0; i < traceRefCount; ++i) {
        SgyTraceRef& ref = index->traces[static_cast<std::size_t>(i)];
        if(!reader.ReadI32(ref.inlineNo) || !reader.ReadI32(ref.xlineNo) || !reader.ReadI32(ref.traceIndex)) {
            reason = "cache trace table is truncated";
            return nullptr;
        }
    }

    std::uint64_t inlineValueCount = 0;
    if(!reader.ReadU64(inlineValueCount) || inlineValueCount > kMaxLineValues) {
        reason = "cache inline table length is invalid";
        return nullptr;
    }
    index->inlineValues.resize(static_cast<std::size_t>(inlineValueCount));
    for(std::uint64_t i = 0; i < inlineValueCount; ++i) {
        if(!reader.ReadI32(index->inlineValues[static_cast<std::size_t>(i)])) {
            reason = "cache inline table is truncated";
            return nullptr;
        }
    }

    std::uint64_t xlineValueCount = 0;
    if(!reader.ReadU64(xlineValueCount) || xlineValueCount > kMaxLineValues) {
        reason = "cache xline table length is invalid";
        return nullptr;
    }
    index->xlineValues.resize(static_cast<std::size_t>(xlineValueCount));
    for(std::uint64_t i = 0; i < xlineValueCount; ++i) {
        if(!reader.ReadI32(index->xlineValues[static_cast<std::size_t>(i)])) {
            reason = "cache xline table is truncated";
            return nullptr;
        }
    }

    const std::size_t payloadEnd = data.size() - 8;
    std::uint64_t storedHash = 0;
    if(!reader.ReadU64(storedHash) || reader.Remaining() != 0) {
        reason = "cache trailer is invalid";
        return nullptr;
    }
    const std::uint64_t computedHash = Fnv1a(
        data.data() + static_cast<std::size_t>(payloadOffset),
        payloadEnd - static_cast<std::size_t>(payloadOffset));
    if(computedHash != storedHash) {
        reason = "cache payload hash mismatch";
        return nullptr;
    }

    if(!index->ruleBased) {
        index->traceByInlineXline.reserve(index->traces.size());
        for(std::size_t i = 0; i < index->traces.size(); ++i) {
            const SgyTraceRef& ref = index->traces[i];
            index->traceByInlineXline[SgyIndex::MakeKey(ref.inlineNo, ref.xlineNo)] = ref.traceIndex;
        }
    }

    return index;
}

bool SgyIndexCache::Save(const SgyIndexPtr& index, std::string& errorMessage,
                         const std::filesystem::path& targetCachePath) {
    errorMessage.clear();
    if(!index || !index->FullyScanned()) {
        // Rule-based, declared-range and bounded indexes must never become a
        // production persistent index: only a full, header-verified scan is
        // allowed to publish.
        errorMessage = "Only fully scanned indexes are cached (complete=" +
                       std::string(index && index->complete ? "yes" : "no") +
                       (index && index->fromTextHeader ? ", text-header placeholder" : "") +
                       (index && index->ruleBased ? ", rule-based snapshot" : "") + ").";
        return false;
    }

    // Never publish a cache for a file that changed while it was being indexed.
    const std::uintmax_t currentSize = FileSize(index->path);
    if(currentSize == 0 || currentSize != index->fileSize) {
        errorMessage = "Source file size changed during indexing; cache not published.";
        return false;
    }
    const std::int64_t currentMtime = FileTimeTicks(index->path);
    if(index->modifiedTimeTicks != 0 && currentMtime != index->modifiedTimeTicks) {
        errorMessage = "Source file modification time changed during indexing; cache not published.";
        return false;
    }
    const std::uint64_t currentFingerprint = FingerprintFile(index->path, currentSize);

    const std::filesystem::path cachePath = targetCachePath.empty()
        ? CachePathFor(index->path)
        : targetCachePath;
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    if(ec) {
        errorMessage = "Cannot create the cache directory (permission denied?); continuing with an in-memory index.";
        return false;
    }

    std::vector<unsigned char> buffer;
    buffer.reserve(1024 + index->traces.size() * 12 + index->inlineValues.size() * 4 +
                   index->xlineValues.size() * 4);
    AppendBytes(buffer, kMagic, sizeof(kMagic));
    AppendU32(buffer, kCacheFormatVersion);
    AppendU32(buffer, kAlgorithmVersion);
    AppendU64(buffer, static_cast<std::uint64_t>(currentSize));
    AppendU64(buffer, currentFingerprint);
    AppendU64(buffer, static_cast<std::uint64_t>(currentMtime));
    const std::string pathKey = NormalizePathKey(index->path);
    AppendU32(buffer, static_cast<std::uint32_t>(pathKey.size()));
    AppendBytes(buffer, pathKey.data(), pathKey.size());
    AppendI32(buffer, index->traceCount);
    AppendI32(buffer, index->scannedTraceCount);
    AppendI32(buffer, index->sampleCount);
    AppendI32(buffer, index->sampleIntervalUs);
    AppendI32(buffer, index->formatCode);
    AppendI32(buffer, index->formatSizeBytes);
    AppendI32(buffer, index->endianness);
    AppendI32(buffer, index->encoding);
    AppendI32(buffer, index->inlineMin);
    AppendI32(buffer, index->inlineMax);
    AppendI32(buffer, index->xlineMin);
    AppendI32(buffer, index->xlineMax);
    unsigned char flags = 0;
    if(index->complete) {
        flags |= 0x01u;
    }
    if(index->fromTextHeader) {
        flags |= 0x02u;
    }
    if(index->ruleBased) {
        flags |= 0x04u;
    }
    AppendU8(buffer, flags);
    if(index->ruleBased) {
        AppendI32(buffer, index->rule.firstInline);
        AppendI32(buffer, index->rule.lastInline);
        AppendI32(buffer, index->rule.inlineStep);
        AppendI32(buffer, index->rule.firstXline);
        AppendI32(buffer, index->rule.lastXline);
        AppendI32(buffer, index->rule.xlineStep);
        AppendI32(buffer, index->rule.inlineCount);
        AppendI32(buffer, index->rule.xlineCount);
        AppendU8(buffer, index->rule.inlineMajor ? 1u : 0u);
        AppendI32(buffer, index->rule.traceCount);
        AppendI32(buffer, index->rule.probeCount);
        AppendI32(buffer, index->rule.matchedProbes);
    }

    // Coordinate samples (affine well localisation).
    AppendU8(buffer, index->coordinateFieldsPresent ? 1u : 0u);
    AppendI32(buffer, index->coordinateScaleFactor);
    AppendI32(buffer, index->coordinateUnits);
    AppendU64(buffer, static_cast<std::uint64_t>(index->coordinateSamples.size()));
    for(const SgyCoordinateSample& sample : index->coordinateSamples) {
        AppendI32(buffer, sample.traceIndex);
        AppendI32(buffer, sample.inlineNo);
        AppendI32(buffer, sample.xlineNo);
        // Bit-exact double storage (numeric casts would truncate fractions).
        std::uint64_t rawX = 0;
        std::uint64_t rawY = 0;
        std::memcpy(&rawX, &sample.x, sizeof(rawX));
        std::memcpy(&rawY, &sample.y, sizeof(rawY));
        AppendU64(buffer, rawX);
        AppendU64(buffer, rawY);
    }

    const std::size_t payloadOffsetPosition = buffer.size();
    AppendU64(buffer, 0); // patched below with the absolute payload offset
    const std::size_t payloadStart = buffer.size();
    AppendU64(buffer, static_cast<std::uint64_t>(index->traces.size()));
    for(const SgyTraceRef& ref : index->traces) {
        AppendI32(buffer, ref.inlineNo);
        AppendI32(buffer, ref.xlineNo);
        AppendI32(buffer, ref.traceIndex);
    }
    AppendU64(buffer, static_cast<std::uint64_t>(index->inlineValues.size()));
    for(int value : index->inlineValues) {
        AppendI32(buffer, value);
    }
    AppendU64(buffer, static_cast<std::uint64_t>(index->xlineValues.size()));
    for(int value : index->xlineValues) {
        AppendI32(buffer, value);
    }
    const std::uint64_t payloadHash = Fnv1a(buffer.data() + payloadStart, buffer.size() - payloadStart);
    AppendU64(buffer, payloadHash);
    const std::uint64_t payloadOffsetValue = static_cast<std::uint64_t>(payloadStart);
    for(int i = 0; i < 8; ++i) {
        buffer[payloadOffsetPosition + static_cast<std::size_t>(i)] =
            static_cast<unsigned char>((payloadOffsetValue >> (8 * i)) & 0xFF);
    }

    unsigned long processId = 0;
#ifdef _WIN32
    processId = GetCurrentProcessId();
#else
    processId = static_cast<unsigned long>(getpid());
#endif
    const std::filesystem::path tempPath = cachePath.parent_path() /
        (cachePath.filename().string() + ".tmp" + std::to_string(processId) + "_" +
         std::to_string(static_cast<unsigned long long>(currentFingerprint & 0xFFFF)));
    if(!WriteAll(tempPath, buffer, errorMessage)) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return false;
    }

    // Re-read and verify the temporary file before publishing it. The reopen
    // itself gets one retry: on Windows the freshly closed write handle can be
    // momentarily re-scanned by anti-malware filters, which used to surface as
    // "Cache verification after writing failed" even though nothing about the
    // bytes was wrong — and skipping verification would have hidden real
    // corruption, so the honest fix is a bounded reopen retry plus a distinct
    // message for the two failure shapes.
    std::vector<unsigned char> reread;
    bool reopened = false;
    std::string reopenNote;
    for(int attempt = 0; attempt < 2 && !reopened; ++attempt) {
        std::ifstream verify(tempPath, std::ios::binary);
        if(verify) {
            reread.assign((std::istreambuf_iterator<char>(verify)),
                          std::istreambuf_iterator<char>());
            if(!verify.bad()) {
                reopened = true;
                break;
            }
            reopenNote = "re-reading the temporary file failed mid-stream";
        } else {
            reopenNote = "the written temporary file cannot be reopened";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(!reopened || reread.size() != buffer.size() ||
       (buffer.empty() ? false : std::memcmp(reread.data(), buffer.data(), buffer.size()) != 0)) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        errorMessage = "Cache verification after writing failed";
        if(!reopened) {
            errorMessage += " (" + reopenNote + ")";
        } else {
            errorMessage += " (expected " + std::to_string(buffer.size()) +
                            " bytes, re-read " + std::to_string(reread.size()) + ")";
        }
        errorMessage += ".";
        return false;
    }

    if(!ReplaceFileAtomically(tempPath, cachePath, errorMessage)) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return false;
    }
    return true;
}

bool SgyIndexCache::Remove(const std::filesystem::path& sgyPath, std::string& errorMessage) {
    errorMessage.clear();
    std::error_code ec;
    const std::filesystem::path companionPath = CompanionPathFor(sgyPath);
    if(std::filesystem::exists(companionPath, ec)) {
        std::filesystem::remove(companionPath, ec);
    }
    const std::filesystem::path cachePath = CachePathFor(sgyPath);
    if(!std::filesystem::exists(cachePath, ec)) {
        return true;
    }
    std::filesystem::remove(cachePath, ec);
    if(ec) {
        errorMessage = "Cannot remove the cache file: " + ec.message();
        return false;
    }
    return true;
}

} // namespace seismic
