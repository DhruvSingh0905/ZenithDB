// src/sstable.cpp
#include "sstable.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifndef htole64
#define htole64(x) (x)
#define le64toh(x) (x)
#endif
#ifndef htole32
#define htole32(x) (x)
#define le32toh(x) (x)
#endif

static constexpr std::size_t BLOCK_TARGET_BYTES = 4096;
static constexpr std::size_t RESTART_INTERVAL = 16; 

static inline std::uint64_t bloom_hash_core(std::string_view k) {
    std::uint64_t h = 1469598103934665603ULL; 
    for (unsigned char c : k) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t SSTable::bloom_hash(std::string_view k, int i) {
    std::uint64_t base = bloom_hash_core(k);
    return base + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
}

bool SSTable::may_contain(std::string_view key) const {
    if (bloom_filter_.empty()) return true;
    
    std::size_t bloom_bits = bloom_filter_.size() * 8;
    for (int i = 0; i < BLOOM_HASHES; ++i) {
        std::uint64_t bit = bloom_hash(key, i) % bloom_bits;
        if ((bloom_filter_[bit >> 3] & (1u << (bit & 7))) == 0) return false;
    }
    return true;
}

SSTable::SSTable(Env* env, const std::filesystem::path& p) : env_(env) {
    file_ = env_->NewRandomAccessFile(p);
    meta_.file_size = env_->GetFileSize(p);
    
    if (meta_.file_size < FOOTER_SIZE) {
        throw std::runtime_error("sstable too small");
    }
    parse_footer_and_index();
}

SSTable::~SSTable() = default;

std::string SSTable::read_bytes(std::uint64_t offset, std::size_t n) const {
    char scratch[4096];
    std::string_view result;
    
    // If n is small, use stack scratch, otherwise alloc heap
    std::unique_ptr<char[]> heap_buf;
    char* buf_ptr = scratch;
    if (n > sizeof(scratch)) {
        heap_buf = std::make_unique<char[]>(n);
        buf_ptr = heap_buf.get();
    }

    file_->Read(offset, n, &result, buf_ptr);
    return std::string(result);
}

void SSTable::parse_footer_and_index() {
    // Read footer
    std::string footer_data = read_bytes(meta_.file_size - FOOTER_SIZE, FOOTER_SIZE);
    const char* base = footer_data.data();
    
    // Decode footer
    std::uint64_t f[4];
    std::memcpy(f, base, FOOTER_SIZE);
    
    data_end_     = le64toh(f[0]);
    bloom_offset_ = le64toh(f[1]);
    index_offset_ = le64toh(f[2]);
    std::uint64_t magic = le64toh(f[3]);

    if (magic != MAGIC) throw std::runtime_error("bad magic");

    index_.clear();
    meta_.entry_count = 0;

    // Load Bloom Filter into memory
    if (index_offset_ > bloom_offset_) {
        std::size_t len = index_offset_ - bloom_offset_;
        std::string bloom_raw = read_bytes(bloom_offset_, len);
        bloom_filter_.resize(len);
        std::memcpy(bloom_filter_.data(), bloom_raw.data(), len);
    }

    if (index_offset_ == meta_.file_size - FOOTER_SIZE) {
        // No index
        auto all = scan("", "\xFF\xFF");
        if (!all.empty()) {
            meta_.min_key = all.front().first;
            meta_.max_key = all.back().first;
            meta_.entry_count = all.size();
        }
        return;
    }

    // Load Index
    std::size_t idx_len = meta_.file_size - FOOTER_SIZE - index_offset_;
    std::string index_data = read_bytes(index_offset_, idx_len);
    const char* p = index_data.data();
    
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

void SSTable::create(Env* env, const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::string>>& entries)
{
    auto out = env->NewWritableFile(path);
    std::uint64_t current_offset = 0;

    std::string data_buf;
    std::vector<IndexEntry> index_entries;
    
    std::size_t i = 0;
    while (i < entries.size()) {
        std::uint64_t block_start_pos = current_offset;
        
        data_buf.clear();
        std::vector<std::uint32_t> restarts;
        restarts.push_back(8); 

        std::uint32_t num_entries = 0;
        std::string min_key = entries[i].first;

        data_buf.resize(8); 

        while (i < entries.size()) {
            const auto& kv = entries[i];
            
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

        out->Append(data_buf);
        current_offset += data_buf.size();

        index_entries.push_back({min_key, block_start_pos});
    }

    std::uint64_t data_end = current_offset;

    // Bloom Filter
    std::size_t total_keys = entries.size();
    
    // BUG FIX: Ensure bloom_bits is a multiple of 8 to match the reader's logic.
    // The reader derives bits from file size (bytes * 8).
    std::size_t raw_bits = std::max<std::size_t>(total_keys * BLOOM_BITS_PER_KEY, 8);
    std::size_t bloom_bytes = (raw_bits + 7) / 8;
    std::size_t bloom_bits = bloom_bytes * 8; // This must be used for modulo
    
    std::vector<unsigned char> bloom(bloom_bytes, 0);
    
    for (const auto& kv : entries) {
        for (int h = 0; h < BLOOM_HASHES; ++h) {
            std::uint64_t bit = SSTable::bloom_hash(kv.first, h) % bloom_bits; 
            bloom[bit/8] |= (1 << (bit%8));
        }
    }

    std::string bloom_str(reinterpret_cast<char*>(bloom.data()), bloom.size());
    std::uint64_t bloom_offset = current_offset;
    out->Append(bloom_str);
    current_offset += bloom_str.size();

    // Sparse Index
    std::uint64_t index_offset = current_offset;
    std::string index_buf;
    std::uint32_t count = htole32(static_cast<std::uint32_t>(index_entries.size()));
    index_buf.append((char*)&count, 4);
    
    for (const auto& ie : index_entries) {
        std::uint32_t klen = htole32(static_cast<std::uint32_t>(ie.min_key.size()));
        index_buf.append((char*)&klen, 4);
        index_buf.append(ie.min_key);
        std::uint64_t off = htole64(ie.offset);
        index_buf.append((char*)&off, 8);
    }
    out->Append(index_buf);
    current_offset += index_buf.size();

    // Footer
    std::uint64_t footer[4];
    footer[0] = htole64(data_end);
    footer[1] = htole64(bloom_offset);
    footer[2] = htole64(index_offset);
    footer[3] = htole64(MAGIC);
    
    std::string footer_str((char*)footer, FOOTER_SIZE);
    out->Append(footer_str);
    
    out->Sync();
    out->Close();
}

std::optional<std::string> SSTable::find_in_block(
    std::uint64_t offset,
    std::string_view key) const
{
    if (offset + 8 > data_end_) return std::nullopt;
    
    // Read header to get block size
    std::string header = read_bytes(offset, 8);
    std::uint32_t num_entries;
    std::uint32_t block_size;
    std::memcpy(&num_entries, header.data(), 4); num_entries = le32toh(num_entries);
    std::memcpy(&block_size, header.data()+4, 4); block_size = le32toh(block_size);

    if (offset + block_size > data_end_) return std::nullopt;

    // Read full block
    std::string block_data = read_bytes(offset, block_size);
    const char* bp = block_data.data();
    const char* block_end = bp + block_size;

    std::uint32_t num_restarts;
    std::memcpy(&num_restarts, block_end - 4, 4);
    num_restarts = le32toh(num_restarts);

    const char* restarts_ptr = block_end - 4 - (num_restarts * 4);
    
    std::uint32_t left = 0, right = num_restarts;
    while (left < right) {
        std::uint32_t mid = (left + right) / 2;
        std::uint32_t restart_off;
        std::memcpy(&restart_off, restarts_ptr + mid*4, 4);
        restart_off = le32toh(restart_off);

        const char* entry = bp + restart_off;
        std::uint32_t klen;
        std::memcpy(&klen, entry, 4); klen = le32toh(klen);
        std::string_view mid_key(entry + 4, klen); 
        
        if (mid_key < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    std::uint32_t start_idx = (left > 0) ? left - 1 : 0;
    std::uint32_t restart_off;
    std::memcpy(&restart_off, restarts_ptr + start_idx*4, 4);
    restart_off = le32toh(restart_off);

    const char* ptr = bp + restart_off;
    const char* limit = restarts_ptr; 

    while (ptr < limit) {
        std::uint32_t klen;
        std::memcpy(&klen, ptr, 4); klen = le32toh(klen);
        ptr += 4;
        std::string_view entry_key(ptr, klen); 
        ptr += klen;

        std::uint32_t vlen;
        std::memcpy(&vlen, ptr, 4); vlen = le32toh(vlen);
        ptr += 4;
        std::string_view entry_val(ptr, vlen); 
        ptr += vlen;

        int cmp = entry_key.compare(key);
        if (cmp == 0) return std::string(entry_val); 
        if (cmp > 0) return std::nullopt; 
    }
    return std::nullopt; 
}

void SSTable::scan_block(
    std::uint64_t offset,
    std::string_view start,
    std::string_view end,
    std::vector<std::pair<std::string, std::string>>& out) const
{
    if (offset + 8 > data_end_) return;
    
    // Read header
    std::string header = read_bytes(offset, 8);
    std::uint32_t block_size;
    std::memcpy(&block_size, header.data()+4, 4); block_size = le32toh(block_size);
    
    // Read block
    std::string block_data = read_bytes(offset, block_size);
    const char* bp = block_data.data();
    
    const char* block_end = bp + block_size;
    std::uint32_t num_restarts;
    std::memcpy(&num_restarts, block_end - 4, 4); num_restarts = le32toh(num_restarts);
    const char* restarts_ptr = block_end - 4 - (num_restarts * 4);

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
    if (!may_contain(key)) return std::nullopt;
    
    if (index_.empty()) return find_in_block(0, key);

    auto it = std::upper_bound(index_.begin(), index_.end(), key,
        [](std::string_view k, const IndexEntry& ie) {
            return k < ie.min_key;
        });
    
    if (it == index_.begin()) return find_in_block(index_.front().offset, key);
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