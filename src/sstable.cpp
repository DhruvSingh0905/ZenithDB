#include "sstable.h"
#include "block_cache.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

// Fallback for endian helpers on little-endian platforms
#ifndef htole64
#define htole64(x) (x)
#define le64toh(x) (x)
#endif

#ifndef htole32
#define htole32(x) (x)
#define le32toh(x) (x)
#endif

// Target data block size (approximate)
static constexpr std::size_t BLOCK_TARGET_BYTES = 4096;

// On-disk constants (must match sstable.h)
static constexpr std::uint64_t MAGIC       = 0xDB55CA1EULL;
static constexpr std::size_t   FOOTER_SIZE = 32;  // 4 * uint64_t

// =====================================================================
//  Bloom hash helpers (shared between create() and may_contain())
// =====================================================================

/**
 * Core hash function using FNV-1a algorithm.
 * 
 * FNV-1a is a fast, non-cryptographic hash function that provides
 * good distribution for bloom filters.
 * 
 * @param k The key to hash
 * @return 64-bit hash value
 */
static inline std::uint64_t bloom_hash_core(const std::string& k) {
    // FNV-1a 64-bit
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : k) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

/**
 * Generates multiple hash values for bloom filter.
 * 
 * Uses the core hash with different offsets to generate independent
 * hash functions for the bloom filter.
 * 
 * @param k The key to hash
 * @param i The hash function index
 * @return Hash value for this hash function
 */
static inline std::uint64_t bloom_hash_static(const std::string& k, int i) {
    std::uint64_t base = bloom_hash_core(k);
    return base + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
}

/**
 * Member function version of bloom hash (used by may_contain).
 */
std::uint64_t SSTable::bloom_hash(const std::string& k, int i) const {
    std::uint64_t base = bloom_hash_core(k);
    return base + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
}

/**
 * Checks if a key might be in this SSTable using the bloom filter.
 * 
 * Returns false only if the key is definitely not present (no false negatives).
 * Returns true if the key might be present (may have false positives).
 */
bool SSTable::may_contain(const std::string& key) const {
    // No bloom region → say "maybe yes"
    if (bloom_offset_ == 0 || index_offset_ <= bloom_offset_) {
        return true;
    }

    const std::size_t bloom_bytes =
        static_cast<std::size_t>(index_offset_ - bloom_offset_);
    if (bloom_bytes == 0) return true;

    const std::size_t bloom_bits = bloom_bytes * 8;
    const unsigned char* bloom =
        reinterpret_cast<const unsigned char*>(data_.data() + bloom_offset_);

    for (int i = 0; i < BLOOM_HASHES; ++i) {
        std::uint64_t bit = bloom_hash(key, i) % bloom_bits;
        std::size_t byte_index = static_cast<std::size_t>(bit >> 3);
        std::size_t bit_index  = static_cast<std::size_t>(bit & 7);

        if ((bloom[byte_index] & (1u << bit_index)) == 0) {
            return false;  // definitely not present
        }
    }
    return true;  // possibly present
}

// =====================================================================
//  Small helper: compare key span (ptr,len) vs std::string
// =====================================================================

/**
 * Compares a key stored as (pointer, length) with a std::string.
 * 
 * Used to avoid allocating strings when comparing keys during block scans.
 * 
 * @param a Pointer to the first key
 * @param alen Length of the first key
 * @param b The second key as a string
 * @return <0 if a < b, 0 if a == b, >0 if a > b
 */
static int compare_key_span_to_string(const char* a, std::uint32_t alen,
                                      const std::string& b) {
    std::size_t blen = b.size();
    std::size_t n    = (alen < blen) ? alen : blen;

    int cmp = std::memcmp(a, b.data(), n);
    if (cmp != 0) {
        return cmp;  // <0 if a<b, >0 if a>b
    }
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;       // equal
}

// =====================================================================
//  Constructor & footer/index parsing
// =====================================================================

/**
 * Constructs an SSTable by loading it from disk.
 * 
 * Attempts to load from BlockCache first, otherwise reads from disk
 * and caches it. Then parses the footer and index.
 */
