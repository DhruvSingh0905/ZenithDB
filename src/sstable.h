#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdint>

/**
 * SSTable - Sorted String Table for persistent storage.
 * 
 * SSTables are immutable on-disk files that store sorted key-value pairs.
 * They use a multi-block layout with:
 * - Data blocks (4KB target size) containing sorted key-value entries
 * - Sparse index mapping min_key -> block offset for fast lookups
 * - Bloom filter for quick negative tests
 * - Footer with metadata and offsets
 * 
 * Features:
 * - Block-based storage for efficient I/O
 * - Bloom filters to skip files that don't contain a key
 * - Range metadata (min_key/max_key) for range pruning
 * - Cached in BlockCache to avoid repeated disk reads
 */
class SSTable {
public:
    /**
     * Metadata about an SSTable file.
     */
    struct Meta {
        std::string   min_key;      ///< Smallest key in this SSTable
        std::string   max_key;      ///< Largest key in this SSTable
        std::uint64_t file_size;    ///< Total file size in bytes
        std::size_t   entry_count;  ///< Number of key-value entries
    };

    /**
     * Opens an existing SSTable from disk.
     * 
     * Reads the file (or retrieves from cache), parses the footer,
     * loads the sparse index, and extracts metadata. The file is
     * cached in BlockCache for future access.
     * 
     * @param path Path to the SSTable file
     * @throws std::runtime_error if the file is corrupt or cannot be opened
     */
    explicit SSTable(const std::filesystem::path& path);
    ~SSTable() = default;

    /**
     * Creates a new SSTable file from sorted entries.
     * 
     * Writes a multi-block SSTable with:
     * - Data blocks (target 4KB each) containing sorted entries
     * - Sparse index mapping block min_keys to offsets
     * - Bloom filter over all keys
     * - Footer with metadata
     * 
     * @param path Where to write the SSTable file
     * @param sorted_entries Key-value pairs, MUST be sorted by key ascending with no duplicates
     * @throws std::runtime_error if the file cannot be created
     */
    static void create(const std::filesystem::path& path,
                       const std::vector<std::pair<std::string, std::string>>& sorted_entries);

    /**
     * Performs a point lookup for a key.
     * 
     * Uses bloom filter for quick rejection, then binary search on
     * the sparse index to find the relevant block, then linear search
     * within that block.
     * 
     * @param key The key to look up
     * @return Optional containing the value, or nullopt if not found
     */
    std::optional<std::string> get(const std::string& key) const;

    /**
     * Performs an inclusive range scan.
     * 
     * Uses range metadata to skip irrelevant blocks, then scans
     * all blocks that might contain keys in the range.
     * 
     * @param start Inclusive start key
     * @param end Inclusive end key
     * @return Vector of (key, value) pairs in sorted order
     */
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const;

    /**
     * Returns metadata about this SSTable.
     * 
     * @return Const reference to the Meta structure
     */
    const Meta& meta() const { return meta_; }

    /**
     * Checks if this SSTable might contain a key (bloom filter).
     * 
     * Returns false if the key is definitely not present (no false negatives).
     * Returns true if the key might be present (may have false positives).
     * 
     * @param key The key to check
     * @return True if the key might be present, false if definitely not
     */
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
    
    /**
     * Computes a bloom filter hash for a key.
     * 
     * Uses FNV-1a hash with a per-hash-function offset to generate
     * multiple independent hash values for the bloom filter.
     * 
     * @param k The key to hash
     * @param i The hash function index (0 to BLOOM_HASHES-1)
     * @return Hash value
     */
    std::uint64_t bloom_hash(const std::string& k, int i) const;

    /**
     * Searches for a key within a specific data block.
     * 
     * Performs linear search through the block entries. Since blocks
     * are sorted, we can stop early if we pass the key.
     * 
     * @param offset Byte offset of the block in the file
     * @param key The key to find
     * @return Optional containing the value, or nullopt if not found
     */
    std::optional<std::string> find_in_block(std::uint64_t offset,
                                             const std::string& key) const;

    /**
     * Scans a data block for keys in a range.
     * 
     * Iterates through all entries in the block and collects those
     * that fall within [start, end] (inclusive).
     * 
     * @param offset Byte offset of the block in the file
     * @param start Inclusive start key
     * @param end Inclusive end key
     * @param out Output vector to append results to
     */
    void scan_block(std::uint64_t offset,
                    const std::string& start,
                    const std::string& end,
                    std::vector<std::pair<std::string, std::string>>& out) const;

    /**
     * Parses the footer and sparse index from the SSTable file.
     * 
     * Reads the footer at the end of the file, validates the magic number,
     * extracts offsets, and loads the sparse index into memory. Also
     * computes metadata like min_key, max_key, and entry_count.
     */
    void parse_footer_and_index();
};