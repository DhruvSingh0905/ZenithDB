#include "sstable.h"
#include "block_cache.h"
// src/sstable.cpp — top of file, add this helper
#include <cstdint>

static inline uint64_t bswap64(uint64_t x) {
    return ((x << 56) & 0xff00000000000000ULL) |
           ((x << 40) & 0x00ff000000000000ULL) |
           ((x << 24) & 0x0000ff0000000000ULL) |
           ((x <<  8) & 0x000000ff00000000ULL) |
           ((x >>  8) & 0x00000000ff000000ULL) |
           ((x >> 24) & 0x0000000000ff0000ULL) |
           ((x >> 40) & 0x000000000000ff00ULL) |
           ((x >> 56) & 0x00000000000000ffULL);
}

#define htole64(x) bswap64(x)
#define le64toh(x) bswap64(x)
#include <fstream>
#include <algorithm>
#include <cstring>
#include <stdexcept>

uint64_t SSTable::bloom_hash(const std::string& k, int i) const {
    uint64_t h = std::hash<std::string>{}(k);
    return h + i * 0x517cc1b727220a95ULL;
}

bool SSTable::may_contain(const std::string& key) const {
    // Empty or no bloom region → treat as "maybe"
    if (data_end_ == 0 || bloom_offset_ >= index_offset_) return true;

    size_t bloom_bytes = index_offset_ - bloom_offset_;
    size_t bits = bloom_bytes * 8;
    if (bits == 0) return true;

    for (int i = 0; i < BLOOM_HASHES; ++i) {
        uint64_t bit = bloom_hash(key, i) % bits;
        size_t byte_index = bloom_offset_ + bit / 8;
        uint8_t mask = uint8_t(1u << (bit % 8));
        if ((static_cast<unsigned char>(data_[byte_index]) & mask) == 0) {
            return false;  // definitely not present
        }
    }
    return true;  // possibly present
}

SSTable::SSTable(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open sstable");

    in.seekg(0, std::ios::end);
    size_t sz = static_cast<size_t>(in.tellg());
    in.seekg(0);
    data_.resize(sz);
    in.read(data_.data(), sz);

    if (sz < 48) throw std::runtime_error("corrupt sstable");
    const uint64_t* f = reinterpret_cast<const uint64_t*>(data_.data() + sz - 48);
    data_end_     = le64toh(f[0]);
    bloom_offset_ = le64toh(f[1]);
    index_offset_ = le64toh(f[2]);
    meta_.file_size = sz;

    // Extract min_key (first key in data region)
    const char* ptr = data_.data();
    const char* end = ptr + data_end_;
    if (ptr < end) {
        const char* sep = std::find(ptr, end, '\0');
        if (sep != end) {
            meta_.min_key.assign(ptr, sep);
        }
    }

    // Extract max_key (last key before data_end_)
    const char* last = nullptr;
    ptr = data_.data();

    while (ptr < end) {
        const char* sep = std::find(ptr, end, '\0');
        if (sep == end) break;
        last = ptr;              // start of key
        ptr = sep + 1;           // value
        sep = std::find(ptr, end, '\0');
        if (sep == end) break;
        ptr = sep + 1;           // next key
        meta_.entry_count++;
    }

    if (last && last < end) {
        const char* sep = std::find(last, end, '\0');
        if (sep != end) {
            meta_.max_key.assign(last, sep);
        }
    }
}

void SSTable::create(const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::string>>& entries)
{
    if (entries.empty()) return;

    // Make a sorted copy by key to guarantee on-disk order.
    std::vector<std::pair<std::string, std::string>> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });

    std::string data;

    // Bloom filter preparation
    size_t bloom_bits = sorted.size() * BLOOM_BITS_PER_KEY;
    if (bloom_bits == 0) bloom_bits = 8;  // at least one byte
    size_t bloom_bytes = (bloom_bits + 7) / 8;
    std::string bloom(bloom_bytes, 0);

    auto set_bloom = [&](const std::string& k) {
        for (int i = 0; i < BLOOM_HASHES; ++i) {
            uint64_t bit = (std::hash<std::string>{}(k)
                            + i * 0x517cc1b727220a95ULL) % bloom_bits;
            bloom[bit >> 3] |= char(1u << (bit & 7));
        }
    };

    // Write KV data, fill bloom
    for (const auto& [k, v] : sorted) {
        set_bloom(k);
        data.append(k);
        data.push_back('\0');
        data.append(v);
        data.push_back('\0');
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create sstable");

    // Write data
    out.write(data.data(), data.size());
    uint64_t bloom_off = data.size();

    // Write bloom
    out.write(bloom.data(), bloom.size());
    uint64_t index_off = bloom_off + bloom.size();

    // Footer: 6 * 8 = 48 bytes
    uint64_t footer[6] = {
        htole64(static_cast<uint64_t>(data.size())),  // data_end
        htole64(bloom_off),
        htole64(index_off),
        htole64(0xDB55CA1E),  // magic
        0,
        0
    };
    out.write(reinterpret_cast<char*>(footer), sizeof footer);
}
std::optional<std::string> SSTable::get(const std::string& key) const {
    // Bloom + range pre-checks are done in ZenithDB::get, but we keep may_contain usable here too.
    if (!may_contain(key)) return std::nullopt;
    if (!meta_.min_key.empty() && key < meta_.min_key) return std::nullopt;
    if (!meta_.max_key.empty() && key > meta_.max_key) return std::nullopt;

    const char* p = data_.data();
    const char* end = p + data_end_;
    while (p < end) {
        const char* sep = std::find(p, end, '\0');
        if (sep == end) break;
        std::string k(p, sep);
        p = sep + 1;
        sep = std::find(p, end, '\0');
        if (sep == end) break;
        std::string v(p, sep);
        p = sep + 1;

        if (k == key) {
            if (!v.empty()) return v;
            return std::nullopt;
        }
        if (k > key) break;
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> SSTable::scan(
    const std::string& start, const std::string& end) const
{
    std::vector<std::pair<std::string, std::string>> res;

    const char* p = data_.data();
    const char* endp = p + data_end_;
    while (p < endp) {
        const char* sep = std::find(p, endp, '\0');
        if (sep == endp) break;
        std::string k(p, sep);
        p = sep + 1;
        sep = std::find(p, endp, '\0');
        if (sep == endp) break;
        std::string v(p, sep);
        p = sep + 1;

        if (!v.empty() && k >= start && k <= end) {
            res.emplace_back(std::move(k), std::move(v));
        }
        if (k > end) break;
    }
    return res;
}