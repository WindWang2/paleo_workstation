#include "Data/Sgy/SgySequentialScan.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <tuple>
#include <system_error>

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

#include "Data/Sgy/SgyFileReader.h"
#include "Data/Sgy/SgyIo.h"

namespace seismic {
namespace {

constexpr std::uint64_t kCheckpointMagic = 0x3143534B56334653ull; // "SF3VKSC1"
// v2: the record pairs and the coordinate samples live in two separate
// append-only partial files with independent byte counts and CRCs, so the
// interleaved write order of v1 can never corrupt a resume.
constexpr std::uint32_t kCheckpointVersion = 2;
constexpr std::uint32_t kCheckpointAlgorithmVersion = 2;

double MsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1000.0;
}

std::uint64_t Fnv1a(const void* data, std::size_t size, std::uint64_t hash) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for(std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

struct CheckpointState {
    std::uint64_t resumeRecord = 0;
    std::uint64_t resumeByte = 0;
    std::uint64_t pairRecords = 0;
    std::uint64_t pairBytes = 0;
    std::uint64_t pairCrc = 1099511628211ull;
    std::uint64_t coordCount = 0;
    std::uint64_t coordBytes = 0;
    std::uint64_t coordCrc = 1099511628211ull;
    bool valid = false;
};

std::filesystem::path PairPathFor(const std::filesystem::path& checkpointPath) {
    std::filesystem::path path = checkpointPath;
    path += L".pairs";
    return path;
}

std::filesystem::path CoordPathFor(const std::filesystem::path& checkpointPath) {
    std::filesystem::path path = checkpointPath;
    path += L".coords";
    return path;
}

// Legacy v1 partial (interleaved pairs and coordinates); only ever removed,
// never parsed, because the interleaving cannot be recovered reliably.
std::filesystem::path LegacyPartialPathFor(const std::filesystem::path& checkpointPath) {
    std::filesystem::path partial = checkpointPath;
    partial += L".partial";
    return partial;
}

bool ReplaceFileAtomically(const std::filesystem::path& tmpPath,
                           const std::filesystem::path& targetPath,
                           std::string& errorMessage) {
#ifdef _WIN32
    if(!MoveFileExW(tmpPath.c_str(), targetPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errorMessage = "MoveFileExW failed while publishing a scan checkpoint.";
        return false;
    }
    return true;
#else
    std::error_code ec;
    std::filesystem::rename(tmpPath, targetPath, ec);
    if(ec) {
        errorMessage = "rename failed while publishing a scan checkpoint.";
        return false;
    }
    return true;
#endif
}

void WriteU32(std::vector<unsigned char>& out, std::uint32_t value) {
    for(int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFF));
    }
}

void WriteU64(std::vector<unsigned char>& out, std::uint64_t value) {
    for(int i = 0; i < 8; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFF));
    }
}

bool ReadU32(const std::vector<unsigned char>& data, std::size_t& offset, std::uint32_t& value) {
    if(offset + 4 > data.size()) {
        return false;
    }
    value = 0;
    for(int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
    }
    offset += 4;
    return true;
}

bool ReadU64(const std::vector<unsigned char>& data, std::size_t& offset, std::uint64_t& value) {
    if(offset + 8 > data.size()) {
        return false;
    }
    value = 0;
    for(int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
    }
    offset += 8;
    return true;
}

bool SaveCheckpoint(const std::filesystem::path& checkpointPath,
                    const std::filesystem::path& sourcePath,
                    std::uint64_t sourceSize,
                    std::int64_t mtimeTicks,
                    std::uint64_t bytesPerTrace,
                    std::uint64_t dataStart,
                    const CheckpointState& state,
                    std::string& errorMessage) {
    if(checkpointPath.empty()) {
        return true;
    }
    const std::string pathText = sourcePath.u8string();
    std::vector<unsigned char> payload;
    payload.reserve(96 + pathText.size());
    WriteU64(payload, kCheckpointMagic);
    WriteU32(payload, kCheckpointVersion);
    WriteU32(payload, kCheckpointAlgorithmVersion);
    WriteU64(payload, sourceSize);
    WriteU64(payload, static_cast<std::uint64_t>(mtimeTicks));
    WriteU32(payload, static_cast<std::uint32_t>(pathText.size()));
    payload.insert(payload.end(), pathText.begin(), pathText.end());
    WriteU64(payload, bytesPerTrace);
    WriteU64(payload, dataStart);
    WriteU64(payload, state.resumeRecord);
    WriteU64(payload, state.resumeByte);
    WriteU64(payload, state.pairRecords);
    WriteU64(payload, state.pairBytes);
    WriteU64(payload, state.pairCrc);
    WriteU64(payload, state.coordCount);
    WriteU64(payload, state.coordBytes);
    WriteU64(payload, state.coordCrc);

    const std::filesystem::path tmpPath = checkpointPath.wstring() + L".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if(!out) {
            errorMessage = "cannot write a scan checkpoint.";
            return false;
        }
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        out.flush();
        if(!out) {
            errorMessage = "cannot flush a scan checkpoint.";
            return false;
        }
    }
    return ReplaceFileAtomically(tmpPath, checkpointPath, errorMessage);
}

