#pragma once
#include <unordered_map>
#include <list>
#include <string>
#include <mutex>

class BlockCache {
public:
    // 16 MB total capacity, assuming 4 KB blocks → 4096 blocks
    static constexpr size_t MAX_BLOCKS = 4096;

    // Return cached block for key, or empty string if not present.
    std::string get(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return {};

        // Move this entry to front (most recently used)
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->second;
    }

    // Insert or update a block for key.
    void put(const std::string& key, std::string value) {
        std::lock_guard<std::mutex> lk(mutex_);

        // Update existing
        if (auto it = map_.find(key); it != map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);
            it->second->second = std::move(value);
            return;
        }

        // Evict least-recently-used if full
        if (lru_.size() >= MAX_BLOCKS) {
            auto& back = lru_.back();
            map_.erase(back.first);
            lru_.pop_back();
        }

        // Insert new at front
        lru_.emplace_front(key, std::move(value));
        map_[key] = lru_.begin();
    }

private:
    mutable std::mutex mutex_;

    using Entry = std::pair<std::string, std::string>;
    std::list<Entry> lru_;  // most recently used at front
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
};