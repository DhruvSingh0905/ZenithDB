#include "memtable.h"

#include <algorithm>

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

void MemTable::put(const std::string& key, const std::string& value) {
    // Simple approximate byte accounting: key + value
    approx_bytes_ += key.size() + value.size();

    data_[key] = value;
    update_range(key);
}

void MemTable::remove(const std::string& key) {
    // Tombstone as empty string; count a bit of size as well
    approx_bytes_ += key.size() + 1;
    data_[key] = std::string();  // empty -> tombstone
    update_range(key);
}

std::optional<std::string> MemTable::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;  // may be empty string (tombstone)
}

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

std::vector<std::pair<std::string, std::string>> MemTable::sorted_entries() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(data_.size());
    for (const auto& kv : data_) {
        out.push_back(kv);
    }
    return out;
}