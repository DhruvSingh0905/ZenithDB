// src/block_cache.h
#pragma once

#include <unordered_map>
#include <list>
#include <memory>
#include <mutex>
#include <string>

// Very simple global LRU block cache.
//
// Key:   usually "<file_id>:<block_offset>"
// Value: shared_ptr<string> holding the block's bytes
class BlockCache {
public:
    using Block      = std::string;
    using BlockPtr   = std::shared_ptr<Block>;
    using Key        = std::string;

    // ~16MB cache by default, assuming ~4KB blocks → ~4000 blocks.
    static constexpr std::size_t DEFAULT_CAP_BYTES = 16 * 1024 * 1024;

    // Singleton accessor
    static BlockCache& instance() {
        static BlockCache inst;
        return inst;
    }

    // Non-copyable
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    // Thread-safe LRU lookup.
    //
    // Returns nullptr if not found.
    BlockPtr get(const Key& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }

        // Move to front (most recently used)
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->second;
    }

    // Thread-safe insert/update.
    //
    // If key already exists, updates value and bumps to MRU.
    // If new key and capacity exceeded, evicts LRU entries until under cap.
    void put(const Key& key, BlockPtr value) {
        if (!value) return;
        const std::size_t block_size = value->size();

        std::lock_guard<std::mutex> lk(mutex_);

        // Update existing
        auto it = map_.find(key);
        if (it != map_.end()) {
            current_bytes_ -= it->second->second->size();
            it->second->second = std::move(value);
            current_bytes_ += block_size;

            // move to front (MRU)
            lru_.splice(lru_.begin(), lru_, it->second);
            evict_if_needed();
            return;
        }

        // Insert new
        lru_.emplace_front(key, std::move(value));
        map_[key] = lru_.begin();
        current_bytes_ += block_size;

        evict_if_needed();
    }

    // Optional: for tuning / tests
    void set_capacity(std::size_t bytes) {
        std::lock_guard<std::mutex> lk(mutex_);
        capacity_bytes_ = bytes;
        evict_if_needed();
    }

    std::size_t capacity_bytes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return capacity_bytes_;
    }

    std::size_t current_bytes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return current_bytes_;
    }

private:
    BlockCache() = default;

    void evict_if_needed() {
        while (current_bytes_ > capacity_bytes_ && !lru_.empty()) {
            auto& back = lru_.back();
            const auto& key  = back.first;
            const auto& ptr  = back.second;
            if (ptr) {
                current_bytes_ -= ptr->size();
            }
            map_.erase(key);
            lru_.pop_back();
        }
    }

    mutable std::mutex mutex_;

    // LRU list of (key, block)
    using ListType = std::list<std::pair<Key, BlockPtr>>;
    ListType lru_;  // front = MRU, back = LRU

    // Map from key → iterator into lru_
    std::unordered_map<Key, ListType::iterator> map_;

    std::size_t capacity_bytes_ = DEFAULT_CAP_BYTES;
    std::size_t current_bytes_  = 0;
};