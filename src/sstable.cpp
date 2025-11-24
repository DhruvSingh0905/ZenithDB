#include "sstable.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdexcept>
#include <fstream>
#include <iostream>

#ifndef htole64
#define htole64(x) (x)
#define le64toh(x) (x)
#endif
#ifndef htole32
#define htole32(x) (x)
#define le32toh(x) (x)
#endif

// Constants
static constexpr std::size_t BLOCK_TARGET_BYTES = 4096;  // Target block size (4KB, matches page size)
static constexpr std::size_t RESTART_INTERVAL = 16;      // Restart point every 16 entries (for binary search) 

/**
 * Core hash function for bloom filter (FNV-1a variant).
 * 
 * Uses FNV-1a hash algorithm, which is fast and provides good distribution.
 * This is a critical low-level optimization for bloom filter performance.
 */
static inline std::uint64_t bloom_hash_core(std::string_view k) {
    std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (unsigned char c : k) {
        h ^= c;
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

// Now static
std::uint64_t SSTable::bloom_hash(std::string_view k, int i) {
    std::uint64_t base = bloom_hash_core(k);
    return base + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
}

bool SSTable::may_contain(std::string_view key) const {
    if (bloom_offset_ == 0 || index_offset_ <= bloom_offset_) return true;
    const std::size_t bloom_bytes = index_offset_ - bloom_offset_;
    if (bloom_bytes == 0) return true;
    const std::size_t bloom_bits = bloom_bytes * 8;
    const unsigned char* bloom = reinterpret_cast<const unsigned char*>(data_ptr_ + bloom_offset_);

    for (int i = 0; i < BLOOM_HASHES; ++i) {
        std::uint64_t bit = bloom_hash(key, i) % bloom_bits;
        if ((bloom[bit >> 3] & (1u << (bit & 7))) == 0) return false;
    }
    return true;
}

// =====================================================================
//  Constructor: Open and Mmap
// =====================================================================

SSTable::SSTable(const std::filesystem::path& p) {
    // Open file for reading
    fd_ = open(p.c_str(), O_RDONLY);
    if (fd_ == -1) {
        throw std::runtime_error("sstable open failed: " + p.string());
    }

    // Get file size
    struct stat sb;
    if (fstat(fd_, &sb) == -1) {
        close(fd_);
        throw std::runtime_error("sstable fstat failed");
    }
    data_size_ = static_cast<std::size_t>(sb.st_size);

    // CRITICAL OPTIMIZATION: Memory-map the entire file for zero-copy reads.
    // This eliminates system call overhead and enables the OS to cache pages
    // efficiently. MAP_PRIVATE means writes don't affect the file.
    if (data_size_ > 0) {
        void* map = mmap(nullptr, data_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (map == MAP_FAILED) {
            close(fd_);
            throw std::runtime_error("sstable mmap failed");
        }
        data_ptr_ = static_cast<char*>(map);
    }
    
    // CRITICAL FIX: Close FD immediately after mmap to avoid hitting ulimit.
    // The mmap remains valid even after closing the FD. This prevents file
    // descriptor exhaustion when opening many SSTables.
    close(fd_);
    fd_ = -1; 

    if (data_size_ < FOOTER_SIZE) {
        throw std::runtime_error("sstable too small");
    }

    meta_.file_size = data_size_;
    
    // Parse footer and sparse index (builds in-memory index for fast lookups)
    parse_footer_and_index();
}

SSTable::~SSTable() {
    if (data_ptr_) {
        munmap(data_ptr_, data_size_);
    }
    // fd_ is already closed in constructor
    if (fd_ != -1) {
        close(fd_);
    }
}

void SSTable::parse_footer_and_index() {
    const char* base = data_ptr_;
    const std::uint64_t* f = reinterpret_cast<const std::uint64_t*>(base + data_size_ - FOOTER_SIZE);

    data_end_     = le64toh(f[0]);
    bloom_offset_ = le64toh(f[1]);
    index_offset_ = le64toh(f[2]);
    std::uint64_t magic = le64toh(f[3]);

    if (magic != MAGIC) throw std::runtime_error("bad magic");

    index_.clear();
    meta_.entry_count = 0;

    if (index_offset_ == data_size_ - FOOTER_SIZE) {
        auto all = scan("", "\xFF\xFF");
        if (!all.empty()) {
            meta_.min_key = all.front().first;
            meta_.max_key = all.back().first;
            meta_.entry_count = all.size();
        }
        return;
    }

    const char* p = base + index_offset_;
    // Safety check commented out to trust footer offset, but good to have in prod
    // const char* limit = base + data_size_ - FOOTER_SIZE;

    std::uint32_t block_count;
    std::memcpy(&block_count, p, 4); p += 4;
    block_count = le32toh(block_count);
    index_.reserve(block_count);

    for (std::uint32_t i = 0; i < block_count; ++i) {
        std::uint32_t key_len;
        std::memcpy(&key_len, p, 4); p += 4;
        key_len = le32toh(key_len);
        
        std::string min_key(p, key_len); p += key_len;
        
        std::uint64_t off;
        std::memcpy(&off, p, 8); p += 8;
        off = le64toh(off);

        index_.push_back({std::move(min_key), off});
    }

    if (!index_.empty()) {
        meta_.min_key = index_.front().min_key;
        meta_.entry_count = 1; // Approximate
    }
}

// =====================================================================
//  Create with Restart Points
// =====================================================================

void SSTable::create(const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("create failed: " + path.string());

    std::string data_buf;
    std::vector<IndexEntry> index_entries;
    std::vector<std::uint32_t> restarts;
    
    std::size_t i = 0;
    while (i < entries.size()) {
        std::uint64_t block_start_pos = out.tellp();
        
        data_buf.clear();
        restarts.clear();
        restarts.push_back(8); // First entry starts at offset 8 (after header)

        std::uint32_t num_entries = 0;
        std::string min_key = entries[i].first;

        data_buf.resize(8); // Reserve header

        while (i < entries.size()) {
            const auto& kv = entries[i];
            
            // Add restart point every 16 entries
            if (num_entries > 0 && num_entries % RESTART_INTERVAL == 0) {
                restarts.push_back(static_cast<std::uint32_t>(data_buf.size()));
            }

            std::uint32_t klen = kv.first.size();
            std::uint32_t vlen = kv.second.size();
            
            if (num_entries > 0 && data_buf.size() + 8 + klen + vlen + restarts.size()*4 > BLOCK_TARGET_BYTES) {
                break;
            }

            std::uint32_t tmp;
            tmp = htole32(klen); data_buf.append((char*)&tmp, 4);
            data_buf.append(kv.first);
            tmp = htole32(vlen); data_buf.append((char*)&tmp, 4);
            data_buf.append(kv.second);
            
            num_entries++;
            i++;
        }

        // Write restarts block
        for (auto r : restarts) {
            std::uint32_t r_le = htole32(r);
            data_buf.append((char*)&r_le, 4);
        }
        std::uint32_t num_restarts = htole32(static_cast<std::uint32_t>(restarts.size()));
        data_buf.append((char*)&num_restarts, 4);

        std::uint32_t final_size = static_cast<std::uint32_t>(data_buf.size());
        std::uint32_t n_le = htole32(num_entries);
        std::uint32_t sz_le = htole32(final_size); 
        
        std::memcpy(&data_buf[0], &n_le, 4);
        std::memcpy(&data_buf[4], &sz_le, 4);

        out.write(data_buf.data(), data_buf.size());
        index_entries.push_back({min_key, block_start_pos});
    }

    std::uint64_t data_end = out.tellp();

    // Bloom Filter
    std::size_t total_keys = entries.size();
    std::size_t bloom_bits = std::max<std::size_t>(total_keys * BLOOM_BITS_PER_KEY, 8);
    std::vector<unsigned char> bloom((bloom_bits + 7) / 8, 0);
    
    for (const auto& kv : entries) {
        for (int h = 0; h < BLOOM_HASHES; ++h) {
            // FIX: Call static bloom_hash directly
            std::uint64_t bit = SSTable::bloom_hash(kv.first, h) % bloom_bits; 
            bloom[bit/8] |= (1 << (bit%8));
        }
    }

    std::uint64_t bloom_offset = out.tellp();
    out.write(reinterpret_cast<char*>(bloom.data()), bloom.size());

    // Sparse Index
    std::uint64_t index_offset = out.tellp();
    std::uint32_t count = htole32(static_cast<std::uint32_t>(index_entries.size()));
    out.write((char*)&count, 4);
    for (const auto& ie : index_entries) {
        std::uint32_t klen = htole32(static_cast<std::uint32_t>(ie.min_key.size()));
        out.write((char*)&klen, 4);
        out.write(ie.min_key.data(), ie.min_key.size());
        std::uint64_t off = htole64(ie.offset);
        out.write((char*)&off, 8);
    }

    // Footer
    std::uint64_t footer[4];
    footer[0] = htole64(data_end);
    footer[1] = htole64(bloom_offset);
    footer[2] = htole64(index_offset);
    footer[3] = htole64(MAGIC);
    out.write((char*)footer, FOOTER_SIZE);
}

// =====================================================================
//  Block Search with Restarts
// =====================================================================

/**
 * Searches for a key within a data block using restart points.
 * 
 * CRITICAL OPTIMIZATION: Uses binary search on restart points to narrow
 * down the search range, then linear scans only the relevant portion.
 * This reduces average search time from O(n) to O(log(n/16) + 16).
 * 
 * Restart points are stored at the end of each block, allowing binary
 * search to find the approximate location, then linear scan from there.
 * 
 * @param offset File offset of the block
 * @param key The key to search for
 * @return Optional containing the value, or nullopt if not found
 */
std::optional<std::string> SSTable::find_in_block(
    std::uint64_t offset,
    std::string_view key) const
{
    if (offset + 8 > data_end_) return std::nullopt;
    const char* bp = data_ptr_ + offset;  // Block pointer (memory-mapped)

    // Read block header: [num_entries (u32)][block_size (u32)]
    std::uint32_t num_entries;
    std::uint32_t block_size;
    std::memcpy(&num_entries, bp, 4); num_entries = le32toh(num_entries);
    std::memcpy(&block_size, bp+4, 4); block_size = le32toh(block_size);

    if (offset + block_size > data_end_) return std::nullopt;

    // Find restart points array at end of block
    const char* block_end = bp + block_size;
    std::uint32_t num_restarts;
    std::memcpy(&num_restarts, block_end - 4, 4);
    num_restarts = le32toh(num_restarts);

    const char* restarts_ptr = block_end - 4 - (num_restarts * 4);
    
    // BINARY SEARCH on restart points to find approximate location
    // This is a critical optimization that reduces search time significantly
    std::uint32_t left = 0, right = num_restarts;
    while (left < right) {
        std::uint32_t mid = (left + right) / 2;
        std::uint32_t restart_off;
        std::memcpy(&restart_off, restarts_ptr + mid*4, 4);
        restart_off = le32toh(restart_off);

        // Decode key at restart point (format: [klen (u32)][key]...)
        const char* entry = bp + restart_off;
        std::uint32_t klen;
        std::memcpy(&klen, entry, 4); klen = le32toh(klen);
        std::string_view mid_key(entry + 4, klen);  // No allocation
        
        if (mid_key < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    // Start linear scan from the restart point before our binary search result
    std::uint32_t start_idx = (left > 0) ? left - 1 : 0;
    
    std::uint32_t restart_off;
    std::memcpy(&restart_off, restarts_ptr + start_idx*4, 4);
    restart_off = le32toh(restart_off);

    // LINEAR SCAN from restart point (at most RESTART_INTERVAL entries)
    const char* ptr = bp + restart_off;
    const char* limit = restarts_ptr; 

    while (ptr < limit) {
        // Decode entry: [klen (u32)][key][vlen (u32)][value]
        std::uint32_t klen;
        std::memcpy(&klen, ptr, 4); klen = le32toh(klen);
        ptr += 4;
        std::string_view entry_key(ptr, klen);  // No allocation
        ptr += klen;

        std::uint32_t vlen;
        std::memcpy(&vlen, ptr, 4); vlen = le32toh(vlen);
        ptr += 4;
        std::string_view entry_val(ptr, vlen);  // No allocation
        ptr += vlen;

        int cmp = entry_key.compare(key);
        if (cmp == 0) return std::string(entry_val);  // Found!
        if (cmp > 0) return std::nullopt;  // Passed the key (entries are sorted)
    }
    return std::nullopt;  // Not found
}

void SSTable::scan_block(
    std::uint64_t offset,
    std::string_view start,
    std::string_view end,
    std::vector<std::pair<std::string, std::string>>& out) const
{
    if (offset + 8 > data_end_) return;
    const char* bp = data_ptr_ + offset;
    
    std::uint32_t block_size;
    std::memcpy(&block_size, bp+4, 4); block_size = le32toh(block_size);
    
    const char* block_end = bp + block_size;
    std::uint32_t num_restarts;
    std::memcpy(&num_restarts, block_end - 4, 4); num_restarts = le32toh(num_restarts);
    const char* restarts_ptr = block_end - 4 - (num_restarts * 4);

    // Binary search for 'start'
    std::uint32_t left = 0, right = num_restarts;
    while (left < right) {
        std::uint32_t mid = (left + right) / 2;
        std::uint32_t roff;
        std::memcpy(&roff, restarts_ptr + mid*4, 4); roff = le32toh(roff);
        
        std::uint32_t klen;
        std::memcpy(&klen, bp + roff, 4); klen = le32toh(klen);
        std::string_view mid_key(bp + roff + 4, klen);
        
        if (mid_key < start) left = mid + 1;
        else right = mid;
    }
    
    std::uint32_t start_idx = (left > 0) ? left - 1 : 0;
    std::uint32_t scan_off;
    std::memcpy(&scan_off, restarts_ptr + start_idx*4, 4); scan_off = le32toh(scan_off);

    const char* ptr = bp + scan_off;
    const char* limit = restarts_ptr;

    while (ptr < limit) {
        std::uint32_t klen;
        std::memcpy(&klen, ptr, 4); klen = le32toh(klen);
        ptr += 4;
        std::string_view k(ptr, klen);
        ptr += klen;

        std::uint32_t vlen;
        std::memcpy(&vlen, ptr, 4); vlen = le32toh(vlen);
        ptr += 4;
        std::string_view v(ptr, vlen);
        ptr += vlen;

        if (k > end) return;
        if (k >= start) {
            out.emplace_back(std::string(k), std::string(v));
        }
    }
}

std::optional<std::string> SSTable::get(std::string_view key) const {
    // Step 1: Bloom filter check (fast negative test)
    // CRITICAL OPTIMIZATION: Rejects ~99% of non-existent keys with a few
    // bit checks, avoiding expensive disk I/O and index lookups.
    if (!may_contain(key)) return std::nullopt;
    
    // Step 2: If no sparse index, search the single block
    if (index_.empty()) return find_in_block(0, key);

    // Step 3: Binary search sparse index to find relevant block
    // CRITICAL OPTIMIZATION: Sparse index allows binary search to find
    // the block containing the key, avoiding scanning the entire file.
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
        [](std::string_view k, const IndexEntry& ie) {
            return k < ie.min_key;
        });
    
    // If key is before first block, check first block anyway
    if (it == index_.begin()) return find_in_block(index_.front().offset, key);
    
    // Check the block before the upper bound (the block that might contain key)
    --it;
    return find_in_block(it->offset, key);
}

std::vector<std::pair<std::string, std::string>> SSTable::scan(
    std::string_view start, std::string_view end) const 
{
    std::vector<std::pair<std::string, std::string>> out;
    if (start > end) return out;
    if (!meta_.max_key.empty() && start > meta_.max_key) return out;
    if (!meta_.min_key.empty() && end < meta_.min_key) return out;

    if (index_.empty()) {
        scan_block(0, start, end, out);
        return out;
    }

    auto it = std::upper_bound(index_.begin(), index_.end(), start,
        [](std::string_view k, const IndexEntry& ie) { return k < ie.min_key; });
    
    if (it != index_.begin()) --it;

    for (; it != index_.end(); ++it) {
        if (it->min_key > end) break;
        scan_block(it->offset, start, end, out);
    }
    return out;
}