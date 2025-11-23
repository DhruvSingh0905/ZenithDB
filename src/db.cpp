#include "db.h"
#include <iostream>
#include <algorithm>
#include <chrono>

ZenithDB::ZenithDB(const std::string& dir)
    : data_dir_(dir), manifest_(dir)
{
    std::filesystem::create_directories(dir);
    wal_ = std::make_unique<WAL>(dir);
    wal_->replay(&current_memtable_);

    auto loaded = manifest_.load();
    levels_ = std::move(loaded);
    if (levels_.empty()) levels_.resize(7);

    worker_ = std::thread(&ZenithDB::background_worker, this);
}

ZenithDB::~ZenithDB() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    if (worker_.joinable()) worker_.join();
    wal_->sync();
}

void ZenithDB::put(const std::string& k, const std::string& v) {
    current_memtable_.put(k, v);
    wal_->append("PUT|" + k + "|" + v);

    if (current_memtable_.approximate_size() > 2 * MEMTABLE_LIMIT) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (current_memtable_.approximate_size() > 2 * MEMTABLE_LIMIT) {
            immutable_memtables_.push_back(
                std::make_unique<MemTable>(std::move(current_memtable_)));
            current_memtable_ = MemTable{};
        }
    }
}

void ZenithDB::remove(const std::string& k) {
    current_memtable_.remove(k);
    wal_->append("DEL|" + k + "|");

    if (current_memtable_.approximate_size() > 2 * MEMTABLE_LIMIT) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (current_memtable_.approximate_size() > 2 * MEMTABLE_LIMIT) {
            immutable_memtables_.push_back(
                std::make_unique<MemTable>(std::move(current_memtable_)));
            current_memtable_ = MemTable{};
        }
    }
}

std::optional<std::string> ZenithDB::get(const std::string& key) const {
    // 1. Current + immutable memtables (fast path)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (auto v = current_memtable_.get(key); v && !v->empty()) {
            return v;
        }
        for (const auto& imm : immutable_memtables_) {
            if (auto v = imm->get(key); v && !v->empty()) {
                return v;
            }
        }
    }

    // 2. SSTables — with bloom + min/max pruning
    for (const auto& level : levels_) {
        for (const auto& filename : level.files) {
            SSTable sst(data_dir_ / filename);

            // Bloom: definitely not present → skip
            if (!sst.may_contain(key)) continue;

            const auto& meta = sst.meta();
            if (!meta.min_key.empty() && key < meta.min_key) continue;
            if (!meta.max_key.empty() && key > meta.max_key) continue;

            if (auto v = sst.get(key); v && !v->empty()) {
                return v;
            }
        }
    }

    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> ZenithDB::scan(
    const std::string& start, const std::string& end) const
{
    std::vector<std::pair<std::string, std::string>> result;

    // 1. Memtables
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto add_from_memtable = [&](const MemTable& mt) {
            auto part = mt.scan(start, end);
            result.insert(result.end(), part.begin(), part.end());
        };

        add_from_memtable(current_memtable_);
        for (const auto& imm : immutable_memtables_) {
            add_from_memtable(*imm);
        }
    }

    // 2. SSTables (pruned by min/max)
    for (const auto& level : levels_) {
        for (const auto& filename : level.files) {
            SSTable sst(data_dir_ / filename);
            const auto& meta = sst.meta();

            if (!meta.max_key.empty() && start > meta.max_key) continue;
            if (!meta.min_key.empty() && end < meta.min_key) continue;

            auto part = sst.scan(start, end);
            result.insert(result.end(), part.begin(), part.end());
        }
    }

    // Dedup by key: sort by key, then erase consecutive same-key entries.
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    result.erase(
        std::unique(result.begin(), result.end(),
                    [](const auto& a, const auto& b) { return a.first == b.first; }),
        result.end()
    );

    return result;
}

void ZenithDB::background_worker() {
    while (true) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stop_) break;

            // Flush all immutable memtables → L0 SSTables
            while (!immutable_memtables_.empty()) {
                auto mem = std::move(immutable_memtables_.front());
                immutable_memtables_.erase(immutable_memtables_.begin());

                auto entries = mem->sorted_entries();
                if (entries.empty()) continue;

                std::string filename = new_filename(0);
                SSTable::create(data_dir_ / filename, entries);

                levels_[0].files.push_back(filename);
                manifest_.add_sstable(0, filename);
            }

            // Run compaction with consistent thresholds
            for (size_t i = 0; i < levels_.size() - 1; ++i) {
                size_t threshold = (i == 0) ? 3 : 4; // L0 needs ≥3 files, others ≥4
                if (levels_[i].files.size() >= threshold) {
                    compact_level(static_cast<int>(i));
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void ZenithDB::compact_level(int level) {
    // background_worker already holds mutex_

    size_t threshold = (level == 0) ? 3 : 4;
    if (levels_[level].files.size() < threshold) return;

    // Move files out of this level
    std::vector<std::string> input_files = std::move(levels_[level].files);
    levels_[level].files.clear();

    std::map<std::string, std::string> merged;

    // Read + merge all SSTables from this level
    for (const auto& file : input_files) {
        SSTable sst(data_dir_ / file);
        auto entries = sst.scan("", "\xFF\xFF\xFF\xFF");
        for (auto& [k, v] : entries) {
            if (v.empty()) {
                merged.erase(k);
            } else {
                merged[k] = v;
            }
        }
    }

    if (!merged.empty()) {
        std::string new_file = new_filename(level + 1);

        std::vector<std::pair<std::string, std::string>> vec;
        vec.reserve(merged.size());
        for (const auto& p : merged) {
            vec.emplace_back(p.first, p.second);
        }

        SSTable::create(data_dir_ / new_file, vec);
        levels_[level + 1].files.push_back(new_file);
        manifest_.add_sstable(level + 1, new_file);
    }

    // Logically drop old files from manifest
    manifest_.replace(level, input_files, {});

    // Optional: physical deletion (only safe if Manifest::load respects DEL)
    // for (const auto& file : input_files) {
    //     std::error_code ec;
    //     std::filesystem::remove(data_dir_ / file, ec);
    // }
}

std::string ZenithDB::new_filename(int level) {
    static uint64_t counter = 0;
    return "L" + std::to_string(level) + "_" +
           std::to_string(std::time(nullptr)) + "_" +
           std::to_string(counter++) + ".sst";
}