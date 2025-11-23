#pragma once
#include <map>
#include <string>
#include <optional>

class MemTable {
public:
    void put(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;
    void clear();

    // For debugging / future flush
    size_t size() const { return map_.size(); }
    std::vector<std::pair<std::string, std::string>> sorted_entries() const;
    size_t approximate_size() const;
    std::vector<std::pair<std::string, std::string>> scan(const std::string& start, const std::string& end) const;  

private:
    // We'll switch to skiplist later — map is fine for Phase 0-1
    std::map<std::string, std::string> map_;
    static constexpr const char* TOMBSTONE = "";  // empty value = deleted
};