#include "Engine/WorkspaceFormat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>

#ifdef SEISMIC_HAVE_ZSTD
#include <zstd.h>
#endif

namespace seismic {
namespace engine {
namespace {

void AppendU32(std::vector<unsigned char>& out, std::uint32_t value) {
    for(int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
    }
}

void AppendU64(std::vector<unsigned char>& out, std::uint64_t value) {
    for(int i = 0; i < 8; ++i) {
        out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
    }
}

void AppendI32(std::vector<unsigned char>& out, std::int32_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
}

bool ReadU32(const std::vector<unsigned char>& data, std::size_t& offset, std::uint32_t& value) {
    if(offset + 4 > data.size()) {
        return false;
    }
    value = 0;
    for(int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
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
        value |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    offset += 8;
    return true;
}

bool ReadI32(const std::vector<unsigned char>& data, std::size_t& offset, std::int32_t& value) {
    std::uint32_t raw = 0;
    if(!ReadU32(data, offset, raw)) {
        return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
}

bool ReadFile(const std::filesystem::path& path, std::vector<unsigned char>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if(!in) {
        error = "cannot open " + path.u8string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if(size < 0) {
        error = "cannot size " + path.u8string();
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if(size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), size);
        if(!in) {
            error = "cannot read " + path.u8string();
            return false;
        }
    }
    return true;
}

bool WriteFileAtomic(
    const std::filesystem::path& path,
    const std::vector<unsigned char>& data,
    std::string& error) {
    std::error_code dirEc;
    if(!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), dirEc);
    }
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if(!out) {
            error = "cannot write " + temp.u8string();
            return false;
        }
        if(!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        if(!out) {
            error = "cannot write " + temp.u8string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
    if(ec) {
        error = "cannot publish " + path.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

std::uint32_t WorkspaceInfo::ChunksS() const {
    return chunkSamples == 0 ? 0 : (samples + chunkSamples - 1) / chunkSamples;
}

std::uint32_t WorkspaceInfo::ChunksI() const {
    return chunkInlines == 0 ? 0 : (inlines + chunkInlines - 1) / chunkInlines;
}

std::uint32_t WorkspaceInfo::ChunksX() const {
    return chunkXlines == 0 ? 0 : (xlines + chunkXlines - 1) / chunkXlines;
}

std::uint64_t WorkspaceInfo::ChunkCount() const {
    return static_cast<std::uint64_t>(ChunksS()) * ChunksI() * ChunksX();
}

std::uint64_t WorkspaceInfo::ChunkBytes() const {
    return static_cast<std::uint64_t>(chunkSamples) * chunkInlines * chunkXlines * sizeof(float);
}

std::uint64_t WorkspaceInfo::ShardCount() const {
    if(chunksPerShard == 0) {
        return 0;
    }
    return (ChunkCount() + chunksPerShard - 1) / chunksPerShard;
}

std::uint64_t WorkspaceInfo::VolumeBytes() const {
    return static_cast<std::uint64_t>(samples) * inlines * xlines * sizeof(float);
}

std::filesystem::path WorkspaceMetaPath(const std::filesystem::path& basePath) {
    return std::filesystem::path(basePath.string() + ".sf3c.meta");
}

std::filesystem::path WorkspaceShardPath(const std::filesystem::path& basePath, std::uint32_t shardIndex) {
    char suffix[16] = {};
    std::snprintf(suffix, sizeof(suffix), ".sf3.s%03u", shardIndex);
    return std::filesystem::path(basePath.string() + suffix);
}

std::uint64_t WorkspaceChecksum(const void* data, std::size_t bytes) {
    const unsigned char* bytesPtr = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for(std::size_t i = 0; i < bytes; ++i) {
        hash ^= bytesPtr[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

WorkspaceWriter::~WorkspaceWriter() {
    if(openShardFile_ != nullptr) {
        std::ofstream* shard = static_cast<std::ofstream*>(openShardFile_);
        shard->close();
        delete shard;
        openShardFile_ = nullptr;
    }
}

bool WorkspaceWriter::Open(
    const std::filesystem::path& basePath,
    const WorkspaceInfo& info,
    bool resumeExisting,
    std::string& errorMessage) {
    if(info.samples == 0 || info.inlines == 0 || info.xlines == 0 ||
       info.chunkSamples == 0 || info.chunkInlines == 0 || info.chunkXlines == 0 ||
       info.chunksPerShard == 0) {
        errorMessage = "workspace layout is incomplete";
        return false;
    }
    if(info.codec != kCodecRaw
#ifdef SEISMIC_HAVE_ZSTD
       && info.codec != kCodecZstd
#endif
    ) {
        errorMessage = info.codec == kCodecZstd
            ? "this build has no zstd support (rebuild with -DSEISMIC_ENABLE_ZSTD=ON)"
            : "unknown workspace codec";
        return false;
    }
    basePath_ = basePath;
    info_ = info;
    if(resumeExisting) {
        WorkspaceReader existing;
        if(!existing.Open(basePath, errorMessage)) {
            return false;
        }
        const WorkspaceInfo& existingInfo = existing.Info();
        if(existingInfo.samples != info.samples || existingInfo.inlines != info.inlines ||
           existingInfo.xlines != info.xlines || existingInfo.chunkSamples != info.chunkSamples ||
           existingInfo.chunkInlines != info.chunkInlines || existingInfo.chunkXlines != info.chunkXlines ||
           existingInfo.chunksPerShard != info.chunksPerShard || existingInfo.codec != info.codec ||
           existingInfo.lodLevel != info.lodLevel || existingInfo.lodMethod != info.lodMethod ||
           existingInfo.lodFactorSamples != info.lodFactorSamples ||
           existingInfo.lodFactorInlines != info.lodFactorInlines ||
           existingInfo.lodFactorXlines != info.lodFactorXlines ||
           existingInfo.lodAlgorithmVersion != info.lodAlgorithmVersion) {
            errorMessage = "the existing workspace layout does not match";
            return false;
        }
        info_ = existingInfo;
        entries_ = existing.Entries();
        completion_ = existing.Completion();
    } else {
        // Fresh workspace: never append to shard files from an earlier run.
        std::error_code ec;
        for(std::uint32_t shard = 0; shard < info.ShardCount(); ++shard) {
            std::filesystem::remove(WorkspaceShardPath(basePath_, shard), ec);
        }
        std::filesystem::remove(WorkspaceMetaPath(basePath_), ec);
        entries_.assign(static_cast<std::size_t>(info.ChunkCount()), WorkspaceChunkEntry{});
        completion_.assign(static_cast<std::size_t>((info.ChunkCount() + 7) / 8), 0);
    }
    created_ = true;
    return WriteMeta(errorMessage);
}

bool WorkspaceWriter::HasChunk(std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const {
    const std::uint64_t ordinal = ChunkOrdinal(cs, ci, cx);
    if(ordinal >= completion_.size() * 8) {
        return false;
    }
    return (completion_[static_cast<std::size_t>(ordinal / 8)] & (1u << (ordinal % 8))) != 0;
}

std::uint64_t WorkspaceWriter::ChunkOrdinal(
    std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const {
    return (static_cast<std::uint64_t>(cs) * info_.ChunksI() + ci) * info_.ChunksX() + cx;
}

bool WorkspaceWriter::WriteChunk(
    std::uint32_t cs, std::uint32_t ci, std::uint32_t cx,
    const float* values, std::size_t valueCount, std::string& errorMessage) {
    if(!created_) {
        errorMessage = "the writer was not created";
        return false;
    }
    const std::uint64_t ordinal = ChunkOrdinal(cs, ci, cx);
    if(ordinal >= entries_.size()) {
        errorMessage = "chunk coordinates are outside the workspace";
        return false;
    }
    const std::uint64_t expected = info_.ChunkBytes() / sizeof(float);
    if(valueCount != expected) {
        errorMessage = "chunk value count does not match the layout";
        return false;
    }
    const std::uint32_t shardIndex = static_cast<std::uint32_t>(ordinal / info_.chunksPerShard);
    if(openShardFile_ == nullptr || openShard_ != shardIndex) {
        if(!OpenShard(shardIndex, errorMessage)) {
            return false;
        }
    }
    std::ofstream* shard = static_cast<std::ofstream*>(openShardFile_);
    const std::uint64_t bytes = info_.ChunkBytes();
    const void* payload = values;
    std::size_t payloadBytes = static_cast<std::size_t>(bytes);
    std::vector<unsigned char> compressed;
    if(info_.codec == kCodecZstd) {
#ifdef SEISMIC_HAVE_ZSTD
        compressed.resize(ZSTD_compressBound(static_cast<std::size_t>(bytes)));
        const std::size_t written = ZSTD_compress(
            compressed.data(), compressed.size(), values, static_cast<std::size_t>(bytes),
            static_cast<int>(std::max(1u, info_.codecLevel)));
        if(ZSTD_isError(written)) {
            errorMessage = std::string("zstd compress failed: ") + ZSTD_getErrorName(written);
            return false;
        }
        compressed.resize(written);
        payload = compressed.data();
        payloadBytes = written;
#else
        errorMessage = "this build has no zstd support";
        return false;
#endif
    }
    bool flushBatch = false;
    if(writeBatchBytes_ > 0) {
        if(writeBatch_.size() + payloadBytes > writeBatchBytes_ && !writeBatch_.empty()) {
            flushBatch = true;
        }
    }
    if(flushBatch) {
        shard->write(reinterpret_cast<const char*>(writeBatch_.data()),
                     static_cast<std::streamsize>(writeBatch_.size()));
        ++writeCalls_;
        if(!*shard) {
            errorMessage = "shard write failed";
            return false;
        }
        writeBatch_.clear();
    }
    if(writeBatchBytes_ > 0) {
        const unsigned char* raw = static_cast<const unsigned char*>(payload);
        writeBatch_.insert(writeBatch_.end(), raw, raw + payloadBytes);
        if(writeBatch_.size() >= writeBatchBytes_) {
            shard->write(reinterpret_cast<const char*>(writeBatch_.data()),
                         static_cast<std::streamsize>(writeBatch_.size()));
            ++writeCalls_;
            if(!*shard) {
                errorMessage = "shard write failed";
                return false;
            }
            writeBatch_.clear();
        }
    } else {
        shard->write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(payloadBytes));
        ++writeCalls_;
        if(!*shard) {
            errorMessage = "shard write failed";
            return false;
        }
    }
    WorkspaceChunkEntry& entry = entries_[static_cast<std::size_t>(ordinal)];
    entry.shardIndex = shardIndex;
    entry.offsetInShard = openShardOffset_;
    entry.storedBytes = payloadBytes;
    entry.checksum = WorkspaceChecksum(payload, payloadBytes);
    openShardOffset_ += payloadBytes;
    completion_[static_cast<std::size_t>(ordinal / 8)] |= static_cast<unsigned char>(1u << (ordinal % 8));
    return true;
}

bool WorkspaceWriter::OpenShard(std::uint32_t shardIndex, std::string& errorMessage) {
    if(openShardFile_ != nullptr) {
        std::ofstream* previous = static_cast<std::ofstream*>(openShardFile_);
        if(!writeBatch_.empty()) {
            previous->write(reinterpret_cast<const char*>(writeBatch_.data()),
                            static_cast<std::streamsize>(writeBatch_.size()));
            ++writeCalls_;
            writeBatch_.clear();
            if(!*previous) {
                previous->close();
                delete previous;
                openShardFile_ = nullptr;
                errorMessage = "shard write failed";
                return false;
            }
        }
        previous->close();
        delete previous;
        openShardFile_ = nullptr;
    }
    const std::filesystem::path shardPath = WorkspaceShardPath(basePath_, shardIndex);
    std::ofstream* shard = new std::ofstream(shardPath, std::ios::binary | std::ios::app);
    if(!*shard) {
        delete shard;
        errorMessage = "cannot open shard " + shardPath.u8string();
        return false;
    }
    openShard_ = shardIndex;
    shard->seekp(0, std::ios::end);
    openShardOffset_ = static_cast<std::uint64_t>(shard->tellp());
    openShardFile_ = shard;
    return true;
}

bool WorkspaceWriter::WriteMeta(std::string& errorMessage) {
    std::vector<unsigned char> meta;
    meta.insert(meta.end(), kWorkspaceMagic, kWorkspaceMagic + 8);
    AppendU32(meta, kWorkspaceFormatVersion);
    AppendU32(meta, info_.samples);
    AppendU32(meta, info_.inlines);
    AppendU32(meta, info_.xlines);
    AppendU32(meta, info_.chunkSamples);
    AppendU32(meta, info_.chunkInlines);
    AppendU32(meta, info_.chunkXlines);
    AppendU32(meta, info_.chunksPerShard);
    AppendU32(meta, info_.codec);
    AppendU32(meta, info_.codecLevel);
    AppendU32(meta, info_.sampleIntervalUs);
    AppendI32(meta, info_.inlineMin);
    AppendI32(meta, info_.xlineMin);
    AppendU64(meta, info_.sourceIdentityHash);
    AppendU32(meta, info_.algorithmVersion);
    AppendU32(meta, info_.lodLevel);
    AppendU32(meta, info_.lodMethod);
    AppendU32(meta, info_.lodFactorSamples);
    AppendU32(meta, info_.lodFactorInlines);
    AppendU32(meta, info_.lodFactorXlines);
    AppendU32(meta, info_.lodAlgorithmVersion);
    AppendU64(meta, info_.lodSourceHash);
    AppendU64(meta, info_.ChunkCount());
    AppendU32(meta, static_cast<std::uint32_t>(info_.ShardCount()));
    AppendU32(meta, static_cast<std::uint32_t>(completion_.size()));
    meta.insert(meta.end(), completion_.begin(), completion_.end());
    for(const WorkspaceChunkEntry& entry : entries_) {
        AppendU32(meta, entry.shardIndex);
        AppendU64(meta, entry.offsetInShard);
        AppendU64(meta, entry.storedBytes);
        AppendU64(meta, entry.checksum);
    }
    return WriteFileAtomic(WorkspaceMetaPath(basePath_), meta, errorMessage);
}

bool WorkspaceWriter::Finalize(std::string& errorMessage) {
    if(openShardFile_ != nullptr) {
        std::ofstream* shard = static_cast<std::ofstream*>(openShardFile_);
        if(!writeBatch_.empty()) {
            shard->write(reinterpret_cast<const char*>(writeBatch_.data()),
                         static_cast<std::streamsize>(writeBatch_.size()));
            ++writeCalls_;
            writeBatch_.clear();
        }
        shard->close();
        delete shard;
        openShardFile_ = nullptr;
    }
    if(!WriteMeta(errorMessage)) {
        return false;
    }
    created_ = false;
    return true;
}

std::uint64_t WorkspaceReader::ChunkOrdinal(
    std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const {
    return (static_cast<std::uint64_t>(cs) * info_.ChunksI() + ci) * info_.ChunksX() + cx;
}

bool WorkspaceReader::Open(const std::filesystem::path& basePath, std::string& errorMessage) {
    std::vector<unsigned char> meta;
    if(!ReadFile(WorkspaceMetaPath(basePath), meta, errorMessage)) {
        return false;
    }
    if(meta.size() < 8 || std::memcmp(meta.data(), kWorkspaceMagic, 8) != 0) {
        errorMessage = "workspace metadata magic is wrong";
        return false;
    }
    std::size_t offset = 8;
    *this = WorkspaceReader{};
    basePath_ = basePath;
    std::uint32_t version = 0;
    if(!ReadU32(meta, offset, version)) {
        errorMessage = "workspace metadata is truncated";
        return false;
    }
    if(version != kWorkspaceFormatVersion) {
        errorMessage = "workspace format version is not supported";
        return false;
    }
    info_.version = version;
    {
        // Cache identity: path + metadata size + mtime + LOD level, so a rebuilt
        // workspace never serves stale cached chunks.
        std::error_code metaEc;
        const std::filesystem::path metaPath = WorkspaceMetaPath(basePath);
        const std::uintmax_t metaSize = std::filesystem::file_size(metaPath, metaEc);
        const std::int64_t metaTime = metaEc ? 0 : static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::filesystem::last_write_time(metaPath, metaEc).time_since_epoch()).count());
        cacheSourceKey_ = WorkspaceChecksum(basePath.u8string().data(), basePath.u8string().size());
        cacheSourceKey_ ^= static_cast<std::uint64_t>(metaSize) * 1099511628211ull;
        cacheSourceKey_ ^= static_cast<std::uint64_t>(metaTime) * 1469598103934665603ull;
    }
    std::uint32_t chunkCount32 = 0;
    std::uint32_t shardCount32 = 0;
    std::uint32_t completionBytes = 0;
    if(!ReadU32(meta, offset, info_.samples) ||
       !ReadU32(meta, offset, info_.inlines) ||
       !ReadU32(meta, offset, info_.xlines) ||
       !ReadU32(meta, offset, info_.chunkSamples) ||
       !ReadU32(meta, offset, info_.chunkInlines) ||
       !ReadU32(meta, offset, info_.chunkXlines) ||
       !ReadU32(meta, offset, info_.chunksPerShard) ||
       !ReadU32(meta, offset, info_.codec) ||
       !ReadU32(meta, offset, info_.codecLevel) ||
       !ReadU32(meta, offset, info_.sampleIntervalUs) ||
       !ReadI32(meta, offset, info_.inlineMin) ||
       !ReadI32(meta, offset, info_.xlineMin) ||
       !ReadU64(meta, offset, info_.sourceIdentityHash) ||
       !ReadU32(meta, offset, info_.algorithmVersion) ||
       !ReadU32(meta, offset, info_.lodLevel) ||
       !ReadU32(meta, offset, info_.lodMethod) ||
       !ReadU32(meta, offset, info_.lodFactorSamples) ||
       !ReadU32(meta, offset, info_.lodFactorInlines) ||
       !ReadU32(meta, offset, info_.lodFactorXlines) ||
       !ReadU32(meta, offset, info_.lodAlgorithmVersion) ||
       !ReadU64(meta, offset, info_.lodSourceHash)) {
        errorMessage = "workspace metadata is truncated";
        return false;
    }
    std::uint64_t chunkCount = 0;
    if(!ReadU64(meta, offset, chunkCount) ||
       !ReadU32(meta, offset, shardCount32) ||
       !ReadU32(meta, offset, completionBytes)) {
        errorMessage = "workspace metadata is truncated";
        return false;
    }
    (void)chunkCount32;
    (void)shardCount32;
    if(offset + completionBytes > meta.size()) {
        errorMessage = "workspace completion map is truncated";
        return false;
    }
    completion_.assign(meta.begin() + static_cast<std::ptrdiff_t>(offset),
                       meta.begin() + static_cast<std::ptrdiff_t>(offset + completionBytes));
    offset += completionBytes;
    entries_.assign(static_cast<std::size_t>(chunkCount), WorkspaceChunkEntry{});
    for(std::size_t i = 0; i < entries_.size(); ++i) {
        WorkspaceChunkEntry& entry = entries_[i];
        if(!ReadU32(meta, offset, entry.shardIndex) ||
           !ReadU64(meta, offset, entry.offsetInShard) ||
           !ReadU64(meta, offset, entry.storedBytes) ||
           !ReadU64(meta, offset, entry.checksum)) {
            errorMessage = "workspace chunk table is truncated";
            return false;
        }
    }
    return true;
}

bool WorkspaceReader::HasChunk(std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const {
    const std::uint64_t ordinal = ChunkOrdinal(cs, ci, cx);
    if(ordinal >= entries_.size() || ordinal >= completion_.size() * 8) {
        return false;
    }
    return (completion_[static_cast<std::size_t>(ordinal / 8)] & (1u << (ordinal % 8))) != 0;
}

bool WorkspaceReader::ReadChunk(
    std::uint32_t cs, std::uint32_t ci, std::uint32_t cx,
    std::vector<float>& out, std::string& errorMessage) {
    const std::uint64_t ordinal = ChunkOrdinal(cs, ci, cx);
    if(ordinal >= entries_.size()) {
        errorMessage = "chunk coordinates are outside the workspace";
        return false;
    }
    if(!HasChunk(cs, ci, cx)) {
        errorMessage = "chunk is not present in the completion map";
        return false;
    }
    const WorkspaceChunkEntry& entry = entries_[static_cast<std::size_t>(ordinal)];
    if(chunkCache_) {
        const ChunkCacheKey cacheKey{cacheSourceKey_, info_.lodLevel, cs, ci, cx};
        if(std::shared_ptr<const std::vector<float>> cached = chunkCache_->Find(cacheKey)) {
            out = *cached;
            return true;
        }
    }
    // Reuse one shard stream for consecutive chunks in the same shard.
    if(!openShard_ || openShardIndex_ != entry.shardIndex) {
        openShard_.reset();
        const std::filesystem::path shardPath = WorkspaceShardPath(basePath_, entry.shardIndex);
        auto stream = std::make_shared<std::ifstream>(shardPath, std::ios::binary);
        if(!*stream) {
            errorMessage = "cannot open the shard file";
            return false;
        }
        openShard_ = std::move(stream);
        openShardIndex_ = entry.shardIndex;
    }
    std::ifstream& shard = *openShard_;
    shard.clear();
    shard.seekg(static_cast<std::streamoff>(entry.offsetInShard), std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(entry.storedBytes));
    shard.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if(!shard) {
        errorMessage = "cannot read the chunk payload";
        return false;
    }
    if(WorkspaceChecksum(bytes.data(), bytes.size()) != entry.checksum) {
        ++checksumFailures_;
        errorMessage = "chunk checksum mismatch";
        return false;
    }
    if(info_.codec == kCodecZstd) {
#ifdef SEISMIC_HAVE_ZSTD
        std::vector<float> decompressed(static_cast<std::size_t>(info_.ChunkBytes() / sizeof(float)));
        const std::size_t written = ZSTD_decompress(
            decompressed.data(), info_.ChunkBytes(), bytes.data(), bytes.size());
        if(ZSTD_isError(written) || written != info_.ChunkBytes()) {
            errorMessage = "zstd decompress failed";
            return false;
        }
        out = std::move(decompressed);
#else
        errorMessage = "this build has no zstd support";
        return false;
#endif
    } else {
        out.resize(bytes.size() / sizeof(float));
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    ++chunksRead_;
    bytesRead_ += bytes.size();
    if(chunkCache_) {
        chunkCache_->Insert(ChunkCacheKey{cacheSourceKey_, info_.lodLevel, cs, ci, cx},
                            std::make_shared<const std::vector<float>>(out));
    }
    return true;
}

bool WorkspaceReader::ReadTrace(int inlineNo, int xlineNo, std::vector<float>& out, std::string& errorMessage) {
    const std::int64_t il = inlineNo - info_.inlineMin;
    const std::int64_t xl = xlineNo - info_.xlineMin;
    if(il < 0 || xl < 0 || il >= info_.inlines || xl >= info_.xlines) {
        errorMessage = "trace coordinates are outside the volume";
        return false;
    }

    out.assign(info_.samples, std::numeric_limits<float>::quiet_NaN());
    const std::uint32_t ci = static_cast<std::uint32_t>(il / info_.chunkInlines);
    const std::uint32_t cxi = static_cast<std::uint32_t>(xl / info_.chunkXlines);
    const std::uint32_t localIl = static_cast<std::uint32_t>(il % info_.chunkInlines);
    const std::uint32_t localXl = static_cast<std::uint32_t>(xl % info_.chunkXlines);
    std::vector<float> chunk;
    for(std::uint32_t c = 0; c < info_.ChunksS(); ++c) {
        if(!ReadChunk(c, ci, cxi, chunk, errorMessage)) {
            return false;
        }
        for(std::uint32_t s = 0; s < info_.chunkSamples; ++s) {
            const std::uint64_t globalSample = static_cast<std::uint64_t>(c) * info_.chunkSamples + s;
            if(globalSample >= info_.samples) {
                break;
            }
            const std::size_t chunkOffset =
                (static_cast<std::size_t>(s) * info_.chunkInlines + localIl) * info_.chunkXlines + localXl;
            out[static_cast<std::size_t>(globalSample)] = chunk[chunkOffset];
        }
    }
    return true;
}

namespace {

constexpr std::uint32_t kSliceKindInline = 0;
constexpr std::uint32_t kSliceKindTime = 2;

} // namespace

bool WorkspaceReader::ReadInlineSlice(int inlineNo, std::vector<float>& out, std::string& errorMessage) {
    const SliceCacheKey sliceKey{cacheSourceKey_, info_.lodLevel, kSliceKindInline, inlineNo};
    if(sliceCache_) {
        if(std::shared_ptr<const Slice2D> cached = sliceCache_->Find(sliceKey)) {
            out = cached->values;
            return true;
        }
    }
    const std::int64_t il = inlineNo - info_.inlineMin;
    if(il < 0 || il >= info_.inlines) {
        errorMessage = "inline is outside the volume";
        return false;
    }
    out.assign(static_cast<std::size_t>(info_.xlines) * info_.samples,
               std::numeric_limits<float>::quiet_NaN());
    const std::uint32_t ci = static_cast<std::uint32_t>(il / info_.chunkInlines);
    const std::uint32_t localIl = static_cast<std::uint32_t>(il % info_.chunkInlines);
    std::vector<float> chunk;
    for(std::uint32_t c = 0; c < info_.ChunksS(); ++c) {
        for(std::uint32_t cxi = 0; cxi < info_.ChunksX(); ++cxi) {
            if(!ReadChunk(c, ci, cxi, chunk, errorMessage)) {
                return false;
            }
            for(std::uint32_t s = 0; s < info_.chunkSamples; ++s) {
                const std::uint64_t globalSample = static_cast<std::uint64_t>(c) * info_.chunkSamples + s;
                if(globalSample >= info_.samples) {
                    break;
                }
                for(std::uint32_t localXl = 0; localXl < info_.chunkXlines; ++localXl) {
                    const std::uint64_t globalXl = static_cast<std::uint64_t>(cxi) * info_.chunkXlines + localXl;
                    if(globalXl >= info_.xlines) {
                        break;
                    }
                    const std::size_t chunkOffset =
                        (static_cast<std::size_t>(s) * info_.chunkInlines + localIl) * info_.chunkXlines + localXl;
                    // SEG-Y display convention: row = samples-1-sample,
                    // column = xline index.
                    const std::size_t row = static_cast<std::size_t>(info_.samples - 1 - globalSample);
                    out[row * info_.xlines + static_cast<std::size_t>(globalXl)] = chunk[chunkOffset];
                }
            }
        }
    }
    if(sliceCache_) {
        Slice2D image;
        image.width = static_cast<int>(info_.xlines);
        image.height = static_cast<int>(info_.samples);
        image.values = out;
        sliceCache_->Insert(sliceKey, std::make_shared<const Slice2D>(std::move(image)));
    }
    return true;
}

bool WorkspaceReader::ReadTimeSlice(int sampleIndex, std::vector<float>& out, std::string& errorMessage) {
    const SliceCacheKey sliceKey{cacheSourceKey_, info_.lodLevel, kSliceKindTime, sampleIndex};
    if(sliceCache_) {
        if(std::shared_ptr<const Slice2D> cached = sliceCache_->Find(sliceKey)) {
            out = cached->values;
            return true;
        }
    }
    if(sampleIndex < 0 || sampleIndex >= static_cast<int>(info_.samples)) {
        errorMessage = "sample index is outside the volume";
        return false;
    }
    out.assign(static_cast<std::size_t>(info_.inlines) * info_.xlines,
               std::numeric_limits<float>::quiet_NaN());
    const std::uint32_t cs = static_cast<std::uint32_t>(sampleIndex) / info_.chunkSamples;
    const std::uint32_t localS = static_cast<std::uint32_t>(sampleIndex) % info_.chunkSamples;
    std::vector<float> chunk;
    for(std::uint32_t ci = 0; ci < info_.ChunksI(); ++ci) {
        for(std::uint32_t cxi = 0; cxi < info_.ChunksX(); ++cxi) {
            if(!ReadChunk(cs, ci, cxi, chunk, errorMessage)) {
                return false;
            }
            for(std::uint32_t localIl = 0; localIl < info_.chunkInlines; ++localIl) {
                const std::uint64_t globalIl = static_cast<std::uint64_t>(ci) * info_.chunkInlines + localIl;
                if(globalIl >= info_.inlines) {
                    break;
                }
                for(std::uint32_t localXl = 0; localXl < info_.chunkXlines; ++localXl) {
                    const std::uint64_t globalXl = static_cast<std::uint64_t>(cxi) * info_.chunkXlines + localXl;
                    if(globalXl >= info_.xlines) {
                        break;
                    }
                    const std::size_t chunkOffset =
                        (static_cast<std::size_t>(localS) * info_.chunkInlines + localIl) * info_.chunkXlines + localXl;
                    // SEG-Y display convention: row = inlines-1-inline.
                    const std::size_t row = static_cast<std::size_t>(info_.inlines - 1 - globalIl);
                    out[row * info_.xlines + static_cast<std::size_t>(globalXl)] = chunk[chunkOffset];
                }
            }
        }
    }
    if(sliceCache_) {
        Slice2D image;
        image.width = static_cast<int>(info_.xlines);
        image.height = static_cast<int>(info_.inlines);
        image.values = out;
        sliceCache_->Insert(sliceKey, std::make_shared<const Slice2D>(std::move(image)));
    }
    return true;
}

} // namespace engine
} // namespace seismic