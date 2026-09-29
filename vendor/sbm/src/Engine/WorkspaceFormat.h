#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Cache.h"

namespace seismic {
namespace engine {

// Native lightweight chunk/shard workspace format (Stage D prototype).
//
// Layout: one metadata file <name>.sf3c.meta plus <name>.sf3.sNNN shard files.
// Every chunk is independently checksummed; the completion map records which
// chunks were written so an interrupted transcode can be resumed.
//
// This is a PROTOTYPE: the codec set is {raw}; zstd is planned behind
// SEISMIC_ENABLE_ZSTD. The format is versioned (kWorkspaceFormatVersion).
struct WorkspaceInfo {
    std::uint32_t version = 1;
    std::uint32_t samples = 0;
    std::uint32_t inlines = 0;
    std::uint32_t xlines = 0;
    std::uint32_t chunkSamples = 0;
    std::uint32_t chunkInlines = 0;
    std::uint32_t chunkXlines = 0;
    std::uint32_t chunksPerShard = 0;
    std::uint32_t codec = 0;      // kCodecRaw / kCodecZstd
    std::uint32_t codecLevel = 3; // zstd level when codec == kCodecZstd
    std::uint32_t sampleIntervalUs = 0;
    std::int32_t inlineMin = 0;
    std::int32_t xlineMin = 0;
    std::uint64_t sourceIdentityHash = 0;
    std::uint32_t algorithmVersion = 0;
    // Stage F LOD metadata.
    std::uint32_t lodLevel = 0;        // 0 = base level
    std::uint32_t lodMethod = 0;       // 0 = none, 1 = average, 2 = decimate, 3 = rms, 4 = envelope
    std::uint32_t lodFactorSamples = 1;
    std::uint32_t lodFactorInlines = 1;
    std::uint32_t lodFactorXlines = 1;
    std::uint32_t lodAlgorithmVersion = 0;
    std::uint64_t lodSourceHash = 0;   // identity of the level this one was built from

    std::uint32_t ChunksS() const;
    std::uint32_t ChunksI() const;
    std::uint32_t ChunksX() const;
    std::uint64_t ChunkCount() const;
    std::uint64_t ChunkBytes() const;
    std::uint64_t ShardCount() const;
    std::uint64_t VolumeBytes() const;
};

struct WorkspaceChunkEntry {
    std::uint32_t shardIndex = 0;
    std::uint64_t offsetInShard = 0;
    std::uint64_t storedBytes = 0;
    std::uint64_t checksum = 0;
};

constexpr char kWorkspaceMagic[8] = {'S', 'F', '3', 'C', 'H', 'N', 'K', '1'};
constexpr std::uint32_t kWorkspaceFormatVersion = 3;
constexpr std::uint32_t kCodecRaw = 0;
constexpr std::uint32_t kCodecZstd = 1;

std::filesystem::path WorkspaceMetaPath(const std::filesystem::path& basePath);
std::filesystem::path WorkspaceShardPath(const std::filesystem::path& basePath, std::uint32_t shardIndex);
std::uint64_t WorkspaceChecksum(const void* data, std::size_t bytes);

// P7 (paleo): header-only meta probe for resume UX / backend readiness checks.
// Reads magic..completion without touching the chunk table or shards, so it is
// safe to call cheaply on every Auto open.
struct WorkspaceMetaSummary {
    bool exists = false;
    bool readable = false;          // header parsed (version supported)
    std::uint32_t formatVersion = 0;
    std::uint32_t algorithmVersion = 0;
    std::uint32_t samples = 0;
    std::uint32_t inlines = 0;
    std::uint32_t xlines = 0;
    std::uint32_t codec = 0;
    std::uint64_t chunkCount = 0;
    std::uint64_t chunksCompleted = 0; // popcount of the completion bitmap
    bool complete = false;
    std::uint64_t sourceIdentityHash = 0;
    std::string error;               // reason when exists && !readable
};
bool ProbeWorkspaceMeta(
    const std::filesystem::path& basePath,
    WorkspaceMetaSummary& out,
    std::string& errorMessage);

// Streaming writer: generate or transcode chunk by chunk, then finalize.
// WriteChunk writes only chunks inside the volume box; padded edge chunks are
// stored with NaN outside the volume so every chunk has a fixed byte size.
class WorkspaceWriter {
public:
    WorkspaceWriter() = default;
    ~WorkspaceWriter();

    // Creates a new workspace, or resumes an existing one (resumeExisting=true)
    // and fills only the missing chunks.
    bool Open(
        const std::filesystem::path& basePath,
        const WorkspaceInfo& info,
        bool resumeExisting,
        std::string& errorMessage);

    bool WriteChunk(
        std::uint32_t cs, std::uint32_t ci, std::uint32_t cx,
        const float* values, std::size_t valueCount, std::string& errorMessage);

