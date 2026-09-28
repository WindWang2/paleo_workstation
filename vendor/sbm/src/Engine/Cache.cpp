#include "Engine/Cache.h"

#include <algorithm>

namespace seismic {
namespace engine {

ChunkCache::ChunkCache(std::size_t budgetBytes) : budget_(budgetBytes) {
    stats_.budgetBytes = budgetBytes;
}

void ChunkCache::SetBudget(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    stats_.budgetBytes = bytes;
    EvictUntil(0);
}

std::size_t ChunkCache::Budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_;
}

std::shared_ptr<const std::vector<float>> ChunkCache::Find(const ChunkCacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    if(it == map_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    lru_.erase(it->second.lru);
    lru_.push_front(key);
    it->second.lru = lru_.begin();
    ++stats_.hits;
    return it->second.data;
}

void ChunkCache::Insert(const ChunkCacheKey& key, std::shared_ptr<const std::vector<float>> data) {
    if(!data) {
        return;
    }
    const std::size_t bytes = data->size() * sizeof(float);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = map_.find(key);
    if(existing != map_.end()) {
        bytes_ -= existing->second.data->size() * sizeof(float);
        lru_.erase(existing->second.lru);
        map_.erase(existing);
    }
    EvictUntil(bytes);
    lru_.push_front(key);
    Entry entry;
    entry.data = std::move(data);
    entry.lru = lru_.begin();
    map_.emplace(key, std::move(entry));
    bytes_ += bytes;
    ++stats_.inserts;
    stats_.entries = map_.size();
    stats_.bytes = bytes_;
}

void ChunkCache::EvictUntil(std::size_t incomingBytes) {
    while(!lru_.empty() && bytes_ + incomingBytes > budget_) {
        const ChunkCacheKey victim = lru_.back();
        lru_.pop_back();
        const auto it = map_.find(victim);
        if(it != map_.end()) {
            bytes_ -= it->second.data->size() * sizeof(float);
            map_.erase(it);
            ++stats_.evictions;
        }
    }
    stats_.entries = map_.size();
    stats_.bytes = bytes_;
}

void ChunkCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    map_.clear();
    bytes_ = 0;
    stats_.entries = 0;
    stats_.bytes = 0;
}

void ChunkCache::ResetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t bytes = stats_.bytes;
    const std::uint64_t entries = stats_.entries;
    const std::uint64_t budget = stats_.budgetBytes;
    stats_ = CacheStats{};
    stats_.bytes = bytes;
    stats_.entries = entries;
    stats_.budgetBytes = budget;
}

CacheStats ChunkCache::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats copy = stats_;
    copy.bytes = bytes_;
    copy.entries = map_.size();
    copy.budgetBytes = budget_;
    return copy;
}

SliceCache::SliceCache(std::size_t budgetBytes) : budget_(budgetBytes) {
    stats_.budgetBytes = budgetBytes;
}

void SliceCache::SetBudget(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    stats_.budgetBytes = bytes;
    EvictUntil(0);
}

std::size_t SliceCache::Budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_;
}

std::shared_ptr<const Slice2D> SliceCache::Find(const SliceCacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    if(it == map_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    lru_.erase(it->second.lru);
    lru_.push_front(key);
    it->second.lru = lru_.begin();
    ++stats_.hits;
    return it->second.data;
}

void SliceCache::Insert(const SliceCacheKey& key, std::shared_ptr<const Slice2D> image) {
    if(!image) {
        return;
    }
    const std::size_t bytes = image->values.size() * sizeof(float) + image->rgba.size();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = map_.find(key);
    if(existing != map_.end()) {
        bytes_ -= existing->second.data->values.size() * sizeof(float) + existing->second.data->rgba.size();
        lru_.erase(existing->second.lru);
        map_.erase(existing);
    }
    EvictUntil(bytes);
    lru_.push_front(key);
    Entry entry;
    entry.data = std::move(image);
    entry.lru = lru_.begin();
    map_.emplace(key, std::move(entry));
    bytes_ += bytes;
    ++stats_.inserts;
    stats_.entries = map_.size();
    stats_.bytes = bytes_;
}

void SliceCache::EvictUntil(std::size_t incomingBytes) {
    while(!lru_.empty() && bytes_ + incomingBytes > budget_) {
        const SliceCacheKey victim = lru_.back();
        lru_.pop_back();
        const auto it = map_.find(victim);
        if(it != map_.end()) {
            bytes_ -= it->second.data->values.size() * sizeof(float) + it->second.data->rgba.size();
            map_.erase(it);
            ++stats_.evictions;
        }
    }
    stats_.entries = map_.size();
    stats_.bytes = bytes_;
}

void SliceCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    map_.clear();
    bytes_ = 0;
    stats_.entries = 0;
    stats_.bytes = 0;
}

void SliceCache::ResetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t bytes = stats_.bytes;
    const std::uint64_t entries = stats_.entries;
    const std::uint64_t budget = stats_.budgetBytes;
    stats_ = CacheStats{};
    stats_.bytes = bytes;
    stats_.entries = entries;
    stats_.budgetBytes = budget;
}