SSTable::SSTable(const std::filesystem::path& p) {
    const std::string key = p.string();

    // Try to get file contents from global cache
    auto& cache  = BlockCache::instance();
    auto  cached = cache.get(key);

    if (cached) {
        data_ = *cached;
    } else {
        // Read entire file from disk, then cache it
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open sstable: " + p.string());
        }

        in.seekg(0, std::ios::end);
        std::size_t sz = static_cast<std::size_t>(in.tellg());
        in.seekg(0);

        data_.resize(sz);
        if (sz > 0) {
            in.read(&data_[0], sz);
        }

        cache.put(key, std::make_shared<std::string>(data_));
    }

    if (data_.size() < FOOTER_SIZE) {
        throw std::runtime_error("corrupt sstable: too small");
    }

    meta_.file_size = data_.size();
    parse_footer_and_index();
}

/**
 * Parses the footer and sparse index from the SSTable file.
 * 
 * Reads the footer at the end of the file, validates magic number,
 * extracts offsets, and loads the sparse index. Also computes metadata
 * like min_key, max_key, and entry_count by examining block headers.
 */
void SSTable::parse_footer_and_index() {
    const std::size_t sz   = data_.size();
    const char*       base = data_.data();

    // Footer: [data_end][bloom_off][index_off][magic]
    const std::uint64_t* f =
        reinterpret_cast<const std::uint64_t*>(base + sz - FOOTER_SIZE);

    data_end_     = le64toh(f[0]);
    bloom_offset_ = le64toh(f[1]);
    index_offset_ = le64toh(f[2]);
    std::uint64_t magic = le64toh(f[3]);

    if (magic != MAGIC) {
        throw std::runtime_error("corrupt sstable: bad magic");
    }

    if (data_end_ > sz ||
        bloom_offset_ > sz ||
        index_offset_ > sz ||
        data_end_ > bloom_offset_ ||
        bloom_offset_ > index_offset_) {
        throw std::runtime_error("corrupt sstable: bad offsets");
    }

    index_.clear();
    meta_.min_key.clear();
    meta_.max_key.clear();
    meta_.entry_count = 0;

    // If index_offset_ == end-of-file - footer_size, there is no index region.
    if (index_offset_ == sz - FOOTER_SIZE) {
        // Single-block file at offset 0. We can derive min/max & count by scanning.
        // This is cold-path (file open), so a full scan here is fine.
        auto all = scan("", "\xFF\xFF\xFF\xFF");
        for (const auto& kv : all) {
            const auto& k = kv.first;
            const auto& v = kv.second;
            if (v.empty()) continue; // ignore tombstones in stats
            if (meta_.min_key.empty() || k < meta_.min_key) meta_.min_key = k;
            if (meta_.max_key.empty() || k > meta_.max_key) meta_.max_key = k;
            meta_.entry_count++;
        }
        return;
    }

    // Parse index region
    const char* p     = base + index_offset_;
    const char* limit = base + sz - FOOTER_SIZE;

    if (p + sizeof(std::uint32_t) > limit) {
        throw std::runtime_error("corrupt sstable: index truncated");
    }

    std::uint32_t block_count_le;
    std::memcpy(&block_count_le, p, sizeof(block_count_le));
    p += sizeof(block_count_le);
    std::uint32_t block_count = le32toh(block_count_le);

    index_.reserve(block_count);

    for (std::uint32_t i = 0; i < block_count; ++i) {
        if (p + sizeof(std::uint32_t) > limit) {
            throw std::runtime_error("corrupt sstable: index key_len truncated");
        }

        std::uint32_t key_len_le;
        std::memcpy(&key_len_le, p, sizeof(key_len_le));
        p += sizeof(key_len_le);
        std::uint32_t key_len = le32toh(key_len_le);

        if (p + key_len > limit) {
            throw std::runtime_error("corrupt sstable: index key truncated");
        }

        std::string min_key(p, p + key_len);
        p += key_len;

        if (p + sizeof(std::uint64_t) > limit) {
            throw std::runtime_error("corrupt sstable: index offset truncated");
        }

        std::uint64_t off_le;
        std::memcpy(&off_le, p, sizeof(off_le));
        p += sizeof(off_le);
        std::uint64_t off = le64toh(off_le);

        index_.push_back({std::move(min_key), off});
    }

    if (index_.empty()) {
        return;
    }

    // min_key from first index entry
    meta_.min_key = index_.front().min_key;

    // Count entries & get max_key by walking each block header.
    // This is done once at open time, so cost is acceptable.
    for (std::size_t bi = 0; bi < index_.size(); ++bi) {
        std::uint64_t off = index_[bi].offset;
        if (off + 8 > data_end_) continue;

        const char* bp = data_.data() + off;
        std::uint32_t num_le, bsize_le;
        std::memcpy(&num_le,   bp,     4);
        std::memcpy(&bsize_le, bp + 4, 4);
        std::uint32_t num   = le32toh(num_le);
        std::uint32_t bsize = le32toh(bsize_le);

        const char* payload = bp + 8;
        const char* bend    = payload + bsize;
        if (bend > base + data_end_) bend = base + data_end_;

        meta_.entry_count += num;

        if (bi == index_.size() - 1) {
            // Last block: walk keys to find the lexicographically largest key.
            std::string last_key;

            for (std::uint32_t i = 0; i < num; ++i) {
                if (payload + 4 > bend) break;

                std::uint32_t klen_le;
                std::memcpy(&klen_le, payload, 4);
                payload += 4;
                std::uint32_t klen = le32toh(klen_le);
                if (payload + klen > bend) break;

                last_key.assign(payload, payload + klen);
                payload += klen;

                if (payload + 4 > bend) break;
                std::uint32_t vlen_le;
                std::memcpy(&vlen_le, payload, 4);
                payload += 4;
                std::uint32_t vlen = le32toh(vlen_le);
                if (payload + vlen > bend) break;
                payload += vlen;
            }

            meta_.max_key = std::move(last_key);
        }
    }
}

