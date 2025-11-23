#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstddef>

/**
 * MemTable - In-memory sorted key-value store.
 * 
 * MemTables are the write buffer for ZenithDB. They store key-value pairs
 * in a sorted map structure. When a MemTable becomes immutable (frozen),
 * it can be safely flushed to disk as an SSTable while reads continue.
 * 
 * Features:
 * - Fast in-memory writes (O(log n) insertion)
 * - Range tracking for efficient pruning during reads
 * - Approximate size tracking for flush decisions
 * - Tombstone support (empty value = deleted)
 */
class MemTable {
public:
    MemTable() = default;

    /**
     * Inserts or updates a key-value pair.
     * 
     * Updates the approximate size and key range metadata.
     * 
     * @param key The key to insert/update
     * @param value The value (non-empty = live, empty = tombstone)
     */
    void put(const std::string& key, const std::string& value);

    /**
     * Marks a key as deleted by inserting a tombstone.
     * 
     * A tombstone is represented as an empty string value.
     * 
     * @param key The key to delete
     */
    void remove(const std::string& key);

    /**
     * Performs a point lookup for a key.
     * 
     * @param key The key to look up
     * @return Optional containing the value (may be empty for tombstone), or nullopt if not found
     */
    std::optional<std::string> get(const std::string& key) const;

    /**
     * Performs an inclusive range scan.
     * 
     * Returns all key-value pairs where start <= key <= end.
     * 
     * @param start The inclusive start key
     * @param end The inclusive end key
     * @return Vector of (key, value) pairs in sorted order
     */
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start,
        const std::string& end) const;

    /**
     * Returns all entries sorted by key.
     * 
     * Used when flushing the memtable to disk. The entries are
     * already sorted since we use a std::map internally.
     * 
     * @return Vector of all (key, value) pairs in sorted order
     */
    std::vector<std::pair<std::string, std::string>> sorted_entries() const;

    /**
     * Returns the approximate size in bytes.
     * 
     * This is a rough estimate used for flush threshold decisions.
     * It counts key + value sizes but doesn't account for map overhead.
     * 
     * @return Approximate size in bytes
     */
    std::size_t approximate_size() const { return approx_bytes_; }

    /**
     * Returns whether this memtable has valid range metadata.
     * 
     * @return True if min_key and max_key are valid
     */
    bool has_range() const { return has_range_; }
    
    /**
     * Returns the minimum key in this memtable.
     * 
     * @return The smallest key, or empty string if no range
     */
    const std::string& min_key() const { return min_key_; }
    
    /**
     * Returns the maximum key in this memtable.
     * 
     * @return The largest key, or empty string if no range
     */
    const std::string& max_key() const { return max_key_; }

private:
    // key -> value (empty string == tombstone)
    std::map<std::string, std::string> data_;

    // Approximate size in bytes
    std::size_t approx_bytes_ = 0;

    // Range tracking
    bool has_range_ = false;
    std::string min_key_;
    std::string max_key_;

    /**
     * Updates the min/max key range metadata.
     * 
     * Called whenever a key is inserted to maintain range bounds
     * for efficient pruning during reads.
     * 
     * @param key The key that was just inserted
     */
    void update_range(const std::string& key);
};