bool LoadCheckpoint(const std::filesystem::path& checkpointPath,
                    const std::filesystem::path& sourcePath,
                    std::uint64_t sourceSize,
                    std::int64_t mtimeTicks,
                    std::uint64_t bytesPerTrace,
                    std::uint64_t dataStart,
                    CheckpointState& outState,
                    std::string& reason) {
    outState = CheckpointState{};
    if(checkpointPath.empty()) {
        reason = "no checkpoint configured";
        return false;
    }
    std::ifstream in(checkpointPath, std::ios::binary);
    if(!in) {
        reason = "no checkpoint present";
        return false;
    }
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    std::size_t offset = 0;
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t algorithmVersion = 0;
    std::uint64_t storedSize = 0;
    std::uint64_t storedMtime = 0;
    std::uint32_t pathLen = 0;
    std::uint64_t storedBytesPerTrace = 0;
    std::uint64_t storedDataStart = 0;
    if(!ReadU64(data, offset, magic) || !ReadU32(data, offset, version) ||
       !ReadU32(data, offset, algorithmVersion) || !ReadU64(data, offset, storedSize) ||
       !ReadU64(data, offset, storedMtime) || !ReadU32(data, offset, pathLen)) {
        reason = "checkpoint is truncated";
        return false;
    }
    if(magic != kCheckpointMagic || version != kCheckpointVersion) {
        reason = "checkpoint format is not supported";
        return false;
    }
    if(algorithmVersion != kCheckpointAlgorithmVersion) {
        reason = "checkpoint algorithm version differs";
        return false;
    }
    if(offset + pathLen > data.size()) {
        reason = "checkpoint path is truncated";
        return false;
    }
    const std::string storedPath(reinterpret_cast<const char*>(data.data() + offset), pathLen);
    offset += pathLen;
    if(storedPath != sourcePath.u8string()) {
        reason = "checkpoint source path differs";
        return false;
    }
    if(storedSize != sourceSize || storedMtime != static_cast<std::uint64_t>(mtimeTicks)) {
        reason = "checkpoint source identity differs";
        return false;
    }
    if(!ReadU64(data, offset, storedBytesPerTrace) || !ReadU64(data, offset, storedDataStart)) {
        reason = "checkpoint layout is truncated";
        return false;
    }
    if(storedBytesPerTrace != bytesPerTrace || storedDataStart != dataStart) {
        reason = "checkpoint trace layout differs";
        return false;
    }
    if(!ReadU64(data, offset, outState.resumeRecord) || !ReadU64(data, offset, outState.resumeByte) ||
       !ReadU64(data, offset, outState.pairRecords) || !ReadU64(data, offset, outState.pairBytes) ||
       !ReadU64(data, offset, outState.pairCrc) || !ReadU64(data, offset, outState.coordCount) ||
       !ReadU64(data, offset, outState.coordBytes) || !ReadU64(data, offset, outState.coordCrc)) {
        reason = "checkpoint payload is truncated";
        return false;
    }
    if(outState.resumeRecord != outState.pairRecords ||
       outState.resumeByte != outState.pairRecords * bytesPerTrace) {
        reason = "checkpoint record counters disagree";
        return false;
    }
    if(outState.pairBytes != outState.pairRecords * 8 ||
       outState.coordBytes != outState.coordCount * 28) {
        reason = "checkpoint partial byte counts disagree with their record counts";
        return false;
    }
    outState.valid = true;
    reason.clear();
    return true;
}

class WindowReader {
public:
    ~WindowReader() { Close(); }

    bool Open(const std::filesystem::path& path, int queueDepth, std::string& errorMessage) {
        queueDepth_ = std::max(1, queueDepth);
#ifdef _WIN32
        DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;
        if(queueDepth_ > 1) {
            flags |= FILE_FLAG_OVERLAPPED;
        }
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, flags, nullptr);
        if(handle_ == INVALID_HANDLE_VALUE) {
            errorMessage = "CreateFileW failed for the SEG-Y source.";
            return false;
        }
        if(queueDepth_ > 1) {
            overlapped_.assign(static_cast<std::size_t>(queueDepth_), OVERLAPPED{});
            buffers_.resize(static_cast<std::size_t>(queueDepth_));
            events_.resize(static_cast<std::size_t>(queueDepth_));
            for(int i = 0; i < queueDepth_; ++i) {
                events_[static_cast<std::size_t>(i)] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if(events_[static_cast<std::size_t>(i)] == nullptr) {
                    errorMessage = "CreateEventW failed for the read queue.";
                    return false;
                }
            }
            for(std::vector<char>& buffer : buffers_) {
                buffer.resize(1);
            }
        }
        return true;
#else
        in_.open(path, std::ios::binary);
        if(!in_) {
            errorMessage = "cannot open the SEG-Y source.";
            return false;
        }
        return true;
#endif
    }

    void Close() {
#ifdef _WIN32
        for(HANDLE event : events_) {
            if(event != nullptr) {
                CloseHandle(event);
            }
        }
        events_.clear();
        if(handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if(in_.is_open()) {
            in_.close();
        }
#endif
    }

    int QueueDepth() const { return queueDepth_; }

#ifdef _WIN32
    HANDLE Handle() const { return handle_; }
    std::vector<std::vector<char>>& Buffers() { return buffers_; }
    std::vector<OVERLAPPED>& Overlapped() { return overlapped_; }
    std::vector<HANDLE>& Events() { return events_; }
#else
    std::istream& Stream() { return in_; }
#endif

private:
    int queueDepth_ = 1;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::vector<std::vector<char>> buffers_;
    std::vector<OVERLAPPED> overlapped_;
    std::vector<HANDLE> events_;
#else
    std::ifstream in_;
#endif
};