    // P6 (paleo): writes an already-encoded payload (compression happened on a
    // pool thread). WriteChunk == encode + this. Single writer thread only.
    bool WriteChunkPrepared(
        std::uint32_t cs, std::uint32_t ci, std::uint32_t cx,
        const void* payload, std::size_t payloadBytes, std::string& errorMessage);

    bool HasChunk(std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const;
    const WorkspaceInfo& Info() const { return info_; }

    // Device-adaptive write batching: encoded chunks are accumulated per shard
    // and flushed when the batch reaches `bytes` (or on shard switch/Finalize).
    // Bigger batches mean fewer, more sequential writes on rotational media.
    void SetWriteBatchBytes(std::size_t bytes) { writeBatchBytes_ = bytes; }
    std::uint64_t WriteCalls() const { return writeCalls_; }

    // Publishes the metadata with the completion map and closes shards.
    bool Finalize(std::string& errorMessage);

private:
    bool WriteMeta(std::string& errorMessage);
    bool OpenShard(std::uint32_t shardIndex, std::string& errorMessage);
    std::uint64_t ChunkOrdinal(std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const;

    std::filesystem::path basePath_;
    WorkspaceInfo info_;
    std::vector<WorkspaceChunkEntry> entries_;
    std::vector<unsigned char> completion_;
    std::uint32_t openShard_ = 0xFFFFFFFFu;
    std::uint64_t openShardOffset_ = 0;
    void* openShardFile_ = nullptr; // std::ofstream kept opaque
    bool created_ = false;
    std::size_t writeBatchBytes_ = 0; // 0 = write every chunk immediately
    std::vector<unsigned char> writeBatch_;
    std::uint64_t writeCalls_ = 0;
};

class WorkspaceReader {
public:
    WorkspaceReader() = default;

    bool Open(const std::filesystem::path& basePath, std::string& errorMessage);

    const WorkspaceInfo& Info() const { return info_; }
    bool HasChunk(std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const;

    bool ReadChunk(
        std::uint32_t cs, std::uint32_t ci, std::uint32_t cx,
        std::vector<float>& out, std::string& errorMessage);

    std::uint32_t Codec() const { return info_.codec; }
    std::uint32_t CodecLevel() const { return info_.codecLevel; }


    // Convenience assembly (NaN outside the volume; NaN for missing chunks).
    bool ReadTrace(int inlineNo, int xlineNo, std::vector<float>& out, std::string& errorMessage);
    bool ReadInlineSlice(int inlineNo, std::vector<float>& out, std::string& errorMessage);
    bool ReadTimeSlice(int sampleIndex, std::vector<float>& out, std::string& errorMessage);

    std::uint64_t ChunksRead() const { return chunksRead_; }
    std::uint64_t BytesRead() const { return bytesRead_; }
    std::uint64_t ChecksumFailures() const { return checksumFailures_; }


    // Optional byte-bounded caches (Stage G). The chunk cache removes repeated
    // shard I/O; the slice cache removes repeated assembly.
    void SetChunkCache(std::shared_ptr<ChunkCache> cache) { chunkCache_ = std::move(cache); }
    void SetSliceCache(std::shared_ptr<SliceCache> cache) { sliceCache_ = std::move(cache); }
    std::shared_ptr<ChunkCache> ChunkCacheHandle() const { return chunkCache_; }
    std::shared_ptr<SliceCache> SliceCacheHandle() const { return sliceCache_; }
    std::uint64_t CacheSourceKey() const { return cacheSourceKey_; }

    // Resume support: the writer reuses the published chunk table.
    const std::vector<unsigned char>& Completion() const { return completion_; }
    const std::vector<WorkspaceChunkEntry>& Entries() const { return entries_; }

    // P7 (paleo): all chunks of the completion bitmap are present. An
    // interrupted-but-resumable workspace is NOT complete.
    bool IsComplete() const;

private:

    std::uint64_t ChunkOrdinal(std::uint32_t cs, std::uint32_t ci, std::uint32_t cx) const;

    std::filesystem::path basePath_;
    WorkspaceInfo info_;
    std::vector<WorkspaceChunkEntry> entries_;
    std::vector<unsigned char> completion_;
    std::uint64_t chunksRead_ = 0;
    std::uint64_t bytesRead_ = 0;
    std::uint64_t checksumFailures_ = 0;

    std::uint64_t cacheSourceKey_ = 0; // metadata identity + generation
    std::shared_ptr<ChunkCache> chunkCache_;
    std::shared_ptr<SliceCache> sliceCache_;
    // The reader keeps one shard stream open: at ROI scale the per-chunk
    // open/close dominated the read time.
    mutable std::uint32_t openShardIndex_ = 0xFFFFFFFFu;
    mutable std::shared_ptr<std::ifstream> openShard_;
};

} // namespace engine
} // namespace seismic