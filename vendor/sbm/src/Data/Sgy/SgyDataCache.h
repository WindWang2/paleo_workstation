#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Data/Sgy/SgyVolume.h"

namespace seismic {

// Bounded LRU cache for computed data (raw slice/section values plus colours).
//
// Budget accounting includes entries that are still referenced elsewhere: an
// in-use image is never evicted, so the cache can temporarily exceed its
// budget instead of invalidating data the renderer still points at. The
// budget is a policy, not a hard memory limit.
class SgyDataCache {
public:
    explicit SgyDataCache(std::size_t budgetBytes = 256ull * 1024ull * 1024ull);

    void SetBudget(std::size_t budgetBytes);
    std::size_t Budget() const;
    std::size_t UsedBytes() const;

    // Returns the cached image and counts a hit, or nullptr (miss).
    std::shared_ptr<const SgySliceImage> Find(const std::string& key);

    // Existence check that does not change the hit/miss statistics.
    bool Peek(const std::string& key) const;

    void Store(const std::string& key, std::shared_ptr<const SgySliceImage> image);
    void Clear();

    std::size_t Hits() const;
    std::size_t Misses() const;
    std::size_t Evictions() const;
    std::size_t EntryCount() const;

    // Bytes an image occupies (values + rgba).
    static std::size_t ImageBytes(const SgySliceImage& image);
    static std::string MakeKey(const std::string& kind, std::uint64_t sourceHash, std::uint64_t payloadHash);

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const SgySliceImage> image;
        std::size_t bytes = 0;
        std::uint64_t lastUse = 0;
    };

    void EvictLocked(std::size_t requiredBytes);
    std::size_t UsedBytesLocked() const;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::size_t budgetBytes_ = 256ull * 1024ull * 1024ull;
    std::uint64_t clock_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
};

} // namespace seismic
