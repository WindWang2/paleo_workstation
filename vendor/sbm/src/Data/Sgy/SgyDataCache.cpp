#include "Data/Sgy/SgyDataCache.h"

#include <algorithm>
#include <cstdio>

namespace seismic {

SgyDataCache::SgyDataCache(std::size_t budgetBytes) : budgetBytes_(budgetBytes) {}

void SgyDataCache::SetBudget(std::size_t budgetBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budgetBytes_ = budgetBytes;
    EvictLocked(0);
}

std::size_t SgyDataCache::Budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budgetBytes_;
}

std::size_t SgyDataCache::UsedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return UsedBytesLocked();
}

std::size_t SgyDataCache::UsedBytesLocked() const {
    std::size_t bytes = 0;
    for(const Entry& entry : entries_) {
        bytes += entry.bytes;
    }
    return bytes;
}

std::shared_ptr<const SgySliceImage> SgyDataCache::Find(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    for(Entry& entry : entries_) {
        if(entry.key == key) {
            entry.lastUse = ++clock_;
            ++hits_;
            return entry.image;
        }
    }
    ++misses_;
    return nullptr;
}

bool SgyDataCache::Peek(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const Entry& entry : entries_) {
        if(entry.key == key) {
            return true;
        }
    }
    return false;
}

void SgyDataCache::Store(const std::string& key, std::shared_ptr<const SgySliceImage> image) {
    if(!image) {
        return;
    }
    const std::size_t bytes = ImageBytes(*image);
    std::lock_guard<std::mutex> lock(mutex_);

    for(auto it = entries_.begin(); it != entries_.end(); ++it) {
        if(it->key == key) {
            entries_.erase(it);
            break;
        }
    }

    EvictLocked(bytes);
    Entry entry;
    entry.key = key;
    entry.image = std::move(image);
    entry.bytes = bytes;
    entry.lastUse = ++clock_;
    entries_.push_back(std::move(entry));
}

void SgyDataCache::EvictLocked(std::size_t requiredBytes) {
    // Evict least-recently-used entries that nobody else references. Entries
    // still in use are kept even when that temporarily exceeds the budget.
    while(UsedBytesLocked() + requiredBytes > budgetBytes_ && !entries_.empty()) {
        auto victim = entries_.end();
        for(auto it = entries_.begin(); it != entries_.end(); ++it) {
            if(it->image.use_count() > 1) {
                continue;
            }
            if(victim == entries_.end() || it->lastUse < victim->lastUse) {
                victim = it;
            }
        }
        if(victim == entries_.end()) {
            return; // everything is in use
        }
        entries_.erase(victim);
        ++evictions_;
    }
}

void SgyDataCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::size_t SgyDataCache::Hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

std::size_t SgyDataCache::Misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

std::size_t SgyDataCache::Evictions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evictions_;
}

std::size_t SgyDataCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t SgyDataCache::ImageBytes(const SgySliceImage& image) {
    return image.values.size() * sizeof(float) + image.rgba.size() * sizeof(unsigned char) +
           sizeof(SgySliceImage);
}

std::string SgyDataCache::MakeKey(const std::string& kind, std::uint64_t sourceHash, std::uint64_t payloadHash) {
    char buffer[64] = {};
    std::snprintf(
        buffer, sizeof(buffer), "%s|%016llx|%016llx", kind.c_str(),
        static_cast<unsigned long long>(sourceHash), static_cast<unsigned long long>(payloadHash));
    return buffer;
}

} // namespace seismic
