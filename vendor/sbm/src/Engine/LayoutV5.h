#pragma once

// Stage L experiment: Workspace v5 layout + page container.
//
// This is a NEW experimental container (magic SF3VL5, version 5) written to its
// own directory. It never touches the production workspace format or the
// existing ROI workspace. Its purpose is to measure, on the real ROI, how the
// logical chunk geometry (I x X x T) and the page geometry inside each chunk
// change physical read bytes, read ranges and read amplification for the
// workload mix (trace / inline / xline / time slice / section / neighbouring).
//
// Page layer: every logical chunk is split into pages; the page directory holds
// an independent offset, length, raw size and CRC per page, so a damaged page
// is found alone and raw pages can be read partially.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace seismic {
namespace engine {

constexpr char kLayoutV5Magic[8] = {'S', 'F', '3', 'V', 'L', '5', '0', '0'};
constexpr std::uint32_t kLayoutV5Version = 5;

struct LayoutV5Spec {
    const char* name = "a64";
    std::uint32_t chunkInline = 64;
    std::uint32_t chunkXline = 64;
    std::uint32_t chunkSample = 64;
    std::uint32_t pageInline = 16;
    std::uint32_t pageXline = 16;
    std::uint32_t pageSample = 64;
};

// The four candidates of this round.
LayoutV5Spec LayoutV5SpecByName(const std::string& name);
std::string LayoutV5SpecList();

struct LayoutV5Info {
    LayoutV5Spec spec;
    std::uint32_t volumeInlines = 0;
    std::uint32_t volumeXlines = 0;
    std::uint32_t volumeSamples = 0;
    std::int32_t inlineMin = 0;
    std::int32_t xlineMin = 0;
    std::uint64_t sourceIdentityHash = 0;
    std::uint32_t codec = 0; // 0 = raw pages, 1 = zstd pages

    std::uint32_t ChunksI() const;
    std::uint32_t ChunksX() const;
    std::uint32_t ChunksT() const;
    std::uint64_t ChunkCount() const;
    std::uint32_t PagesI() const;
    std::uint32_t PagesX() const;
    std::uint32_t PagesT() const;
    std::uint64_t PagesPerChunk() const;
    std::uint64_t PageCount() const;
    std::uint64_t PageBytes() const;
    std::uint64_t LogicalBytes() const;
};

struct LayoutV5Stats {
    std::uint64_t readCalls = 0;
    std::uint64_t readBytes = 0;
    std::uint64_t readRanges = 0;
    std::uint64_t pagesTouched = 0;
    std::uint64_t chunksTouched = 0;
    std::uint64_t neededInPageBytes = 0; // bytes actually needed inside touched pages
    std::uint64_t outputBytes = 0;       // logical bytes of the assembled result
    std::uint64_t partialPageReads = 0;
    std::uint64_t checksumFailures = 0;
    double wallMs = 0.0;

    double Amplification() const {
        return outputBytes == 0 ? 0.0 : static_cast<double>(readBytes) / static_cast<double>(outputBytes);
    }
};

class LayoutV5Writer {
public:
    ~LayoutV5Writer();
    bool Open(const std::filesystem::path& path, const LayoutV5Info& info, std::string& errorMessage);
    // values has chunkInline*chunkXline*chunkSample floats; NaN outside the volume.
    bool WriteChunk(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                    const float* values, std::string& errorMessage);
    bool Finalize(std::string& errorMessage);
    std::uint64_t WriteCalls() const { return writeCalls_; }
    std::uint64_t BytesWritten() const { return bytesWritten_; }

private:
    struct Impl;
    // shared_ptr so the incomplete Impl does not force the destructor into every
    // translation unit that only uses the writer.
    std::shared_ptr<Impl> impl_;
    std::uint64_t writeCalls_ = 0;
    std::uint64_t bytesWritten_ = 0;
};

// Builds a v5 layout from a raw workspace (chunk-by-chunk, bounded source cache).
bool BuildLayoutV5FromWorkspace(const std::filesystem::path& roiWorkspace,
                                const std::filesystem::path& outFile,
                                const LayoutV5Spec& spec,
                                std::uint32_t codec,
                                std::string& errorMessage,
                                std::uint64_t& sourceChunkReads,
                                LayoutV5Info* outInfo = nullptr);

class LayoutV5Reader {
public:
    bool Open(const std::filesystem::path& path, std::string& errorMessage);
    const LayoutV5Info& Info() const { return info_; }
    const LayoutV5Stats& Stats() const { return stats_; }
    void ResetStats() { stats_ = LayoutV5Stats{}; }

    // Generic box read (NaN outside the volume); counts all physical I/O.
    bool ReadBox(int inlineIndex, int xlineIndex, int sampleIndex,
                 int inlines, int xlines, int samples,
                 std::vector<float>& out, std::string& errorMessage);
    bool ReadTrace(int inlineIndex, int xlineIndex, std::vector<float>& out, std::string& errorMessage);
    bool ReadInline(int inlineIndex, std::vector<float>& out, std::string& errorMessage);
    bool ReadCrossline(int xlineIndex, std::vector<float>& out, std::string& errorMessage);
    bool ReadTimeSlice(int sampleIndex, std::vector<float>& out, std::string& errorMessage);

    // Page index for a chunk/page coordinate (diagnostics and partial reads).
    std::uint64_t PageIndexOf(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                              std::uint32_t pi, std::uint32_t px, std::uint32_t pt) const {
        return PageOrdinal(ci, cx, ct, pi, px, pt);
    }

    // Real raw partial read: a contiguous byte range inside one page.
    bool ReadPageBytes(std::uint64_t pageIndex, std::uint64_t byteOffset, std::uint64_t byteLength,
                       std::vector<unsigned char>& out, std::string& errorMessage);

private:
    struct PageEntry {
        std::uint64_t offset = 0;
        std::uint32_t storedBytes = 0;
        std::uint32_t rawBytes = 0;
        std::uint64_t checksum = 0;
        std::uint32_t codec = 0;
    };
    std::uint64_t ChunkOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct) const;
    std::uint64_t PageOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                              std::uint32_t pi, std::uint32_t px, std::uint32_t pt) const;
    bool ReadPage(std::uint64_t pageIndex, std::vector<float>& out, std::string& errorMessage);
    bool ReadPageRaw(std::uint64_t pageIndex, std::vector<unsigned char>& out, std::string& errorMessage);

    LayoutV5Info info_;
    std::vector<PageEntry> pages_;
    std::vector<std::uint64_t> chunkPageStart_;
    std::uint64_t dataOffset_ = 0;
    std::shared_ptr<std::ifstream> file_;
    LayoutV5Stats stats_;
};

} // namespace engine
} // namespace seismic
