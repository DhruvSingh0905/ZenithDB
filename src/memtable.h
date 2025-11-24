#pragma once

#include "skiplist.h"
#include "arena.h"
#include <vector>
#include <string>
#include <string_view>
#include <optional>

/**
 * MemTable - In-memory sorted key-value store using lock-free skip list.
 * 
 * CRITICAL LOW-LEVEL OPTIMIZATION: Uses a lock-free skip list with arena
 * allocator instead of std::map. This provides:
 * - Lock-free reads: Better concurrent read performance
 * - Zero memory fragmentation: Arena allocator eliminates heap fragmentation
 * - Cache-friendly: Sequential allocations improve cache locality
 * - Reduced allocation overhead: O(1) arena allocations vs O(log n) heap allocations
 * 
 * The skip list provides O(log n) average-case operations (insert, lookup)
 * with better cache behavior than balanced trees. The arena allocator ensures
 * all memory is allocated from contiguous blocks, eliminating fragmentation.
 * 
 * Thread Safety: Not thread-safe (protected by writer_mutex_ in ZenithDB)
 */
class MemTable {
public:
    /**
     * Constructs an empty memtable.
     * 
     * Initializes the skip list with the arena allocator.
     */
    MemTable();
    
    // No copy/move (arena and skip list are not copyable)
    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    /**
     * Inserts or updates a key-value pair.
     * 
     * @param key The key (string_view avoids allocation)
     * @param value The value (string_view avoids allocation)
     */
    void put(std::string_view key, std::string_view value);
    
    /**
     * Deletes a key by inserting a tombstone (empty value).
     * 
     * @param key The key to delete
     */
    void remove(std::string_view key);

    /**
     * Retrieves a value by key.
     * 
     * @param key The key to look up
     * @return Optional containing the value, or nullopt if not found
     */
    std::optional<std::string> get(std::string_view key) const;

    /**
     * Performs a range scan (inclusive on both ends).
     * 
     * Uses skip list iterator for efficient sequential traversal.
     * 
     * @param start Start key (inclusive)
     * @param end End key (inclusive)
     * @return Vector of key-value pairs in sorted order
     */
    std::vector<std::pair<std::string, std::string>> scan(
        std::string_view start,
        std::string_view end) const;

    /**
     * Returns all entries in sorted order (for flushing to SSTable).
     * 
     * @return Vector of all key-value pairs, sorted by key
     */
    std::vector<std::pair<std::string, std::string>> sorted_entries() const;

    /**
     * Returns approximate size in bytes (for flush decisions).
     * 
     * Uses arena memory usage, which accurately tracks all allocations.
     */
    std::size_t approximate_size() const { return arena_.MemoryUsage(); }
    
    /**
     * Returns whether this memtable has valid range metadata.
     */
    bool has_range() const { return has_range_; }
    
    /**
     * Returns the minimum key in this memtable (for range pruning).
     */
    const std::string& min_key() const { return min_key_; }
    
    /**
     * Returns the maximum key in this memtable (for range pruning).
     */
    const std::string& max_key() const { return max_key_; }

private:
    Arena arena_;        // Arena allocator for all memory (zero fragmentation)
    SkipList table_;     // Lock-free skip list for sorted key-value storage

    bool has_range_ = false;         // Whether min/max keys are valid
    std::string min_key_;            // Minimum key (for range pruning)
    std::string max_key_;            // Maximum key (for range pruning)

    /**
     * Updates min_key and max_key when a new key is inserted.
     * 
     * @param key The newly inserted key
     */
    void update_range(std::string_view key);
};