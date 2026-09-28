#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Engine/Types.h"

namespace seismic {
namespace engine {

// Byte-bounded LRU caches for decoded workspace chunks and assembled slices.
// Entries are shared_ptr: eviction only drops the cache's reference, so an
// in-flight reader keeps its data valid.

struct ChunkCacheKey {
    std::uint64_t source = 0; // workspace identity + metadata generation
    std::uint32_t level = 0;
    std::uint32_t cs = 0;
    std::uint32_t ci = 0;
    std::uint32_t cx = 0;

    bool operator==(const ChunkCacheKey& other) const {
        return source == other.source && level == other.level && cs == other.cs &&
               ci == other.ci && cx == other.cx;
    }
};

struct ChunkCacheKeyHash {
    std::size_t operator()(const ChunkCacheKey& key) const {
        std::uint64_t h = key.source;
        h ^= (static_cast<std::uint64_t>(key.level) + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull;
        h ^= (static_cast<std::uint64_t>(key.cs) + 0x94d049bb133111ebull) * 0x2545f4914f6cdd1dull;
        h ^= (static_cast<std::uint64_t>(key.ci) << 21) ^ (static_cast<std::uint64_t>(key.cx) << 42);
        return static_cast<std::size_t>(h);
    }
};

struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t inserts = 0;
    std::uint64_t bytes = 0;
    std::uint64_t budgetBytes = 0;
    std::uint64_t entries = 0;
};

class ChunkCache {
public:
    explicit ChunkCache(std::size_t budgetBytes = 256ull * 1024ull * 1024ull);

    void SetBudget(std::size_t bytes);
    std::size_t Budget() const;

    std::shared_ptr<const std::vector<float>> Find(const ChunkCacheKey& key);
    void Insert(const ChunkCacheKey& key, std::shared_ptr<const std::vector<float>> data);

    void Clear();
    void ResetStats();
    CacheStats Stats() const;

private:
    struct Entry {
        std::shared_ptr<const std::vector<float>> data;
        std::list<ChunkCacheKey>::iterator lru;
    };

    void EvictUntil(std::size_t incomingBytes);

    mutable std::mutex mutex_;
    std::size_t budget_ = 0;
    std::size_t bytes_ = 0;
    std::list<ChunkCacheKey> lru_;
    std::unordered_map<ChunkCacheKey, Entry, ChunkCacheKeyHash> map_;
    mutable CacheStats stats_;
};

// Assembled slice cache key: kind (0 inline, 1 xline, 2 time), index and level.
struct SliceCacheKey {
    std::uint64_t source = 0;
    std::uint32_t level = 0;
    std::uint32_t kind = 0;
    std::int32_t index = 0;
    // Section/derived requests: path hash + interpolation + resolution + the
    // processing algorithm version.
    std::uint64_t variant = 0;

    bool operator==(const SliceCacheKey& other) const {
        return source == other.source && level == other.level && kind == other.kind &&
               index == other.index && variant == other.variant;
    }
};

struct SliceCacheKeyHash {
    std::size_t operator()(const SliceCacheKey& key) const {
        std::uint64_t h = key.source ^ (static_cast<std::uint64_t>(key.level) << 32);
        h ^= (static_cast<std::uint64_t>(key.kind) << 8) ^ static_cast<std::uint64_t>(key.index) * 2654435761ull;
        h ^= key.variant * 0x9e3779b97f4a7c15ull;
        return static_cast<std::size_t>(h);
    }
};

// 2Q variant of the chunk cache: a small FIFO probation queue (A1in) and an LRU
// protected list (Am), with a key-only ghost list (A1out). Compared against the
// plain LRU cache by tests/engine_cache_test.cpp.
class ChunkCache2Q {
public:
    explicit ChunkCache2Q(std::size_t budgetBytes = 256ull * 1024ull * 1024ull);

    void SetBudget(std::size_t bytes);
    std::size_t Budget() const;

    std::shared_ptr<const std::vector<float>> Find(const ChunkCacheKey& key);
    void Insert(const ChunkCacheKey& key, std::shared_ptr<const std::vector<float>> data);

    void Clear();
    void ResetStats();
    CacheStats Stats() const;

private:
    struct Entry {
        std::shared_ptr<const std::vector<float>> data;
    };

    std::size_t Bytes() const;
    void EnforceBudget();

    mutable std::mutex mutex_;
    std::size_t budget_ = 0;
    std::size_t bytes_ = 0;
    std::list<ChunkCacheKey> a1in_;  // FIFO probation
    std::list<ChunkCacheKey> am_;    // LRU protected
    std::list<ChunkCacheKey> a1out_; // ghost keys only
    std::unordered_map<ChunkCacheKey, Entry, ChunkCacheKeyHash> entries_;
    mutable CacheStats stats_;
    std::size_t a1outCapacity_ = 64;
};

class SliceCache {
public:
    explicit SliceCache(std::size_t budgetBytes = 64ull * 1024ull * 1024ull);

    void SetBudget(std::size_t bytes);
    std::size_t Budget() const;

    std::shared_ptr<const Slice2D> Find(const SliceCacheKey& key);
    void Insert(const SliceCacheKey& key, std::shared_ptr<const Slice2D> image);

    void Clear();
    void ResetStats();
    CacheStats Stats() const;

private:
    struct Entry {
        std::shared_ptr<const Slice2D> data;
        std::list<SliceCacheKey>::iterator lru;
    };

    void EvictUntil(std::size_t incomingBytes);

    mutable std::mutex mutex_;
    std::size_t budget_ = 0;
    std::size_t bytes_ = 0;
    std::list<SliceCacheKey> lru_;
    std::unordered_map<SliceCacheKey, Entry, SliceCacheKeyHash> map_;
    mutable CacheStats stats_;
};

} // namespace engine
} // namespace seismic