// =====================================================================
//  Create SSTable (multi-block layout + sparse index + bloom + footer)
// =====================================================================

/**
 * Creates a new SSTable file from sorted key-value entries.
 * 
 * Builds a multi-block SSTable with:
 * 1. Data blocks (target 4KB each) containing sorted entries
 * 2. Sparse index mapping block min_keys to offsets
 * 3. Bloom filter over all keys
 * 4. Footer with metadata and offsets
 * 
 * The entries must be sorted by key and have no duplicates.
 */
void SSTable::create(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries_in)
{
    if (entries_in.empty()) return;

    // 0) Make a sorted copy by key to guarantee on-disk order
    std::vector<std::pair<std::string, std::string>> entries = entries_in;
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });

    std::string data;
    std::vector<IndexEntry> index_entries;
    index_entries.reserve(entries.size() / 64 + 1);

    const std::size_t n = entries.size();
    std::size_t i = 0;

    while (i < n) {
        // Start a new block at current data size
        std::uint64_t block_offset = data.size();
        std::size_t header_pos     = data.size();

        // Reserve space for header: [num_entries(u32)][block_size(u32)]
        data.resize(data.size() + 8);

        std::uint32_t num_entries  = 0;
        std::size_t   payload_start = data.size();

        std::string block_min_key;

        while (i < n) {
            const auto& kv = entries[i];
            const std::string& k = kv.first;
            const std::string& v = kv.second;

            std::uint32_t klen = static_cast<std::uint32_t>(k.size());
            std::uint32_t vlen = static_cast<std::uint32_t>(v.size());

            std::size_t entry_size = 4 + klen + 4 + vlen;

            // If we would exceed target block size, close block (if it has entries)
            if (num_entries > 0 &&
                data.size() + entry_size > block_offset + BLOCK_TARGET_BYTES) {
                break;
            }

            if (num_entries == 0) {
                block_min_key = k;  // first key in this block
            }

            std::uint32_t klen_le = htole32(klen);
            std::uint32_t vlen_le = htole32(vlen);

            data.append(reinterpret_cast<char*>(&klen_le), 4);
            data.append(k.data(), klen);
            data.append(reinterpret_cast<char*>(&vlen_le), 4);
            data.append(v.data(), vlen);

            num_entries++;
            i++;
        }

        std::uint32_t block_size =
            static_cast<std::uint32_t>(data.size() - payload_start);

        // Fill header
        std::uint32_t num_le  = htole32(num_entries);
        std::uint32_t size_le = htole32(block_size);
        std::memcpy(&data[header_pos],     &num_le,  4);
        std::memcpy(&data[header_pos + 4], &size_le, 4);

        // Add sparse index entry
        index_entries.push_back({block_min_key, block_offset});
    }

    std::uint64_t data_end = data.size();

    // 2) Bloom filter over all keys
    std::size_t total_keys  = entries.size();
    std::size_t bloom_bits  =
        std::max<std::size_t>(total_keys * BLOOM_BITS_PER_KEY, 8);
    std::size_t bloom_bytes = (bloom_bits + 7) / 8;

    std::string bloom(bloom_bytes, 0);
    auto set_bloom = [&](const std::string& k) {
        for (int i = 0; i < BLOOM_HASHES; ++i) {
            std::uint64_t bit = bloom_hash_static(k, i) % bloom_bits;
            std::size_t byte_index = static_cast<std::size_t>(bit >> 3);
            std::size_t bit_index  = static_cast<std::size_t>(bit & 7);
            bloom[byte_index] |= (1u << bit_index);
        }
    };
    for (const auto& kv : entries) {
        set_bloom(kv.first);
    }

    // 3) Build index region
    std::string index_region;
    {
        std::uint32_t block_count =
            static_cast<std::uint32_t>(index_entries.size());
        std::uint32_t block_count_le = htole32(block_count);
        index_region.append(reinterpret_cast<char*>(&block_count_le), 4);

        for (const auto& ie : index_entries) {
            std::uint32_t klen    =
                static_cast<std::uint32_t>(ie.min_key.size());
            std::uint32_t klen_le = htole32(klen);
            index_region.append(reinterpret_cast<char*>(&klen_le), 4);
            index_region.append(ie.min_key.data(), klen);

            std::uint64_t off_le = htole64(ie.offset);
            index_region.append(reinterpret_cast<char*>(&off_le), 8);
        }
    }

    // 4) Write everything to file
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create sstable: " + path.string());
    }

    out.write(data.data(), data.size());
    std::uint64_t bloom_off = data.size();
    out.write(bloom.data(), bloom.size());
    std::uint64_t index_off = bloom_off + bloom.size();
    out.write(index_region.data(), index_region.size());

    // 5) Footer
    std::uint64_t footer[4];
    footer[0] = htole64(data_end);
    footer[1] = htole64(bloom_off);
    footer[2] = htole64(index_off);
    footer[3] = htole64(MAGIC);

    out.write(reinterpret_cast<char*>(footer), FOOTER_SIZE);
}

