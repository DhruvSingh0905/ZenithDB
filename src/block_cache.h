// src/block_cache.h
#pragma once

#include <unordered_map>
#include <list>
#include <memory>
#include <mutex>
#include <string>

/**
 * BlockCache - Global LRU cache for SSTable file contents.
 * 
 * This singleton cache stores entire SSTable files in memory to avoid
 * repeated disk reads. It uses an LRU eviction policy to manage memory.
 * 
 * Key:   File path (e.g., "data/L0_1234567890_1.sst")
 * Value: shared_ptr to the file's raw bytes
 * 
 * Thread-safe: All operations are protected by a mutex.
 */
class BlockCache {
public:
    using Block      = std::string;
    using BlockPtr   = std::shared_ptr<Block>;
    using Key        = std::string;

    // ~16MB cache by default, assuming ~4KB blocks → ~4000 blocks.
    static constexpr std::size_t DEFAULT_CAP_BYTES = 16 * 1024 * 1024;

    /**
     * Returns the singleton instance of BlockCache.
     * 
     * @return Reference to the global BlockCache instance
     */
    static BlockCache& instance() {
        static BlockCache inst;
        return inst;
    }

    // Non-copyable
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    /**
     * Looks up a block in the cache.
     * 
     * If found, moves it to the front (most recently used) and returns it.
     * Thread-safe.
     * 
     * @param key The cache key (file path)
     * @return shared_ptr to the cached block, or nullptr if not found
     */
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

    /**
     * Inserts or updates a block in the cache.
     * 
     * If the key exists, updates the value and moves it to MRU.
     * If it's new and capacity is exceeded, evicts LRU entries until under limit.
     * Thread-safe.
     * 
     * @param key The cache key (file path)
     * @param value The block data to cache
     */
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

    /**
     * Sets the cache capacity in bytes.
     * 
     * If the current size exceeds the new capacity, evicts entries until under limit.
     * 
     * @param bytes Maximum cache size in bytes
     */
    void set_capacity(std::size_t bytes) {
        std::lock_guard<std::mutex> lk(mutex_);
        capacity_bytes_ = bytes;
        evict_if_needed();
    }

    /**
     * Returns the current cache capacity in bytes.
     * 
     * @return Maximum cache size in bytes
     */
    std::size_t capacity_bytes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return capacity_bytes_;
    }

    /**
     * Returns the current cache size in bytes.
     * 
     * @return Current number of bytes cached
     */
    std::size_t current_bytes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return current_bytes_;
    }

private:
    BlockCache() = default;

    /**
     * Evicts LRU entries until the cache is under capacity.
     * 
     * Called internally after insertions. Removes entries from the back
     * of the LRU list (least recently used) until current_bytes_ <= capacity_bytes_.
     */
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