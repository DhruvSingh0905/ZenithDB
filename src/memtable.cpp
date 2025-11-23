#include "memtable.h"

void MemTable::put(const std::string& key, const std::string& value) {
    map_[key] = value;
}

void MemTable::remove(const std::string& key) {
    map_[key] = TOMBSTONE;
}

std::optional<std::string> MemTable::get(const std::string& key) const {
    auto it = map_.find(key);
    if (it == map_.end() || it->second == TOMBSTONE) {
        return std::nullopt;
    }
    return it->second;
}

void MemTable::clear() {
    map_.clear();
}

std::vector<std::pair<std::string, std::string>> MemTable::sorted_entries() const {
    std::vector<std::pair<std::string, std::string>> vec;
    for (const auto& [k, v] : map_) {
        if (!v.empty()) vec.emplace_back(k, v);
    }
    return vec;
}

size_t MemTable::approximate_size() const {
    size_t s = 0;
    for (const auto& [k, v] : map_) s += k.size() + v.size();
    return s;
}

std::vector<std::pair<std::string, std::string>> MemTable::scan(const std::string& start, const std::string& end) const {
    std::vector<std::pair<std::string, std::string>> res;
    auto it = map_.lower_bound(start);
    for (; it != map_.end() && it->first <= end; ++it) {
        if (!it->second.empty()) res.emplace_back(it->first, it->second);
    }
    return res;
}