#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdint>

class SSTable {
public:
    struct Meta {
        std::string   min_key;
        std::string   max_key;
        std::uint64_t file_size   = 0;
        std::size_t   entry_count = 0;
    };

    explicit SSTable(const std::filesystem::path& path);
    ~SSTable() = default;

    // entries MUST be sorted by key ascending, and no duplicate keys
    static void create(const std::filesystem::path& path,
                       const std::vector<std::pair<std::string, std::string>>& sorted_entries);

    // Point lookup
    std::optional<std::string> get(const std::string& key) const;

    // Inclusive range scan [start, end]
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const;

    const Meta& meta() const { return meta_; }

    // Bloom filter quick negative test
    bool may_contain(const std::string& key) const;

private:
    struct IndexEntry {
        std::string   min_key;   // first key in this block
        std::uint64_t offset;    // byte offset of the block in data_
    };

    Meta             meta_;
    std::string      data_;           // entire file contents
    std::uint64_t    data_end_     = 0; // end of data blocks (start of bloom)
    std::uint64_t    bloom_offset_ = 0; // start of bloom region
    std::uint64_t    index_offset_ = 0; // start of index region

    std::vector<IndexEntry> index_;   // in-memory sparse index over blocks

    static constexpr std::size_t BLOOM_BITS_PER_KEY = 10;
    static constexpr int         BLOOM_HASHES       = 7;
    static constexpr std::uint64_t MAGIC            = 0xDB55CA1EULL;
    static constexpr std::size_t   FOOTER_SIZE      = 32; // 4 * u64

    // helpers
    std::uint64_t bloom_hash(const std::string& k, int i) const;

    std::optional<std::string> find_in_block(std::uint64_t offset,
                                             const std::string& key) const;

    void scan_block(std::uint64_t offset,
                    const std::string& start,
                    const std::string& end,
                    std::vector<std::pair<std::string, std::string>>& out) const;

    void parse_footer_and_index();
};