#ifdef _WIN32
bool ReadWindowSync(HANDLE handle, std::uint64_t offset, char* buffer, std::size_t bytes,
                    std::size_t& outRead, std::string& errorMessage) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if(!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        errorMessage = "SetFilePointerEx failed during a sequential window read.";
        return false;
    }
    DWORD read = 0;
    if(!ReadFile(handle, buffer, static_cast<DWORD>(bytes), &read, nullptr)) {
        errorMessage = "ReadFile failed during a sequential window read.";
        return false;
    }
    outRead = read;
    return true;
}
#else
bool ReadWindowSync(std::istream& in, std::uint64_t offset, char* buffer, std::size_t bytes,
                    std::size_t& outRead, std::string& errorMessage) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if(!in) {
        errorMessage = "seek failed during a sequential window read.";
        return false;
    }
    in.read(buffer, static_cast<std::streamsize>(bytes));
    outRead = static_cast<std::size_t>(in.gcount());
    if(in.bad()) {
        errorMessage = "read failed during a sequential window read.";
        return false;
    }
    return true;
}
#endif

} // namespace

bool ScanSegySequentially(const std::filesystem::path& path,
                          const SgyScanOptions& options,
                          const SgyRecordVisitor& visitor,
                          SgyScanResult& outResult) {
    outResult = SgyScanResult{};
    const auto wallStart = std::chrono::steady_clock::now();

    const std::filesystem::path absolutePath = std::filesystem::absolute(path);
    std::error_code error;
    if(!std::filesystem::exists(absolutePath, error) || error) {
        outResult.message = "SGY file does not exist.";
        return false;
    }

    SgyFileSummary summary;
    std::string summaryError;
    if(!SgyFileReader::ReadSummary(absolutePath, summary, summaryError)) {
        outResult.message = summaryError.empty() ? "cannot read the SEG-Y metadata." : summaryError;
        return false;
    }
    if(summary.traceCount <= 0 || summary.sampleCount <= 0 || summary.formatSizeBytes <= 0) {
        outResult.message = "Invalid SGY metadata.";
        return false;
    }

    std::int64_t mtimeTicks = 0;
    std::error_code timeError;
    const auto writeTime = std::filesystem::last_write_time(absolutePath, timeError);
    if(!timeError) {
        mtimeTicks = static_cast<std::int64_t>(writeTime.time_since_epoch().count());
    }
    const std::uint64_t sourceSize = summary.fileSize;
    const std::uint64_t dataStart = SEGY_TEXT_HEADER_SIZE + SEGY_BINARY_HEADER_SIZE;
    const std::uint64_t bytesPerTrace =
        static_cast<std::uint64_t>(SEGY_TRACE_HEADER_SIZE) +
        static_cast<std::uint64_t>(summary.sampleCount) * static_cast<std::uint64_t>(summary.formatSizeBytes);
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(summary.traceCount) * bytesPerTrace;
    if(dataStart + dataBytes > sourceSize) {
        outResult.message = "The file is smaller than its trace layout claims.";
        return false;
    }

    const std::uint64_t windowRecords = std::max<std::uint64_t>(1, options.windowBytes / bytesPerTrace);
    const std::uint64_t windowBytes = windowRecords * bytesPerTrace;
    const std::uint64_t totalWindows = (dataBytes + windowBytes - 1) / windowBytes;
    std::uint64_t windowLimit = totalWindows;
    if(options.maxSourceBytes > 0) {
        const std::uint64_t boundedWindows =
            std::max<std::uint64_t>(1, options.maxSourceBytes / windowBytes);
        windowLimit = std::max<std::uint64_t>(1, std::min(totalWindows, boundedWindows));
    }
    const int queueDepth = std::max(1, std::min(options.readQueueDepth, 16));

    auto index = std::make_shared<SgyIndex>();
    index->path = absolutePath;
    index->fileSize = sourceSize;
    index->modifiedTimeTicks = mtimeTicks;
    index->traceCount = summary.traceCount;
    index->sampleCount = summary.sampleCount;
    index->sampleIntervalUs = summary.sampleIntervalUs;
    index->formatCode = summary.formatCode;
    index->formatSizeBytes = summary.formatSizeBytes;
    index->endianness = summary.endianness;
    index->encoding = summary.encoding;
    index->inlineMin = index->xlineMin = std::numeric_limits<int>::max();
    index->inlineMax = index->xlineMax = std::numeric_limits<int>::min();
    index->traces.reserve(static_cast<std::size_t>(summary.traceCount));
    index->traceByInlineXline.reserve(static_cast<std::size_t>(summary.traceCount));
    std::set<int> inlineSet;
    std::set<int> xlineSet;
    const int coordinateSampleInterval = std::max(1, summary.traceCount / 2048);

    CheckpointState checkpoint;
    std::string checkpointReason;
    const bool resumed = LoadCheckpoint(options.checkpointPath, absolutePath, sourceSize, mtimeTicks,
                                        bytesPerTrace, dataStart, checkpoint, checkpointReason);
    const std::filesystem::path pairPath = PairPathFor(options.checkpointPath);
    const std::filesystem::path coordPath = CoordPathFor(options.checkpointPath);
    std::uint64_t startRecord = 0;
    if(resumed) {
        // The two partial files are validated and (if a crash left a tail)
        // truncated independently, so an interleaved write order can never
        // corrupt the resume.
        bool ok = true;
        std::string failReason;
        std::vector<unsigned char> pairs;
        {
            std::ifstream pairIn(pairPath, std::ios::binary);
            if(!pairIn) {
                ok = false;
                failReason = "checkpoint pair file is missing";
            } else {
                pairs.assign(std::istreambuf_iterator<char>(pairIn), std::istreambuf_iterator<char>());
                if(pairs.size() < checkpoint.pairBytes) {
                    ok = false;
                    failReason = "checkpoint pair file is shorter than its checkpoint";
                } else if(Fnv1a(pairs.data(), static_cast<std::size_t>(checkpoint.pairBytes),
                                1099511628211ull) != checkpoint.pairCrc) {
                    ok = false;
                    failReason = "checkpoint pair file failed its CRC";
                } else if(pairs.size() > checkpoint.pairBytes) {
                    std::error_code truncateError;
                    std::filesystem::resize_file(pairPath, checkpoint.pairBytes, truncateError);
                    pairs.resize(static_cast<std::size_t>(checkpoint.pairBytes));
                }
            }
        }
        std::vector<unsigned char> coords;
        if(ok) {
            std::ifstream coordIn(coordPath, std::ios::binary);
            if(!coordIn) {
                ok = false;
                failReason = "checkpoint coordinate file is missing";
            } else {
                coords.assign(std::istreambuf_iterator<char>(coordIn), std::istreambuf_iterator<char>());
                if(coords.size() < checkpoint.coordBytes) {
                    ok = false;
                    failReason = "checkpoint coordinate file is shorter than its checkpoint";
                } else if(Fnv1a(coords.data(), static_cast<std::size_t>(checkpoint.coordBytes),
                                1099511628211ull) != checkpoint.coordCrc) {
                    ok = false;
                    failReason = "checkpoint coordinate file failed its CRC";
                } else if(coords.size() > checkpoint.coordBytes) {
                    std::error_code truncateError;
                    std::filesystem::resize_file(coordPath, checkpoint.coordBytes, truncateError);
                    coords.resize(static_cast<std::size_t>(checkpoint.coordBytes));
                }
            }
        }
        if(ok) {
            std::size_t offset = 0;
            for(std::uint64_t record = 0; record < checkpoint.pairRecords; ++record) {
                std::uint32_t inlineNo = 0;
                std::uint32_t xlineNo = 0;
                if(!ReadU32(pairs, offset, inlineNo) || !ReadU32(pairs, offset, xlineNo)) {
                    ok = false;
                    failReason = "checkpoint pair file is inconsistent";
                    break;
                }
                const int inlineValue = static_cast<int>(inlineNo);
                const int xlineValue = static_cast<int>(xlineNo);
                if(inlineValue == 0 && xlineValue == 0) {
                    continue;
                }
                index->traces.push_back({ inlineValue, xlineValue, static_cast<int>(record) });
                index->traceByInlineXline[SgyIndex::MakeKey(inlineValue, xlineValue)] =
                    static_cast<int>(record);
                inlineSet.insert(inlineValue);
                xlineSet.insert(xlineValue);
                index->inlineMin = std::min(index->inlineMin, inlineValue);
                index->inlineMax = std::max(index->inlineMax, inlineValue);
                index->xlineMin = std::min(index->xlineMin, xlineValue);
                index->xlineMax = std::max(index->xlineMax, xlineValue);
                index->scannedTraceCount = static_cast<int>(record) + 1;
            }
        }
        if(ok) {
            std::size_t offset = 0;
            for(std::uint64_t c = 0; c < checkpoint.coordCount; ++c) {
                std::uint32_t traceOrdinal = 0;
                std::uint32_t inlineNo = 0;
                std::uint32_t xlineNo = 0;
                if(!ReadU32(coords, offset, traceOrdinal) || !ReadU32(coords, offset, inlineNo) ||
                   !ReadU32(coords, offset, xlineNo) || offset + 16 > coords.size()) {
                    ok = false;
                    failReason = "checkpoint coordinate file is inconsistent";
                    break;
                }
                double x = 0.0;
                double y = 0.0;
                std::memcpy(&x, coords.data() + offset, 8);
                std::memcpy(&y, coords.data() + offset + 8, 8);
                offset += 16;
                SgyCoordinateSample sample;
                sample.traceIndex = static_cast<int>(traceOrdinal);
                sample.inlineNo = static_cast<int>(inlineNo);
                sample.xlineNo = static_cast<int>(xlineNo);
                sample.x = x;
                sample.y = y;
                index->coordinateSamples.push_back(sample);
            }
        }
        if(ok) {
            startRecord = checkpoint.resumeRecord;
            outResult.stats.resumedFromRecord = checkpoint.resumeRecord;
            outResult.stats.resumedFromByte = checkpoint.resumeByte;
        } else {
            checkpoint = CheckpointState{};
            checkpointReason = failReason;
        }
    }

    WindowReader reader;
    std::string readError;
    if(!reader.Open(absolutePath, queueDepth, readError)) {
        outResult.message = readError;
        return false;
    }

    // Cross-check the window layout against segyio's own stride calculation so a
    // wrong stride can never be mistaken for real geometry.
    {
        sgyio::Handle reference(sgyio::OpenReadOnly(absolutePath));
        if(!reference) {
            outResult.message = "segy_open failed while validating the scan layout.";
            return false;
        }
        std::array<char, SEGY_TRACE_HEADER_SIZE> referenceHeader{};
        if(segy_collect_metadata(reference.Get(), -1, -1, 0) != SEGY_OK ||
           segy_read_standard_traceheader(reference.Get(), 0, referenceHeader.data()) != SEGY_OK) {
            outResult.message = "cannot read the first trace header for layout validation.";
            return false;
        }
        // A dedicated queue-depth-1 reader avoids mixing a synchronous read
        // with an overlapped handle.
        WindowReader crossReader;
        std::string crossError;
        std::vector<char> firstRecord(static_cast<std::size_t>(bytesPerTrace));
        std::size_t read = 0;
        bool firstOk = false;
        if(crossReader.Open(absolutePath, 1, crossError)) {
#ifdef _WIN32
            firstOk = ReadWindowSync(crossReader.Handle(), dataStart, firstRecord.data(),
                                     firstRecord.size(), read, crossError);
#else
            firstOk = ReadWindowSync(crossReader.Stream(), dataStart, firstRecord.data(),
                                     firstRecord.size(), read, crossError);
#endif
        }
        if(readError.empty()) {
            readError = crossError;
        }
        if(!firstOk || read != firstRecord.size()) {
            outResult.message = readError.empty() ? "short read on the first record." : readError;
            return false;
        }
        outResult.stats.readCalls += 1;
        outResult.stats.readBytes += firstRecord.size();
        outResult.stats.readRanges += 1;
        if(std::memcmp(firstRecord.data(), referenceHeader.data(), SEGY_TRACE_HEADER_SIZE) != 0) {
            outResult.message = "the sequential window layout disagrees with segyio's trace stride.";
            return false;
        }
    }

    std::ofstream pairOut;
    std::ofstream coordOut;
    std::uint64_t pairRecords = checkpoint.pairRecords;
    std::uint64_t pairBytes = checkpoint.pairBytes;
    std::uint64_t pairCrc = checkpoint.pairCrc;
    std::uint64_t coordCount = checkpoint.coordCount;
    std::uint64_t coordBytes = checkpoint.coordBytes;
    std::uint64_t coordCrc = checkpoint.coordCrc;
    if(!options.checkpointPath.empty()) {
        // Checkpoints live in an isolated cache directory that may not exist yet.
        std::error_code directoryError;
        if(!options.checkpointPath.parent_path().empty()) {
            std::filesystem::create_directories(options.checkpointPath.parent_path(), directoryError);
        }
        const bool appendPairs = resumed && pairBytes > 0;
        pairOut.open(pairPath, std::ios::binary | (appendPairs ? std::ios::app : std::ios::trunc));
        if(!pairOut) {
            outResult.message = "cannot open the scan pair partial file.";
            return false;
        }
        const bool appendCoords = resumed && coordBytes > 0;
        coordOut.open(coordPath, std::ios::binary | (appendCoords ? std::ios::app : std::ios::trunc));
        if(!coordOut) {
            outResult.message = "cannot open the scan coordinate partial file.";
            return false;
        }
        if(!resumed) {
            // A rejected legacy (v1) checkpoint must not leave its interleaved
            // partial behind.
            std::error_code legacyError;
            std::filesystem::remove(LegacyPartialPathFor(options.checkpointPath), legacyError);
        }
    }

    std::uint64_t lastCheckpointPairBytes = pairBytes;
    std::uint64_t lastCheckpointCoordBytes = coordBytes;
    std::uint64_t nextCheckpointByte = 0;
    if(!options.checkpointPath.empty() && options.checkpointIntervalBytes > 0) {
        nextCheckpointByte = ((pairRecords * bytesPerTrace / options.checkpointIntervalBytes) + 1) *
                             options.checkpointIntervalBytes;
    }

    auto writeCheckpoint = [&](std::uint64_t resumeRecord) -> bool {
        if(options.checkpointPath.empty() || options.checkpointIntervalBytes == 0) {
            return true;
        }
        if(pairOut.is_open()) {
            pairOut.flush();
            if(!pairOut) {
                outResult.message = "cannot flush the scan pair partial file.";
                return false;
            }
        }
        if(coordOut.is_open()) {
            coordOut.flush();
            if(!coordOut) {
                outResult.message = "cannot flush the scan coordinate partial file.";
                return false;
            }
        }
        CheckpointState state;
        state.resumeRecord = resumeRecord;
        state.resumeByte = resumeRecord * bytesPerTrace;
        state.pairRecords = pairRecords;
        state.pairBytes = pairBytes;
        state.pairCrc = pairCrc;
        state.coordCount = coordCount;
        state.coordBytes = coordBytes;
        state.coordCrc = coordCrc;
        state.valid = true;
        std::string checkpointError;
        if(!SaveCheckpoint(options.checkpointPath, absolutePath, sourceSize, mtimeTicks,
                           bytesPerTrace, dataStart, state, checkpointError)) {
            outResult.message = checkpointError;
            return false;
        }
        outResult.stats.checkpointWrites += 1;
        outResult.stats.checkpointBytes += (pairBytes - lastCheckpointPairBytes) +
                                           (coordBytes - lastCheckpointCoordBytes);
        lastCheckpointPairBytes = pairBytes;
        lastCheckpointCoordBytes = coordBytes;
        return true;
    };

    const auto parseStart = std::chrono::steady_clock::now();
    std::uint64_t windowsDone = 0;
    std::uint64_t processedByte = startRecord * bytesPerTrace;
    bool cancelled = false;
    bool failed = false;

    std::vector<char> singleBuffer;
    if(queueDepth == 1) {
        singleBuffer.assign(static_cast<std::size_t>(windowBytes), 0);
    }
    // Records consumed in this run (with or without checkpointing); the window
    // start check must not depend on the checkpoint being enabled.
    std::uint64_t recordsConsumed = startRecord;
#ifdef _WIN32
    std::vector<bool> pendingSlots(static_cast<std::size_t>(queueDepth), false);
#endif

    const bool alreadyComplete = resumed &&
        checkpoint.pairRecords >= static_cast<std::uint64_t>(summary.traceCount);
    const std::uint64_t startWindow = startRecord / windowRecords;
    std::uint64_t issuedWindows = startWindow;
    if(alreadyComplete) {
        // The checkpoint already covers every record: nothing left to read.
        processedByte = dataBytes;
    }
    for(std::uint64_t window = startWindow; !alreadyComplete && window < windowLimit; ++window) {
        if(options.shouldCancel && options.shouldCancel()) {
            cancelled = true;
            break;
        }
        const std::uint64_t recordStart = window * windowRecords;
        if(recordStart != recordsConsumed) {
            outResult.message = "internal error: window start disagrees with the scan progress.";
            failed = true;
            break;
        }
        const std::uint64_t recordCount = std::min<std::uint64_t>(
            windowRecords, static_cast<std::uint64_t>(summary.traceCount) - recordStart);
        const std::uint64_t offset = dataStart + recordStart * bytesPerTrace;
        const std::size_t bytes = static_cast<std::size_t>(recordCount * bytesPerTrace);
        const char* windowData = nullptr;
        const auto windowState = std::make_tuple(pairRecords, pairBytes, pairCrc, coordCount,
                                                 coordBytes, coordCrc);

#ifdef _WIN32
        if(queueDepth > 1) {
            while(issuedWindows < windowLimit && issuedWindows < window + static_cast<std::uint64_t>(queueDepth)) {
                const std::uint64_t target = issuedWindows;
                const std::size_t slot = static_cast<std::size_t>(target % static_cast<std::uint64_t>(queueDepth));
                const std::uint64_t targetRecord = target * windowRecords;
                const std::uint64_t targetCount = std::min<std::uint64_t>(
                    windowRecords, static_cast<std::uint64_t>(summary.traceCount) - targetRecord);
                const std::uint64_t targetOffset = dataStart + targetRecord * bytesPerTrace;
                const std::size_t targetBytes = static_cast<std::size_t>(targetCount * bytesPerTrace);
                OVERLAPPED& overlapped = reader.Overlapped()[slot];
                std::memset(&overlapped, 0, sizeof(OVERLAPPED));
                overlapped.Offset = static_cast<DWORD>(targetOffset & 0xFFFFFFFFull);
                overlapped.OffsetHigh = static_cast<DWORD>(targetOffset >> 32);
                overlapped.hEvent = reader.Events()[slot];
                ResetEvent(reader.Events()[slot]);
                std::vector<char>& buffer = reader.Buffers()[slot];
                buffer.assign(targetBytes, 0);
                bool pending = false;
                if(!ReadFile(reader.Handle(), buffer.data(), static_cast<DWORD>(targetBytes), nullptr, &overlapped)) {
                    const DWORD overlappedError = GetLastError();
                    if(overlappedError != ERROR_IO_PENDING) {
                        outResult.message = "overlapped ReadFile failed during a sequential scan.";
                        failed = true;
                        break;
                    }
                    pending = true;
                }
                pendingSlots[slot] = pending;
                outResult.stats.readCalls += 1;
                outResult.stats.readBytes += targetBytes;
                ++issuedWindows;
            }
            if(failed) {
                break;
            }
            const std::size_t slot = static_cast<std::size_t>(window % static_cast<std::uint64_t>(queueDepth));
            if(pendingSlots[slot]) {
                WaitForSingleObject(reader.Events()[slot], INFINITE);
            }
            DWORD transferred = 0;
            if(!GetOverlappedResult(reader.Handle(), &reader.Overlapped()[slot], &transferred, FALSE) ||
               transferred != static_cast<DWORD>(bytes)) {
                outResult.message = "short overlapped read during a sequential scan.";
                failed = true;
                break;
            }
            windowData = reader.Buffers()[slot].data();
        } else {
            std::size_t read = 0;
            if(!ReadWindowSync(reader.Handle(), offset, singleBuffer.data(), bytes, read, readError) ||
               read != bytes) {
                outResult.message = readError.empty() ? "short read during a sequential scan." : readError;
                failed = true;
                break;
            }
            outResult.stats.readCalls += 1;
            outResult.stats.readBytes += bytes;
            windowData = singleBuffer.data();
        }
#else
        std::size_t read = 0;
        if(!ReadWindowSync(reader.Stream(), offset, singleBuffer.data(), bytes, read, readError) ||
           read != bytes) {
            outResult.message = readError.empty() ? "short read during a sequential scan." : readError;
            failed = true;
            break;
        }
        outResult.stats.readCalls += 1;
        outResult.stats.readBytes += bytes;
        windowData = singleBuffer.data();
#endif
        outResult.stats.sequentialWindows += 1;
        outResult.stats.readRanges += 1;

        bool windowCancelled = false;
        for(std::uint64_t r = 0; r < recordCount; ++r) {
            const char* record = windowData + static_cast<std::size_t>(r * bytesPerTrace);
            const int traceOrdinal = static_cast<int>(recordStart + r);
            int inlineNo = 0;
            int xlineNo = 0;
            segy_get_tracefield_int(record, SEGY_TR_INLINE, &inlineNo);
            segy_get_tracefield_int(record, SEGY_TR_CROSSLINE, &xlineNo);
            ++recordsConsumed;
            if(visitor && !visitor(traceOrdinal, record, static_cast<int>(bytesPerTrace))) {
                windowCancelled = true;
                cancelled = true;
                break;
            }
            if(pairOut.is_open()) {
                const std::uint32_t storedInline = static_cast<std::uint32_t>(inlineNo);
                const std::uint32_t storedXline = static_cast<std::uint32_t>(xlineNo);
                const unsigned char pair[8] = {
                    static_cast<unsigned char>(storedInline & 0xFF),
                    static_cast<unsigned char>((storedInline >> 8) & 0xFF),
                    static_cast<unsigned char>((storedInline >> 16) & 0xFF),
                    static_cast<unsigned char>((storedInline >> 24) & 0xFF),
                    static_cast<unsigned char>(storedXline & 0xFF),
                    static_cast<unsigned char>((storedXline >> 8) & 0xFF),
                    static_cast<unsigned char>((storedXline >> 16) & 0xFF),
                    static_cast<unsigned char>((storedXline >> 24) & 0xFF) };
                pairOut.write(reinterpret_cast<const char*>(pair), 8);
                pairCrc = Fnv1a(pair, 8, pairCrc);
                pairBytes += 8;
                ++pairRecords;
            }
            if(inlineNo == 0 && xlineNo == 0) {
                outResult.stats.recordsSkipped += 1;
                continue;
            }
            index->traces.push_back({ inlineNo, xlineNo, traceOrdinal });
            index->traceByInlineXline[SgyIndex::MakeKey(inlineNo, xlineNo)] = traceOrdinal;
            inlineSet.insert(inlineNo);
            xlineSet.insert(xlineNo);
            if(traceOrdinal % coordinateSampleInterval == 0 && index->coordinateSamples.size() < 4096) {
                int cdpX = 0;
                int cdpY = 0;
                int srcX = 0;
                int srcY = 0;
                int coordUnits = 0;
                int sourceGroupScalar = 0;
                segy_get_tracefield_int(record, SEGY_TR_CDP_X, &cdpX);
                segy_get_tracefield_int(record, SEGY_TR_CDP_Y, &cdpY);
                segy_get_tracefield_int(record, SEGY_TR_SOURCE_X, &srcX);
                segy_get_tracefield_int(record, SEGY_TR_SOURCE_Y, &srcY);
                segy_get_tracefield_int(record, SEGY_TR_SOURCE_GROUP_SCALAR, &sourceGroupScalar);
                segy_get_tracefield_int(record, SEGY_TR_COORD_UNITS, &coordUnits);
                const int rawX = cdpX != 0 ? cdpX : srcX;
                const int rawY = cdpY != 0 ? cdpY : srcY;
                if(rawX != 0 || rawY != 0) {
                    const double scale = sourceGroupScalar < 0
                        ? 1.0 / static_cast<double>(-sourceGroupScalar)
                        : static_cast<double>(sourceGroupScalar > 0 ? sourceGroupScalar : 1);
                    SgyCoordinateSample sample;
                    sample.traceIndex = traceOrdinal;
                    sample.inlineNo = inlineNo;
                    sample.xlineNo = xlineNo;
                    sample.x = static_cast<double>(rawX) * scale;
                    sample.y = static_cast<double>(rawY) * scale;
                    index->coordinateSamples.push_back(sample);
                    if(coordOut.is_open()) {
                        const std::uint32_t t = static_cast<std::uint32_t>(traceOrdinal);
                        const std::uint32_t i = static_cast<std::uint32_t>(inlineNo);
                        const std::uint32_t x = static_cast<std::uint32_t>(xlineNo);
                        const unsigned char header[12] = {
                            static_cast<unsigned char>(t & 0xFF), static_cast<unsigned char>((t >> 8) & 0xFF),
                            static_cast<unsigned char>((t >> 16) & 0xFF), static_cast<unsigned char>((t >> 24) & 0xFF),
                            static_cast<unsigned char>(i & 0xFF), static_cast<unsigned char>((i >> 8) & 0xFF),
                            static_cast<unsigned char>((i >> 16) & 0xFF), static_cast<unsigned char>((i >> 24) & 0xFF),
                            static_cast<unsigned char>(x & 0xFF), static_cast<unsigned char>((x >> 8) & 0xFF),
                            static_cast<unsigned char>((x >> 16) & 0xFF), static_cast<unsigned char>((x >> 24) & 0xFF) };
                        unsigned char value[16];
                        std::memcpy(value, &sample.x, 8);
                        std::memcpy(value + 8, &sample.y, 8);
                        coordOut.write(reinterpret_cast<const char*>(header), 12);
                        coordOut.write(reinterpret_cast<const char*>(value), 16);
                        coordCrc = Fnv1a(header, 12, coordCrc);
                        coordCrc = Fnv1a(value, 16, coordCrc);
                        coordBytes += 28;
                        ++coordCount;
                    }
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
            index->scannedTraceCount = traceOrdinal + 1;
        }
        if(windowCancelled) {
            // Roll the partial file back to the window start so the checkpoint
            // always points at a whole window boundary.
            pairRecords = std::get<0>(windowState);
            pairBytes = std::get<1>(windowState);
            pairCrc = std::get<2>(windowState);
            coordCount = std::get<3>(windowState);
            coordBytes = std::get<4>(windowState);
            coordCrc = std::get<5>(windowState);
            recordsConsumed = recordStart;
            if(pairOut.is_open()) {
                pairOut.close();
                std::error_code resizeError;
                std::filesystem::resize_file(pairPath, pairBytes, resizeError);
                pairOut.open(pairPath, std::ios::binary | std::ios::app);
            }
            if(coordOut.is_open()) {
                coordOut.close();
                std::error_code resizeError;
                std::filesystem::resize_file(coordPath, coordBytes, resizeError);
                coordOut.open(coordPath, std::ios::binary | std::ios::app);
            }
            break;
        }

        outResult.stats.recordsParsed += recordCount;
        processedByte = offset + bytes;
        ++windowsDone;
        if(options.checkpointIntervalBytes > 0 && !options.checkpointPath.empty() &&
           processedByte >= nextCheckpointByte) {
            if(!writeCheckpoint(pairRecords)) {
                failed = true;
                break;
            }
            nextCheckpointByte = processedByte + options.checkpointIntervalBytes;
        }
    }
    outResult.stats.parseMs = MsSince(parseStart);
    outResult.stats.peakBufferBytes = static_cast<std::size_t>(
        (queueDepth > 1 ? static_cast<std::size_t>(queueDepth) * windowBytes : windowBytes) +
        index->traces.capacity() * sizeof(SgyTraceRef) +
        index->coordinateSamples.capacity() * sizeof(SgyCoordinateSample));

    if(failed) {
        outResult.stats.wallMs = MsSince(wallStart);
        if(pairOut.is_open()) {
            pairOut.flush();
        }
        if(coordOut.is_open()) {
            coordOut.flush();
        }
        return false;
    }

    const bool whole = !cancelled && windowLimit == totalWindows && processedByte >= dataBytes;
    if(!whole) {
        if(!writeCheckpoint(pairRecords)) {
            outResult.stats.wallMs = MsSince(wallStart);
            return false;
        }
        outResult.cancelled = cancelled;
        if(!cancelled) {
            outResult.bounded = true;
            outResult.message = "the scan was bounded by a diagnostic limit.";
        }
        outResult.stats.wallMs = MsSince(wallStart);
        if(pairOut.is_open()) {
            pairOut.flush();
        }
        if(coordOut.is_open()) {
            coordOut.flush();
        }
        // Only a complete scan reports success: a bounded or cancelled scan is
        // resumable progress, never a published index.
        return false;
    }

    // Final integrity check: the last physical record must match segyio's view.
    {
        sgyio::Handle reference(sgyio::OpenReadOnly(absolutePath));
        std::array<char, SEGY_TRACE_HEADER_SIZE> referenceHeader{};
        if(!reference || segy_collect_metadata(reference.Get(), -1, -1, 0) != SEGY_OK ||
           segy_read_standard_traceheader(reference.Get(), summary.traceCount - 1,
                                          referenceHeader.data()) != SEGY_OK) {
            outResult.message = "cannot read the last trace header for final validation.";
            outResult.stats.wallMs = MsSince(wallStart);
            return false;
        }
        std::vector<char> lastRecord(static_cast<std::size_t>(bytesPerTrace));
        std::ifstream lastIn(absolutePath, std::ios::binary);
        lastIn.seekg(static_cast<std::streamoff>(
            dataStart + static_cast<std::uint64_t>(summary.traceCount - 1) * bytesPerTrace));
        lastIn.read(lastRecord.data(), static_cast<std::streamsize>(lastRecord.size()));
        if(!lastIn || static_cast<std::size_t>(lastIn.gcount()) != lastRecord.size() ||
           std::memcmp(lastRecord.data(), referenceHeader.data(), SEGY_TRACE_HEADER_SIZE) != 0) {
            outResult.message = "the final record disagrees with segyio's trace view.";
            outResult.stats.wallMs = MsSince(wallStart);
            return false;
        }
        outResult.stats.readCalls += 1;
        outResult.stats.readBytes += lastRecord.size();
        outResult.stats.readRanges += 1;
    }

    if(index->traces.empty()) {
        outResult.message = "No valid inline/crossline trace headers found.";
        outResult.stats.wallMs = MsSince(wallStart);
        return false;
    }
    index->inlineValues.assign(inlineSet.begin(), inlineSet.end());
    index->xlineValues.assign(xlineSet.begin(), xlineSet.end());
    index->coordinateFieldsPresent = index->coordinateSamples.size() >= 6;
    index->scannedTraceCount = summary.traceCount;
    index->complete = true;

    outResult.index = std::move(index);
    outResult.complete = true;
    outResult.stats.wallMs = MsSince(wallStart);
    if(pairOut.is_open()) {
        pairOut.flush();
        pairOut.close();
    }
    if(coordOut.is_open()) {
        coordOut.flush();
        coordOut.close();
    }
    // A completed scan has no resumable state left: remove its own checkpoint.
    RemoveSegyScanCheckpoint(options.checkpointPath);
    return true;
}

void RemoveSegyScanCheckpoint(const std::filesystem::path& checkpointPath) {
    if(checkpointPath.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(checkpointPath, error);
    std::filesystem::remove(PairPathFor(checkpointPath), error);
    std::filesystem::remove(CoordPathFor(checkpointPath), error);
    std::filesystem::remove(LegacyPartialPathFor(checkpointPath), error);
}

} // namespace seismic
