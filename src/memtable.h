#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

/**
 * MemTable - In-memory sorted key-value store.
 * 
 * Uses std::map with transparent comparator (std::less<>) to enable
 * zero-allocation lookups with string_view. This is a critical optimization
 * that avoids string allocations during read operations.
 * 
 * Features:
 * - O(log n) insertions and lookups
 * - Range tracking (min_key, max_key) for efficient pruning
 * - Approximate size tracking for flush decisions
 * - Tombstone support (empty value = deleted)
 * 
 * Thread Safety: Not thread-safe (protected by writer_mutex_ in ZenithDB)
 */
class MemTable {
public:
    MemTable() = default;

    /**
     * Inserts or updates a key-value pair.
     * 
     * Uses emplace() to avoid unnecessary string copies when key doesn't exist.
     * Updates approximate size counter for flush decisions.
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
     * Uses std::map::find() with string_view (no allocation thanks to std::less<>).
     * This is a critical optimization for read performance.
     * 
     * @param key The key to look up
     * @return Optional containing the value, or nullopt if not found
     */
    std::optional<std::string> get(std::string_view key) const;

    /**
     * Performs a range scan (inclusive on both ends).
     * 
     * Uses lower_bound() with string_view for efficient range queries.
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
     * This is approximate because it doesn't account for map overhead,
     * but it's sufficient for threshold checks.
     */
    std::size_t approximate_size() const { return approx_bytes_; }
    
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
    /**
     * The underlying sorted map.
     * 
     * std::less<> enables transparent comparison, allowing find() and lower_bound()
     * to work with string_view without allocating temporary strings. This is a
     * critical low-level optimization that significantly improves read performance.
     */
    std::map<std::string, std::string, std::less<>> data_;

    std::size_t approx_bytes_ = 0;  // Approximate size in bytes
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