// =====================================================================
//  Block-level helpers (optimized for fewer allocations)
// =====================================================================

/**
 * Searches for a key within a specific data block.
 * 
 * Parses the block header, then performs linear search through entries.
 * Since entries are sorted, can stop early if we pass the key.
 */
std::optional<std::string> SSTable::find_in_block(
    std::uint64_t offset,
    const std::string& key) const
{
    if (offset + 8 > data_end_) return std::nullopt;

    const char* base = data_.data();
    const char* bp   = base + offset;

    std::uint32_t num_le, bsize_le;
    std::memcpy(&num_le,   bp,     4);
    std::memcpy(&bsize_le, bp + 4, 4);
    std::uint32_t num   = le32toh(num_le);
    std::uint32_t bsize = le32toh(bsize_le);

    const char* payload = bp + 8;
    const char* bend    = payload + bsize;
    if (bend > base + data_end_) bend = base + data_end_;

    for (std::uint32_t i = 0; i < num; ++i) {
        if (payload + 4 > bend) break;

        // key length
        std::uint32_t klen_le;
        std::memcpy(&klen_le, payload, 4);
        payload += 4;
        std::uint32_t klen = le32toh(klen_le);
        if (payload + klen > bend) break;

        const char* key_ptr = payload;
        payload += klen;

        // value length
        if (payload + 4 > bend) break;
        std::uint32_t vlen_le;
        std::memcpy(&vlen_le, payload, 4);
        payload += 4;
        std::uint32_t vlen = le32toh(vlen_le);
        if (payload + vlen > bend) break;

        const char* val_ptr = payload;

        int cmp = compare_key_span_to_string(key_ptr, klen, key);
        if (cmp == 0) {
            // Only allocate value string on match
            return std::string(val_ptr, val_ptr + vlen);
        } else if (cmp > 0) {
            // On-disk key > probe key; keys sorted, so stop early.
            return std::nullopt;
        }

        payload += vlen;
    }

    return std::nullopt;
}

/**
 * Scans a data block for keys in the specified range.
 * 
 * Parses the block and collects all entries where start <= key <= end.
 * Can stop early if we pass the end key (since entries are sorted).
 */
