#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstddef>

class MemTable {
public:
    MemTable() = default;

    // Insert / update key with value (non-empty means live value)
    void put(const std::string& key, const std::string& value);

    // Tombstone: empty string means deleted
    void remove(const std::string& key);

    // Point lookup
    std::optional<std::string> get(const std::string& key) const;

    // Inclusive range scan [start, end]
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start,
        const std::string& end) const;

    // All entries sorted by key (for flush/compaction)
    std::vector<std::pair<std::string, std::string>> sorted_entries() const;

    // Very approximate size in bytes; used only for flush thresholds
    std::size_t approximate_size() const { return approx_bytes_; }

    // Key-range metadata (for skipping memtables in ZenithDB::get)
    bool has_range() const { return has_range_; }
    const std::string& min_key() const { return min_key_; }
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

    void update_range(const std::string& key);
};