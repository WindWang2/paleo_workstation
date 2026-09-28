#include "Engine/LayoutV5.h"

#include "Engine/WorkspaceFormat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

#ifdef SEISMIC_HAVE_ZSTD
#include <zstd.h>
#endif

namespace seismic {
namespace engine {
namespace {

constexpr std::uint64_t kFnvOffset = 1099511628211ull;

double MsSince(const std::chrono::steady_clock::time_point& start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1000.0;
}

std::uint64_t Fnv1a(const void* data, std::size_t size, std::uint64_t hash = kFnvOffset) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for(std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename T>
void Append(std::vector<unsigned char>& out, const T& value) {
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool Consume(const std::vector<unsigned char>& data, std::size_t& offset, T& value) {
    if(offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

} // namespace

LayoutV5Spec LayoutV5SpecByName(const std::string& name) {
    if(name == "a64") {
        return LayoutV5Spec{ "a64", 64, 64, 64, 16, 16, 64 };
    }
    if(name == "b8x8x256") {
        return LayoutV5Spec{ "b8x8x256", 8, 8, 256, 8, 8, 16 };
    }
    if(name == "c8x16x256") {
        return LayoutV5Spec{ "c8x16x256", 8, 16, 256, 8, 16, 16 };
    }
    if(name == "d4x32x512") {
        return LayoutV5Spec{ "d4x32x512", 4, 32, 512, 4, 32, 16 };
    }
    return LayoutV5Spec{};
}

std::string LayoutV5SpecList() {
    return "a64 (64x64x64, baseline), b8x8x256, c8x16x256, d4x32x512";
}

std::uint32_t LayoutV5Info::ChunksI() const {
    return (volumeInlines + spec.chunkInline - 1) / std::max(1u, spec.chunkInline);
}

std::uint32_t LayoutV5Info::ChunksX() const {
    return (volumeXlines + spec.chunkXline - 1) / std::max(1u, spec.chunkXline);
}

std::uint32_t LayoutV5Info::ChunksT() const {
    return (volumeSamples + spec.chunkSample - 1) / std::max(1u, spec.chunkSample);
}

std::uint64_t LayoutV5Info::ChunkCount() const {
    return static_cast<std::uint64_t>(ChunksI()) * ChunksX() * ChunksT();
}

std::uint32_t LayoutV5Info::PagesI() const {
    return (spec.chunkInline + spec.pageInline - 1) / std::max(1u, spec.pageInline);
}

std::uint32_t LayoutV5Info::PagesX() const {
    return (spec.chunkXline + spec.pageXline - 1) / std::max(1u, spec.pageXline);
}

std::uint32_t LayoutV5Info::PagesT() const {
    return (spec.chunkSample + spec.pageSample - 1) / std::max(1u, spec.pageSample);
}

std::uint64_t LayoutV5Info::PagesPerChunk() const {
    return static_cast<std::uint64_t>(PagesI()) * PagesX() * PagesT();
}

std::uint64_t LayoutV5Info::PageCount() const {
    return ChunkCount() * PagesPerChunk();
}

std::uint64_t LayoutV5Info::PageBytes() const {
    return static_cast<std::uint64_t>(spec.pageInline) * spec.pageXline * spec.pageSample * 4ull;
}

std::uint64_t LayoutV5Info::LogicalBytes() const {
    return static_cast<std::uint64_t>(volumeInlines) * volumeXlines * volumeSamples * 4ull;
}

// ---------------------------------------------------------------- writer

struct LayoutV5Writer::Impl {
    LayoutV5Info info;
    std::ofstream out;
    std::vector<unsigned char> chunkDir; // pageStart (u64) + pageCount (u32)
    std::vector<unsigned char> pageDir;  // offset,stored,raw,crc,codec
    std::uint64_t dataOffset = 0;
    std::uint64_t pageOrdinal = 0;
    std::uint64_t dataWritten = 0; // running payload bytes, keeps offsets O(1)
};

LayoutV5Writer::~LayoutV5Writer() = default;

bool LayoutV5Writer::Open(const std::filesystem::path& path, const LayoutV5Info& info, std::string& errorMessage) {
    impl_ = std::make_shared<Impl>();
    impl_->info = info;
    impl_->out.open(path, std::ios::binary | std::ios::trunc);
    if(!impl_->out) {
        errorMessage = "cannot create the v5 layout file.";
        return false;
    }
    const std::uint64_t chunkCount = info.ChunkCount();
    const std::uint64_t pageCount = info.PageCount();
    const std::uint64_t headerBytes = 128;
    const std::uint64_t chunkDirBytes = chunkCount * 12;
    const std::uint64_t pageDirBytes = pageCount * 32;
    impl_->dataOffset = headerBytes + chunkDirBytes + pageDirBytes;
    impl_->chunkDir.resize(static_cast<std::size_t>(chunkDirBytes), 0);
    impl_->pageDir.resize(static_cast<std::size_t>(pageDirBytes), 0);
    // Reserve the directory area; pages are appended after it.
    impl_->out.seekp(static_cast<std::streamoff>(impl_->dataOffset));
    return true;
}

bool LayoutV5Writer::WriteChunk(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                                const float* values, std::string& errorMessage) {
    if(!impl_) {
        errorMessage = "the v5 writer is not open.";
        return false;
    }
    const LayoutV5Info& info = impl_->info;
    const std::uint32_t pi = info.PagesI();
    const std::uint32_t px = info.PagesX();
    const std::uint32_t pt = info.PagesT();
    const std::uint64_t chunkOrdinal = static_cast<std::uint64_t>(ci) +
        static_cast<std::uint64_t>(info.ChunksI()) *
            (cx + static_cast<std::uint64_t>(info.ChunksX()) * ct);
    const std::uint64_t pageStart = impl_->pageOrdinal;

    std::vector<float> pageBuffer(static_cast<std::size_t>(info.PageBytes() / 4));
    std::vector<unsigned char> compressed;
    for(std::uint32_t a = 0; a < pi; ++a) {
        for(std::uint32_t b = 0; b < px; ++b) {
            for(std::uint32_t c = 0; c < pt; ++c) {
                const std::uint32_t i0 = a * info.spec.pageInline;
                const std::uint32_t x0 = b * info.spec.pageXline;
                const std::uint32_t t0 = c * info.spec.pageSample;
                for(std::uint32_t s = 0; s < info.spec.pageSample; ++s) {
                    const std::uint32_t t = t0 + s;
                    for(std::uint32_t xx = 0; xx < info.spec.pageXline; ++xx) {
                        const std::uint32_t x = x0 + xx;
                        const float* row = values +
                            (static_cast<std::size_t>(t) * info.spec.chunkXline + x) * info.spec.chunkInline + i0;
                        float* dst = pageBuffer.data() +
                            (static_cast<std::size_t>(s) * info.spec.pageXline + xx) * info.spec.pageInline;
                        for(std::uint32_t ii = 0; ii < info.spec.pageInline; ++ii) {
                            dst[ii] = i0 + ii < info.spec.chunkInline ? row[ii] : std::numeric_limits<float>::quiet_NaN();
                        }
                    }
                }
                const std::size_t rawBytes = pageBuffer.size() * sizeof(float);
                const unsigned char* payload = reinterpret_cast<const unsigned char*>(pageBuffer.data());
                std::uint32_t storedBytes = static_cast<std::uint32_t>(rawBytes);
                std::uint32_t codec = 0;
#ifdef SEISMIC_HAVE_ZSTD
                if(info.codec == 1) {
                    compressed.resize(ZSTD_compressBound(rawBytes));
                    const std::size_t written = ZSTD_compress(compressed.data(), compressed.size(),
                                                              payload, rawBytes, 3);
                    if(ZSTD_isError(written)) {
                        errorMessage = std::string("zstd page compress failed: ") + ZSTD_getErrorName(written);
                        return false;
                    }
                    payload = compressed.data();
                    storedBytes = static_cast<std::uint32_t>(written);
                    codec = 1;
                }
#else
                if(info.codec == 1) {
                    errorMessage = "this build has no zstd support for v5 pages.";
                    return false;
                }
#endif
                const std::uint64_t crc = Fnv1a(payload, storedBytes);
                impl_->out.write(reinterpret_cast<const char*>(payload), storedBytes);
                if(!impl_->out) {
                    errorMessage = "cannot write a v5 page.";
                    return false;
                }
                ++writeCalls_;
                bytesWritten_ += storedBytes;
                // page directory entry: indexed by the logical page ordinal
                // (chunk-major), while payloads stay in physical write order.
                std::vector<unsigned char>& dir = impl_->pageDir;
                const std::uint64_t within =
                    static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(pi) *
                        (static_cast<std::uint64_t>(b) + static_cast<std::uint64_t>(px) * c);
                const std::uint64_t dirIndex = chunkOrdinal * info.PagesPerChunk() + within;
                const std::size_t base = static_cast<std::size_t>(dirIndex) * 32;
                const std::uint64_t pageOffset = impl_->dataOffset + impl_->dataWritten;
                impl_->dataWritten += storedBytes;
                std::memcpy(dir.data() + base, &pageOffset, 8);
                std::memcpy(dir.data() + base + 8, &storedBytes, 4);
                std::uint32_t raw32 = static_cast<std::uint32_t>(rawBytes);
                std::memcpy(dir.data() + base + 12, &raw32, 4);
                std::memcpy(dir.data() + base + 16, &crc, 8);
                std::memcpy(dir.data() + base + 24, &codec, 4);
                ++impl_->pageOrdinal;
            }
        }
    }
    std::vector<unsigned char>& cdir = impl_->chunkDir;
    const std::size_t cbase = static_cast<std::size_t>(chunkOrdinal) * 12;
    std::memcpy(cdir.data() + cbase, &pageStart, 8);
    std::uint32_t pageCount = static_cast<std::uint32_t>(info.PagesPerChunk());
    std::memcpy(cdir.data() + cbase + 8, &pageCount, 4);
    return true;
}

bool LayoutV5Writer::Finalize(std::string& errorMessage) {
    if(!impl_) {
        errorMessage = "the v5 writer is not open.";
        return false;
    }
    if(impl_->pageOrdinal != impl_->info.PageCount()) {
        errorMessage = "the v5 layout is incomplete (" + std::to_string(impl_->pageOrdinal) + " of " +
                       std::to_string(impl_->info.PageCount()) + " pages).";
        return false;
    }
    impl_->out.seekp(0, std::ios::beg);
    std::vector<unsigned char> header;
    header.insert(header.end(), kLayoutV5Magic, kLayoutV5Magic + 8);
    Append(header, kLayoutV5Version);
    Append(header, impl_->info.codec);
    Append(header, impl_->info.spec.chunkInline);
    Append(header, impl_->info.spec.chunkXline);
    Append(header, impl_->info.spec.chunkSample);
    Append(header, impl_->info.spec.pageInline);
    Append(header, impl_->info.spec.pageXline);
    Append(header, impl_->info.spec.pageSample);
    Append(header, impl_->info.volumeInlines);
    Append(header, impl_->info.volumeXlines);
    Append(header, impl_->info.volumeSamples);
    Append(header, impl_->info.inlineMin);
    Append(header, impl_->info.xlineMin);
    Append(header, impl_->info.sourceIdentityHash);
    Append(header, impl_->info.ChunkCount());
    Append(header, impl_->info.PageCount());
    header.resize(128, 0);
    impl_->out.write(reinterpret_cast<const char*>(header.data()), 128);
    impl_->out.write(reinterpret_cast<const char*>(impl_->chunkDir.data()),
                     static_cast<std::streamsize>(impl_->chunkDir.size()));
    impl_->out.write(reinterpret_cast<const char*>(impl_->pageDir.data()),
                     static_cast<std::streamsize>(impl_->pageDir.size()));
    impl_->out.flush();
    if(!impl_->out) {
        errorMessage = "cannot finalize the v5 layout.";
        return false;
    }
    impl_->out.close();
    impl_.reset();
    return true;
}

// ---------------------------------------------------------------- reader

std::uint64_t LayoutV5Reader::ChunkOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct) const {
    return static_cast<std::uint64_t>(ci) + static_cast<std::uint64_t>(info_.ChunksI()) *
        (cx + static_cast<std::uint64_t>(info_.ChunksX()) * ct);
}

std::uint64_t LayoutV5Reader::PageOrdinal(std::uint32_t ci, std::uint32_t cx, std::uint32_t ct,
                                          std::uint32_t pi, std::uint32_t px, std::uint32_t pt) const {
    const std::uint64_t chunk = ChunkOrdinal(ci, cx, ct);
    const std::uint64_t within = static_cast<std::uint64_t>(pi) +
        static_cast<std::uint64_t>(info_.PagesI()) *
            (px + static_cast<std::uint64_t>(info_.PagesX()) * pt);
    return chunk * info_.PagesPerChunk() + within;
}

bool LayoutV5Reader::Open(const std::filesystem::path& path, std::string& errorMessage) {
    file_ = std::make_shared<std::ifstream>(path, std::ios::binary);
    if(!*file_) {
        errorMessage = "cannot open the v5 layout file.";
        return false;
    }
    std::vector<unsigned char> header(128, 0);
    file_->read(reinterpret_cast<char*>(header.data()), 128);
    if(!*file_ || std::memcmp(header.data(), kLayoutV5Magic, 8) != 0) {
        errorMessage = "the v5 layout magic is wrong.";
        return false;
    }
    std::size_t offset = 8;
    std::uint32_t version = 0;
    Consume(header, offset, version);
    if(version != kLayoutV5Version) {
        errorMessage = "the v5 layout version is not supported.";
        return false;
    }
    Consume(header, offset, info_.codec);
    Consume(header, offset, info_.spec.chunkInline);
    Consume(header, offset, info_.spec.chunkXline);
    Consume(header, offset, info_.spec.chunkSample);
    Consume(header, offset, info_.spec.pageInline);
    Consume(header, offset, info_.spec.pageXline);
    Consume(header, offset, info_.spec.pageSample);
    Consume(header, offset, info_.volumeInlines);
    Consume(header, offset, info_.volumeXlines);
    Consume(header, offset, info_.volumeSamples);
    Consume(header, offset, info_.inlineMin);
    Consume(header, offset, info_.xlineMin);
    Consume(header, offset, info_.sourceIdentityHash);
    std::uint64_t chunkCount = 0;
    std::uint64_t pageCount = 0;
    Consume(header, offset, chunkCount);
    Consume(header, offset, pageCount);
    if(chunkCount != info_.ChunkCount() || pageCount != info_.PageCount()) {
        errorMessage = "the v5 layout directory sizes disagree with the header.";
        return false;
    }
    const std::uint64_t chunkDirBytes = chunkCount * 12;
    const std::uint64_t pageDirBytes = pageCount * 32;
    dataOffset_ = 128 + chunkDirBytes + pageDirBytes;
    std::vector<unsigned char> chunkDir(static_cast<std::size_t>(chunkDirBytes), 0);
    std::vector<unsigned char> pageDir(static_cast<std::size_t>(pageDirBytes), 0);
    file_->read(reinterpret_cast<char*>(chunkDir.data()), static_cast<std::streamsize>(chunkDir.size()));
    file_->read(reinterpret_cast<char*>(pageDir.data()), static_cast<std::streamsize>(pageDir.size()));
    if(!*file_) {
        errorMessage = "the v5 layout directory is truncated.";
        return false;
    }
    chunkPageStart_.resize(static_cast<std::size_t>(chunkCount));
    for(std::size_t c = 0; c < chunkPageStart_.size(); ++c) {
        std::memcpy(&chunkPageStart_[c], chunkDir.data() + c * 12, 8);
    }
    pages_.resize(static_cast<std::size_t>(pageCount));
    for(std::size_t p = 0; p < pages_.size(); ++p) {
        PageEntry& entry = pages_[p];
        std::memcpy(&entry.offset, pageDir.data() + p * 32, 8);
        std::memcpy(&entry.storedBytes, pageDir.data() + p * 32 + 8, 4);
        std::memcpy(&entry.rawBytes, pageDir.data() + p * 32 + 12, 4);
        std::memcpy(&entry.checksum, pageDir.data() + p * 32 + 16, 8);
        std::memcpy(&entry.codec, pageDir.data() + p * 32 + 24, 4);
    }
    return true;
}

bool LayoutV5Reader::ReadPageRaw(std::uint64_t pageIndex, std::vector<unsigned char>& out,
                                 std::string& errorMessage) {
    if(pageIndex >= pages_.size()) {
        errorMessage = "the page index is out of range.";
        return false;
    }
    const PageEntry& entry = pages_[pageIndex];
    out.resize(entry.storedBytes);
    file_->clear();
    file_->seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    file_->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if(!*file_) {
        errorMessage = "a v5 page read failed.";
        return false;
    }
    ++stats_.readCalls;
    stats_.readBytes += entry.storedBytes;
    stats_.readRanges += 1;
    ++stats_.pagesTouched;
    const std::uint64_t crc = Fnv1a(out.data(), out.size());
    if(crc != entry.checksum) {
        ++stats_.checksumFailures;
        errorMessage = "a v5 page checksum failed.";
        return false;
    }
    return true;
}

bool LayoutV5Reader::ReadPage(std::uint64_t pageIndex, std::vector<float>& out, std::string& errorMessage) {
    std::vector<unsigned char> stored;
    if(!ReadPageRaw(pageIndex, stored, errorMessage)) {
        return false;
    }
    const PageEntry& entry = pages_[pageIndex];
    if(entry.codec == 0) {
        out.resize(entry.rawBytes / 4);
        std::memcpy(out.data(), stored.data(), entry.rawBytes);
        return true;
    }
#ifdef SEISMIC_HAVE_ZSTD
    out.resize(entry.rawBytes / 4);
    const std::size_t written = ZSTD_decompress(out.data(), entry.rawBytes, stored.data(), stored.size());
    if(ZSTD_isError(written) || written != entry.rawBytes) {
        ++stats_.checksumFailures;
        errorMessage = "a v5 zstd page failed to decompress.";
        return false;
    }
    return true;
#else
    errorMessage = "this build has no zstd support for v5 pages.";
    return false;
#endif
}

bool LayoutV5Reader::ReadPageBytes(std::uint64_t pageIndex, std::uint64_t byteOffset, std::uint64_t byteLength,
                                   std::vector<unsigned char>& out, std::string& errorMessage) {
    if(pageIndex >= pages_.size()) {
        errorMessage = "the page index is out of range.";
        return false;
    }
    const PageEntry& entry = pages_[pageIndex];
    if(entry.codec != 0) {
        errorMessage = "raw partial reads need raw pages.";
        return false;
    }
    if(byteOffset + byteLength > entry.storedBytes) {
        errorMessage = "the partial read exceeds the page.";
        return false;
    }
    out.resize(static_cast<std::size_t>(byteLength));
    file_->clear();
    file_->seekg(static_cast<std::streamoff>(entry.offset + byteOffset), std::ios::beg);
    file_->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(byteLength));
    if(!*file_) {
        errorMessage = "a v5 partial page read failed.";
        return false;
    }
    ++stats_.readCalls;
    stats_.readBytes += byteLength;
    stats_.readRanges += 1;
    ++stats_.pagesTouched;
    ++stats_.partialPageReads;
    return true;
}

bool LayoutV5Reader::ReadBox(int inlineIndex, int xlineIndex, int sampleIndex,
                             int inlines, int xlines, int samples,
                             std::vector<float>& out, std::string& errorMessage) {
    const auto start = std::chrono::steady_clock::now();
    const LayoutV5Info& info = info_;
    if(inlineIndex < 0 || xlineIndex < 0 || sampleIndex < 0 || inlines <= 0 || xlines <= 0 || samples <= 0 ||
       inlineIndex + inlines > static_cast<int>(info.volumeInlines) ||
       xlineIndex + xlines > static_cast<int>(info.volumeXlines) ||
       sampleIndex + samples > static_cast<int>(info.volumeSamples)) {
        errorMessage = "the requested box is outside the volume.";
        return false;
    }
    out.assign(static_cast<std::size_t>(inlines) * xlines * samples,
               std::numeric_limits<float>::quiet_NaN());
    stats_.outputBytes += static_cast<std::uint64_t>(out.size()) * 4;

    const std::uint32_t ci0 = static_cast<std::uint32_t>(inlineIndex) / info.spec.chunkInline;
    const std::uint32_t ci1 = static_cast<std::uint32_t>(inlineIndex + inlines - 1) / info.spec.chunkInline;
    const std::uint32_t cx0 = static_cast<std::uint32_t>(xlineIndex) / info.spec.chunkXline;
    const std::uint32_t cx1 = static_cast<std::uint32_t>(xlineIndex + xlines - 1) / info.spec.chunkXline;
    const std::uint32_t ct0 = static_cast<std::uint32_t>(sampleIndex) / info.spec.chunkSample;
    const std::uint32_t ct1 = static_cast<std::uint32_t>(sampleIndex + samples - 1) / info.spec.chunkSample;

    struct Needed {
        std::uint64_t page = 0;
        std::uint32_t ci = 0, cx = 0, ct = 0, pi = 0, px = 0, pt = 0;
    };
    std::vector<Needed> needed;
    std::set<std::uint64_t> chunkSet;
    for(std::uint32_t ct = ct0; ct <= ct1; ++ct) {
        for(std::uint32_t ci = ci0; ci <= ci1; ++ci) {
            for(std::uint32_t cx = cx0; cx <= cx1; ++cx) {
                chunkSet.insert(ChunkOrdinal(ci, cx, ct));
                for(std::uint32_t pi = 0; pi < info.PagesI(); ++pi) {
                    const std::uint32_t i0 = ci * info.spec.chunkInline + pi * info.spec.pageInline;
                    if(i0 >= static_cast<std::uint32_t>(inlineIndex + inlines) ||
                       i0 + info.spec.pageInline <= static_cast<std::uint32_t>(inlineIndex)) {
                        continue;
                    }
                    for(std::uint32_t px = 0; px < info.PagesX(); ++px) {
                        const std::uint32_t x0 = cx * info.spec.chunkXline + px * info.spec.pageXline;
                        if(x0 >= static_cast<std::uint32_t>(xlineIndex + xlines) ||
                           x0 + info.spec.pageXline <= static_cast<std::uint32_t>(xlineIndex)) {
                            continue;
                        }
                        for(std::uint32_t pt = 0; pt < info.PagesT(); ++pt) {
                            const std::uint32_t t0 = ct * info.spec.chunkSample + pt * info.spec.pageSample;
                            if(t0 >= static_cast<std::uint32_t>(sampleIndex + samples) ||
                               t0 + info.spec.pageSample <= static_cast<std::uint32_t>(sampleIndex)) {
                                continue;
                            }
                            Needed item;
                            item.page = PageOrdinal(ci, cx, ct, pi, px, pt);
                            item.ci = ci;
                            item.cx = cx;
                            item.ct = ct;
                            item.pi = pi;
                            item.px = px;
                            item.pt = pt;
                            needed.push_back(item);
                        }
                    }
                }
            }
        }
    }
    stats_.chunksTouched += chunkSet.size();

    std::sort(needed.begin(), needed.end(),
              [this](const Needed& a, const Needed& b) { return pages_[a.page].offset < pages_[b.page].offset; });

    // Merge adjacent pages into physical ranges and read each range once.
    std::vector<float> pageValues;
    std::vector<unsigned char> rangeBytes;
    std::size_t index = 0;
    while(index < needed.size()) {
        std::size_t end = index + 1;
        const std::uint64_t rangeStart = pages_[needed[index].page].offset;
        std::uint64_t rangeEnd = rangeStart + pages_[needed[index].page].storedBytes;
        while(end < needed.size() && pages_[needed[end].page].offset == rangeEnd) {
            rangeEnd += pages_[needed[end].page].storedBytes;
            ++end;
        }
        rangeBytes.resize(static_cast<std::size_t>(rangeEnd - rangeStart));
        file_->clear();
        file_->seekg(static_cast<std::streamoff>(rangeStart), std::ios::beg);
        file_->read(reinterpret_cast<char*>(rangeBytes.data()), static_cast<std::streamsize>(rangeBytes.size()));
        if(!*file_) {
            errorMessage = "a v5 range read failed.";
            return false;
        }
        ++stats_.readCalls;
        stats_.readBytes += rangeBytes.size();
        stats_.readRanges += 1;
        for(std::size_t k = index; k < end; ++k) {
            const Needed& item = needed[k];
            const PageEntry& entry = pages_[item.page];
            ++stats_.pagesTouched;
            const unsigned char* payload = rangeBytes.data() + (entry.offset - rangeStart);
            const std::uint64_t crc = Fnv1a(payload, entry.storedBytes);
            if(crc != entry.checksum) {
                ++stats_.checksumFailures;
                errorMessage = "a v5 page checksum failed.";
                return false;
            }
            if(entry.codec == 0) {
                pageValues.resize(entry.rawBytes / 4);
                std::memcpy(pageValues.data(), payload, entry.rawBytes);
            } else {
#ifdef SEISMIC_HAVE_ZSTD
                pageValues.resize(entry.rawBytes / 4);
                const std::size_t written = ZSTD_decompress(pageValues.data(), entry.rawBytes,
                                                            payload, entry.storedBytes);
                if(ZSTD_isError(written) || written != entry.rawBytes) {
                    ++stats_.checksumFailures;
                    errorMessage = "a v5 zstd page failed to decompress.";
                    return false;
                }
#else
                errorMessage = "this build has no zstd support for v5 pages.";
                return false;
#endif
            }
            const std::uint32_t i0 = item.ci * info.spec.chunkInline + item.pi * info.spec.pageInline;
            const std::uint32_t x0 = item.cx * info.spec.chunkXline + item.px * info.spec.pageXline;
            const std::uint32_t t0 = item.ct * info.spec.chunkSample + item.pt * info.spec.pageSample;
            for(std::uint32_t s = 0; s < info.spec.pageSample; ++s) {
                const std::uint32_t t = t0 + s;
                if(t < static_cast<std::uint32_t>(sampleIndex) ||
                   t >= static_cast<std::uint32_t>(sampleIndex + samples)) {
                    continue;
                }
                for(std::uint32_t xx = 0; xx < info.spec.pageXline; ++xx) {
                    const std::uint32_t x = x0 + xx;
                    if(x < static_cast<std::uint32_t>(xlineIndex) ||
                       x >= static_cast<std::uint32_t>(xlineIndex + xlines)) {
                        continue;
                    }
                    for(std::uint32_t ii = 0; ii < info.spec.pageInline; ++ii) {
                        const std::uint32_t i = i0 + ii;
                        if(i < static_cast<std::uint32_t>(inlineIndex) ||
                           i >= static_cast<std::uint32_t>(inlineIndex + inlines)) {
                            continue;
                        }
                        const float value = pageValues[
                            (static_cast<std::size_t>(s) * info.spec.pageXline + xx) * info.spec.pageInline + ii];
                        const std::size_t outIndex =
                            (static_cast<std::size_t>(t - sampleIndex) * xlines + (x - xlineIndex)) * inlines +
                            (i - inlineIndex);
                        out[outIndex] = value;
                    }
                }
            }
        }
        index = end;
    }
    // Needed bytes inside touched pages (what a sub-page partial read could save).
    stats_.neededInPageBytes += static_cast<std::uint64_t>(inlines) * xlines * samples * 4;
    stats_.wallMs += MsSince(start);
    return true;
}

bool LayoutV5Reader::ReadTrace(int inlineIndex, int xlineIndex, std::vector<float>& out, std::string& errorMessage) {
    return ReadBox(inlineIndex, xlineIndex, 0, 1, 1, static_cast<int>(info_.volumeSamples), out, errorMessage);
}

bool LayoutV5Reader::ReadInline(int inlineIndex, std::vector<float>& out, std::string& errorMessage) {
    return ReadBox(inlineIndex, 0, 0, 1, static_cast<int>(info_.volumeXlines),
                   static_cast<int>(info_.volumeSamples), out, errorMessage);
}

bool LayoutV5Reader::ReadCrossline(int xlineIndex, std::vector<float>& out, std::string& errorMessage) {
    return ReadBox(0, xlineIndex, 0, static_cast<int>(info_.volumeInlines), 1,
                   static_cast<int>(info_.volumeSamples), out, errorMessage);
}

bool LayoutV5Reader::ReadTimeSlice(int sampleIndex, std::vector<float>& out, std::string& errorMessage) {
    return ReadBox(0, 0, sampleIndex, static_cast<int>(info_.volumeInlines),
                   static_cast<int>(info_.volumeXlines), 1, out, errorMessage);
}

// ------------------------------------------------- build from a workspace

bool BuildLayoutV5FromWorkspace(const std::filesystem::path& roiWorkspace,
                                const std::filesystem::path& outFile,
                                const LayoutV5Spec& spec,
                                std::uint32_t codec,
                                std::string& errorMessage,
                                std::uint64_t& sourceChunkReads,
                                LayoutV5Info* outInfo) {
    WorkspaceReader source;
    if(!source.Open(roiWorkspace, errorMessage)) {
        return false;
    }
    const WorkspaceInfo& src = source.Info();
    if(src.codec != 0) {
        errorMessage = "the v5 builder needs a raw source workspace.";
        return false;
    }
    LayoutV5Info info;
    info.spec = spec;
    info.volumeInlines = src.inlines;
    info.volumeXlines = src.xlines;
    info.volumeSamples = src.samples;
    info.inlineMin = src.inlineMin;
    info.xlineMin = src.xlineMin;
    info.sourceIdentityHash = src.sourceIdentityHash;
    info.codec = codec;

    LayoutV5Writer writer;
    if(!writer.Open(outFile, info, errorMessage)) {
        return false;
    }

    const std::uint32_t srcS = src.chunkSamples;
    const std::uint32_t srcI = src.chunkInlines;
    const std::uint32_t srcX = src.chunkXlines;
    struct CacheEntry {
        std::uint64_t key = 0;
        std::vector<float> values;
    };
    // std::deque keeps references to existing entries stable across push/pop,
    // so cached chunk pointers stay valid while a v5 chunk is assembled.
    std::deque<CacheEntry> cache;
    sourceChunkReads = 0;
    auto fetchChunk = [&](std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) -> const float* {
        const std::uint64_t key = (static_cast<std::uint64_t>(cs) * src.ChunksI() + ci) * src.ChunksX() + cx;
        for(const CacheEntry& entry : cache) {
            if(entry.key == key) {
                return entry.values.data();
            }
        }
        CacheEntry entry;
        entry.key = key;
        if(!source.ReadChunk(cs, ci, cx, entry.values, errorMessage)) {
            return nullptr;
        }
        ++sourceChunkReads;
        cache.push_back(std::move(entry));
        if(cache.size() > 64) {
            cache.pop_front();
        }
        return cache.back().values.data();
    };

    const std::uint32_t csPerChunk = (spec.chunkSample + srcS - 1) / srcS;
    const std::uint32_t csxPerChunk = (spec.chunkXline + srcX - 1) / srcX;
    std::vector<float> chunkValues(static_cast<std::size_t>(spec.chunkInline) * spec.chunkXline * spec.chunkSample);
    for(std::uint32_t ct = 0; ct < info.ChunksT(); ++ct) {
        for(std::uint32_t ci = 0; ci < info.ChunksI(); ++ci) {
            const std::uint32_t csi = (ci * spec.chunkInline) / srcI;
            for(std::uint32_t cx = 0; cx < info.ChunksX(); ++cx) {
                std::fill(chunkValues.begin(), chunkValues.end(), std::numeric_limits<float>::quiet_NaN());
                // The v5 chunk can start inside a source chunk, so the spanned
                // source range is computed from the real start, not from a
                // multiple of the candidate chunk size.
                const std::uint32_t baseCs = (ct * spec.chunkSample) / srcS;
                const std::uint32_t baseCsx = (cx * spec.chunkXline) / srcX;
                const std::uint32_t spanCs = ((ct * spec.chunkSample + spec.chunkSample - 1) / srcS) - baseCs + 1;
                const std::uint32_t spanCsx = ((cx * spec.chunkXline + spec.chunkXline - 1) / srcX) - baseCsx + 1;
                std::vector<const float*> rowPtrs(static_cast<std::size_t>(spanCs) * spanCsx, nullptr);
                for(std::uint32_t a = 0; a < spanCs; ++a) {
                    for(std::uint32_t b = 0; b < spanCsx; ++b) {
                        const std::uint32_t cs = baseCs + a;
                        const std::uint32_t csx = baseCsx + b;
                        if(cs >= src.ChunksS() || csx >= src.ChunksX()) {
                            continue;
                        }
                        const float* values = fetchChunk(cs, csi, csx);
                        if(values == nullptr) {
                            return false;
                        }
                        rowPtrs[static_cast<std::size_t>(a) * spanCsx + b] = values;
                    }
                }
                for(std::uint32_t t = 0; t < spec.chunkSample; ++t) {
                    const std::uint64_t vT = static_cast<std::uint64_t>(ct) * spec.chunkSample + t;
                    if(vT >= src.samples) {
                        break;
                    }
                    const std::uint32_t cs = static_cast<std::uint32_t>(vT / srcS);
                    const std::uint32_t s = static_cast<std::uint32_t>(vT % srcS);
                    for(std::uint32_t x = 0; x < spec.chunkXline; ++x) {
                        const std::uint64_t vX = static_cast<std::uint64_t>(cx) * spec.chunkXline + x;
                        if(vX >= src.xlines) {
                            break;
                        }
                        const std::uint32_t csx = static_cast<std::uint32_t>(vX / srcX);
                        const std::uint32_t lx = static_cast<std::uint32_t>(vX % srcX);
                        const float* srcChunk = rowPtrs[
                            static_cast<std::size_t>(cs - baseCs) * spanCsx + (csx - baseCsx)];
                        if(srcChunk == nullptr) {
                            continue;
                        }
                        for(std::uint32_t i = 0; i < spec.chunkInline; ++i) {
                            const std::uint64_t vI = static_cast<std::uint64_t>(ci) * spec.chunkInline + i;
                            if(vI >= src.inlines) {
                                break;
                            }
                            const std::uint32_t li = static_cast<std::uint32_t>(vI % srcI);
                            const std::size_t srcOffset =
                                (static_cast<std::size_t>(s) * srcI + li) * srcX + lx;
                            chunkValues[(static_cast<std::size_t>(t) * spec.chunkXline + x) * spec.chunkInline + i] =
                                srcChunk[srcOffset];
                        }
                    }
                }

                if(!writer.WriteChunk(ci, cx, ct, chunkValues.data(), errorMessage)) {
                    return false;
                }
            }
        }
    }
    if(outInfo != nullptr) {
        *outInfo = info;
    }
    return writer.Finalize(errorMessage);
}

} // namespace engine
} // namespace seismic