void SSTable::scan_block(
    std::uint64_t offset,
    const std::string& start,
    const std::string& end,
    std::vector<std::pair<std::string, std::string>>& out) const
{
    if (offset + 8 > data_end_) return;

    const char* base = data_.data();
    const char* bp   = base + offset;

    std::uint32_t num_le, bsize_le;
    std::memcpy(&num_le,   bp,     4);
    std::memcpy(&bsize_le, bp + 4, 4);
    std::uint32_t num   = le32toh(num_le);
    std::uint32_t bsize = le32toh(bsize_le);

    const char* payload = bp + 8;
    const char* bend    = payload + bsize;
    if (bend > base + data_end_) bend = base + data_end_;

    for (std::uint32_t i = 0; i < num; ++i) {
        if (payload + 4 > bend) break;

        // key length
        std::uint32_t klen_le;
        std::memcpy(&klen_le, payload, 4);
        payload += 4;
        std::uint32_t klen = le32toh(klen_le);
        if (payload + klen > bend) break;

        const char* key_ptr = payload;
        payload += klen;

        // value length
        if (payload + 4 > bend) break;
        std::uint32_t vlen_le;
        std::memcpy(&vlen_le, payload, 4);
        payload += 4;
        std::uint32_t vlen = le32toh(vlen_le);
        if (payload + vlen > bend) break;

        const char* val_ptr = payload;

        // If key > end, we can stop scanning this block.
        if (compare_key_span_to_string(key_ptr, klen, end) > 0) {
            return;
        }

        // Check start <= key <= end
        if (compare_key_span_to_string(key_ptr, klen, start) >= 0) {
            std::string key_str(key_ptr, key_ptr + klen);
            std::string val_str(val_ptr, val_ptr + vlen);
            out.emplace_back(std::move(key_str), std::move(val_str));
        }

        payload += vlen;
    }
}

// =====================================================================
//  Public get()/scan() using sparse index
// =====================================================================

/**
 * Performs a point lookup using bloom filter and sparse index.
 * 
 * First checks the bloom filter to quickly reject keys that aren't present.
 * Then uses binary search on the sparse index to find the relevant block,
 * then searches within that block.
 */
std::optional<std::string> SSTable::get(const std::string& key) const {
    // Bloom-based quick negative check
    if (!may_contain(key)) {
        return std::nullopt;
    }

    // We rely on the caller (ZenithDB) for range pruning via meta().
    // Here we only use the per-file sparse index.

    if (index_.empty()) {
        // No index region -> single-block file starting at offset 0
        return find_in_block(0, key);
    }

    // Binary search index_ to find block whose min_key <= key < next.min_key
    auto it = std::upper_bound(
        index_.begin(), index_.end(), key,
        [](const std::string& k, const IndexEntry& ie) {
            return k < ie.min_key;
        });

    if (it == index_.begin()) {
        // key is before first min_key => candidate is first block
        return find_in_block(index_.front().offset, key);
    }

    --it; // block with min_key <= key
    return find_in_block(it->offset, key);
}

/**
 * Performs a range scan using range metadata and sparse index.
 * 
 * Uses min_key/max_key to skip irrelevant files, then uses the sparse
 * index to find relevant blocks, then scans those blocks.
 */
std::vector<std::pair<std::string, std::string>> SSTable::scan(
    const std::string& start,
    const std::string& end) const
{
    std::vector<std::pair<std::string, std::string>> out;
    if (start > end) return out;

    // Range pruning with global min/max if available
    if (!meta_.max_key.empty() && start > meta_.max_key) return out;
    if (!meta_.min_key.empty() && end   < meta_.min_key) return out;

    if (index_.empty()) {
        // Single-block file; just scan whole block and filter by [start,end]
        scan_block(0, start, end, out);
        return out;
    }

    // Find first potentially relevant block (min_key <= start or first > start)
    auto it = std::upper_bound(
        index_.begin(), index_.end(), start,
        [](const std::string& k, const IndexEntry& ie) {
            return k < ie.min_key;
        });

    if (it != index_.begin()) {
        --it; // start from block whose min_key <= start
    }

    for (; it != index_.end(); ++it) {
        // If this block's min_key is already > end, we can stop
        if (it->min_key > end) break;
        scan_block(it->offset, start, end, out);
    }

    return out;
}