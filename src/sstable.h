// src/sstable.h
#pragma once

#include "env.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <string_view>
#include <cstdint>
#include <memory>

class SSTable {
public:
    struct Meta {
        std::string   min_key;
        std::string   max_key;
        std::uint64_t file_size;
        std::size_t   entry_count;
    };

    // Constructor now takes Env
    explicit SSTable(Env* env, const std::filesystem::path& path);
    ~SSTable();

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;

    // Static creator now takes Env
    static void create(Env* env, const std::filesystem::path& path,
                       const std::vector<std::pair<std::string, std::string>>& sorted_entries);

    std::optional<std::string> get(std::string_view key) const;

    std::vector<std::pair<std::string, std::string>> scan(
        std::string_view start, std::string_view end) const;

    const Meta& meta() const { return meta_; }
    bool may_contain(std::string_view key) const;

    static std::uint64_t bloom_hash(std::string_view k, int i);

private:
    struct IndexEntry {
        std::string   min_key;
        std::uint64_t offset;
    };

    Env* env_;
    Meta             meta_;
    std::unique_ptr<RandomAccessFile> file_;

    std::uint64_t    data_end_     = 0;
    std::uint64_t    bloom_offset_ = 0;
    std::uint64_t    index_offset_ = 0;

    std::vector<IndexEntry> index_;
    std::vector<unsigned char> bloom_filter_; 

    static constexpr std::size_t BLOOM_BITS_PER_KEY = 10;
    static constexpr int         BLOOM_HASHES       = 7;
    static constexpr std::uint64_t MAGIC            = 0xDB55CA1EULL;
    static constexpr std::size_t   FOOTER_SIZE      = 32;

    // Helper to read bytes from random access file
    std::string read_bytes(std::uint64_t offset, std::size_t n) const;

    std::optional<std::string> find_in_block(std::uint64_t offset,
                                             std::string_view key) const;

    void scan_block(std::uint64_t offset,
                    std::string_view start,
                    std::string_view end,
                    std::vector<std::pair<std::string, std::string>>& out) const;

    void parse_footer_and_index();
};