CacheStats SliceCache::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats copy = stats_;
    copy.bytes = bytes_;
    copy.entries = map_.size();
    copy.budgetBytes = budget_;
    return copy;
}

ChunkCache2Q::ChunkCache2Q(std::size_t budgetBytes) : budget_(budgetBytes) {
    stats_.budgetBytes = budgetBytes;
}

void ChunkCache2Q::SetBudget(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    stats_.budgetBytes = bytes;
    EnforceBudget();
}

std::size_t ChunkCache2Q::Budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_;
}

std::size_t ChunkCache2Q::Bytes() const {
    std::size_t total = 0;
    for(const auto& pair : entries_) {
        total += pair.second.data->size() * sizeof(float);
    }
    return total;
}

std::shared_ptr<const std::vector<float>> ChunkCache2Q::Find(const ChunkCacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if(it == entries_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    const auto amIt = std::find(am_.begin(), am_.end(), key);
    if(amIt != am_.end()) {
        am_.erase(amIt);
        am_.push_front(key);
    } else {
        // Second access: promote from probation to the protected list.
        const auto a1It = std::find(a1in_.begin(), a1in_.end(), key);
        if(a1It != a1in_.end()) {
            a1in_.erase(a1It);
        }
        am_.push_front(key);
    }
    ++stats_.hits;
    return it->second.data;
}

void ChunkCache2Q::Insert(const ChunkCacheKey& key, std::shared_ptr<const std::vector<float>> data) {
    if(!data) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(entries_.find(key) != entries_.end()) {
        return;
    }
    const bool recentlyUsed = std::find(a1out_.begin(), a1out_.end(), key) != a1out_.end();
    if(recentlyUsed) {
        const auto ghost = std::find(a1out_.begin(), a1out_.end(), key);
        if(ghost != a1out_.end()) {
            a1out_.erase(ghost);
        }
        am_.push_front(key); // protected: it was evicted recently and came back
    } else {
        a1in_.push_front(key);
    }
    Entry entry;
    entry.data = std::move(data);
    entries_.emplace(key, std::move(entry));
    bytes_ = Bytes();
    ++stats_.inserts;
    if(!recentlyUsed) {
        // Keep A1in near a quarter of the budget; overflow demotes to A1out.
        const std::size_t a1Budget = std::max<std::size_t>(1, budget_ / 4);
        std::size_t a1Bytes = 0;
        for(const ChunkCacheKey& k : a1in_) {
            const auto it = entries_.find(k);
            if(it != entries_.end()) {
                a1Bytes += it->second.data->size() * sizeof(float);
            }
        }
        while(a1Bytes > a1Budget && a1in_.size() > 1) {
            const ChunkCacheKey victim = a1in_.back();
            a1in_.pop_back();
            const auto vit = entries_.find(victim);
            if(vit != entries_.end()) {
                a1Bytes -= vit->second.data->size() * sizeof(float);
                entries_.erase(vit);
                ++stats_.evictions;
            }
            a1out_.push_front(victim);
            while(a1out_.size() > a1outCapacity_) {
                a1out_.pop_back();
            }
        }
    }
    EnforceBudget();
    stats_.entries = entries_.size();
    stats_.bytes = bytes_;
}

void ChunkCache2Q::EnforceBudget() {
    while(bytes_ > budget_ && !entries_.empty()) {
        ChunkCacheKey victim;
        bool haveVictim = false;
        if(!a1in_.empty()) {
            victim = a1in_.back();
            a1in_.pop_back();
            haveVictim = true;
            a1out_.push_front(victim);
            while(a1out_.size() > a1outCapacity_) {
                a1out_.pop_back();
            }
        } else if(!am_.empty()) {
            victim = am_.back();
            am_.pop_back();
            haveVictim = true;
        }
        if(!haveVictim) {
            break;
        }
        const auto it = entries_.find(victim);
        if(it != entries_.end()) {
            bytes_ -= it->second.data->size() * sizeof(float);
            entries_.erase(it);
            ++stats_.evictions;
        }
    }
    stats_.entries = entries_.size();
    stats_.bytes = bytes_;
}

void ChunkCache2Q::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    a1in_.clear();
    am_.clear();
    a1out_.clear();
    entries_.clear();
    bytes_ = 0;
    stats_.entries = 0;
    stats_.bytes = 0;
}

void ChunkCache2Q::ResetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t bytes = stats_.bytes;
    const std::uint64_t entries = stats_.entries;
    const std::uint64_t budget = stats_.budgetBytes;
    stats_ = CacheStats{};
    stats_.bytes = bytes;
    stats_.entries = entries;
    stats_.budgetBytes = budget;
}

CacheStats ChunkCache2Q::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats copy = stats_;
    copy.bytes = bytes_;
    copy.entries = entries_.size();
    copy.budgetBytes = budget_;
    return copy;
}

} // namespace engine
} // namespace seismic