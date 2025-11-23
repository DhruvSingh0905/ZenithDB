#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

class BlockCache;

class SSTable {
public:
    struct Meta {
        std::string min_key;
        std::string max_key;
        uint64_t file_size = 0;
        size_t entry_count = 0;
    };

    explicit SSTable(const std::filesystem::path& path);
    ~SSTable() = default;

    // entries must be sorted by key
    static void create(const std::filesystem::path& path,
                       const std::vector<std::pair<std::string, std::string>>& sorted_entries);

    std::optional<std::string> get(const std::string& key) const;

    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const;

    const Meta& meta() const { return meta_; }

    bool may_contain(const std::string& key) const;

private:
    Meta meta_;
    std::string data_;     // entire file contents in memory
    size_t data_end_ = 0;  // end of data region (start of bloom)
    size_t bloom_offset_ = 0;
    size_t index_offset_ = 0;  // offset at which (optional) index begins

    static constexpr size_t BLOOM_BITS_PER_KEY = 10;
    static constexpr int BLOOM_HASHES = 7;

    uint64_t bloom_hash(const std::string& k, int i) const;
};