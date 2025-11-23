#include "memtable.h"

#include <algorithm>

/**
 * Updates the key range metadata when a new key is inserted.
 * 
 * Maintains the minimum and maximum keys seen so far to enable
 * efficient range pruning during point lookups.
 */
void MemTable::update_range(const std::string& key) {
    if (!has_range_) {
        min_key_   = key;
        max_key_   = key;
        has_range_ = true;
    } else {
        if (key < min_key_) min_key_ = key;
        if (key > max_key_) max_key_ = key;
    }
}

/**
 * Inserts or updates a key-value pair in the memtable.
 * 
 * Updates size approximation and range metadata.
 */
void MemTable::put(const std::string& key, const std::string& value) {
    // Simple approximate byte accounting: key + value
    approx_bytes_ += key.size() + value.size();

    data_[key] = value;
    update_range(key);
}

/**
 * Deletes a key by inserting a tombstone (empty value).
 * 
 * The tombstone will hide the key and eventually cause it to be
 * removed during compaction.
 */
void MemTable::remove(const std::string& key) {
    // Tombstone as empty string; count a bit of size as well
    approx_bytes_ += key.size() + 1;
    data_[key] = std::string();  // empty -> tombstone
    update_range(key);
}

/**
 * Performs a point lookup in the memtable.
 * 
 * Returns the value (which may be empty for tombstones) or nullopt.
 */
std::optional<std::string> MemTable::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;  // may be empty string (tombstone)
}

/**
 * Performs a range scan over keys in the memtable.
 * 
 * Uses std::map's lower_bound for efficient range iteration.
 */
std::vector<std::pair<std::string, std::string>> MemTable::scan(
    const std::string& start,
    const std::string& end) const
{
    std::vector<std::pair<std::string, std::string>> out;
    if (start > end) return out;

    auto it = data_.lower_bound(start);
    for (; it != data_.end() && it->first <= end; ++it) {
        out.emplace_back(it->first, it->second);
    }
    return out;
}

/**
 * Returns all entries in sorted order for flushing to disk.
 * 
 * Since we use std::map, entries are already sorted by key.
 */
std::vector<std::pair<std::string, std::string>> MemTable::sorted_entries() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(data_.size());
    for (const auto& kv : data_) {
        out.push_back(kv);
    }
    return out;
}