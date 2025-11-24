#include "memtable.h"
#include <algorithm>

/**
 * Updates the min/max key range when a new key is inserted.
 * 
 * This enables efficient range pruning: readers can skip this memtable
 * entirely if the lookup key is outside [min_key_, max_key_].
 */
void MemTable::update_range(std::string_view key) {
    if (!has_range_) {
        min_key_   = key;
        max_key_   = key;
        has_range_ = true;
    } else {
        if (key < min_key_) min_key_ = key;
        if (key > max_key_) max_key_ = key;
    }
}

void MemTable::put(std::string_view key, std::string_view value) {
    // Update approximate size (used for flush threshold checks)
    approx_bytes_ += key.size() + value.size();
    
    // Emplace constructs string from string_view only if key doesn't exist.
    // This avoids unnecessary allocations when updating existing keys.
    auto [it, inserted] = data_.emplace(key, value);
    if (!inserted) {
        // Key already exists - update value
        it->second = value;
    }
    update_range(key);
}

void MemTable::remove(std::string_view key) {
    approx_bytes_ += key.size() + 1;
    // Insert/Update with empty string (tombstone)
    auto [it, inserted] = data_.emplace(key, "");
    if (!inserted) {
        it->second.clear();
    }
    update_range(key);
}

std::optional<std::string> MemTable::get(std::string_view key) const {
    // CRITICAL OPTIMIZATION: find() with string_view works without allocation
    // thanks to std::less<> transparent comparator. This is a low-level
    // optimization that significantly improves read performance by avoiding
    // temporary string allocations.
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::pair<std::string, std::string>> MemTable::scan(
    std::string_view start,
    std::string_view end) const
{
    std::vector<std::pair<std::string, std::string>> out;
    if (start > end) return out;

    // CRITICAL OPTIMIZATION: lower_bound() with string_view works without
    // allocation thanks to std::less<> transparent comparator.
    auto it = data_.lower_bound(start);
    for (; it != data_.end(); ++it) {
        if (it->first > end) break;  // Stop when we've passed the end key
        out.emplace_back(it->first, it->second);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> MemTable::sorted_entries() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(data_.size());
    for (const auto& kv : data_) {
        out.push_back(kv);
    }
    return out;
}