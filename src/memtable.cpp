#include "memtable.h"
#include <algorithm>

MemTable::MemTable() : table_(&arena_) {}

void MemTable::update_range(std::string_view key) {
    if (!has_range_) {
        min_key_ = std::string(key);
        max_key_ = std::string(key);
        has_range_ = true;
    } else {
        if (key < min_key_) min_key_ = std::string(key);
        if (key > max_key_) max_key_ = std::string(key);
    }
}

void MemTable::put(std::string_view key, std::string_view value) {
    table_.Insert(key, value);
    update_range(key);
}

void MemTable::remove(std::string_view key) {
    // Insert tombstone (empty value)
    table_.Insert(key, "");
    update_range(key);
}

std::optional<std::string> MemTable::get(std::string_view key) const {
    std::string_view val;
    if (table_.Get(key, &val)) {
        return std::string(val);
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> MemTable::scan(
    std::string_view start,
    std::string_view end) const 
{
    std::vector<std::pair<std::string, std::string>> out;
    if (start > end) return out;

    auto iter = table_.NewIterator();
    iter.Seek(start);

    while (iter.Valid()) {
        std::string_view k = iter.key();
        if (k > end) break;
        
        out.emplace_back(std::string(k), std::string(iter.value()));
        iter.Next();
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> MemTable::sorted_entries() const {
    std::vector<std::pair<std::string, std::string>> out;
    auto iter = table_.NewIterator();
    iter.SeekToFirst();
    
    while (iter.Valid()) {
        out.emplace_back(std::string(iter.key()), std::string(iter.value()));
        iter.Next();
    }
    return out;
}