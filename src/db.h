#pragma once

#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include "manifest.h"
#include "level.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ZenithDB {
public:
    explicit ZenithDB(const std::string& dir = "data");
    ~ZenithDB();

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    void remove(const std::string& key);

    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start = "",
        const std::string& end   = "\xFF\xFF") const;

private:
    // --------- On-disk layout snapshot (RCU) ---------
    struct Layout {
        struct FileEntry {
            std::shared_ptr<SSTable> sst;
            std::string min_key;
            std::string max_key;
        };

        // levels[level_index][file_index]
        std::vector<std::vector<FileEntry>> levels;
    };

    // Shared pointer used with std::atomic_load/store (via free functions).
    std::shared_ptr<Layout> layout_;

    // --------- Memtables ---------
    // Active mutable memtable
    std::shared_ptr<MemTable> active_mem_;

    // Immutable memtable chain; readers walk it lock-free.
    struct ImmNode {
        std::shared_ptr<MemTable> mt;
        ImmNode* next;
        bool flushed;  // set true by background thread once flushed to disk
    };

    std::atomic<ImmNode*> immut_head_{nullptr};

    // --------- Storage + background state ---------
    std::filesystem::path data_dir_;
    Manifest manifest_;
    std::unique_ptr<WAL> wal_;
    std::vector<Level> levels_meta_;

    std::thread worker_;
    std::atomic<bool> stop_{false};

    // Single writer-side mutex:
    // - put/remove
    // - flushing memtables
    // - compaction
    // - updating manifest_ and levels_meta_
    mutable std::mutex writer_mutex_;

    static const std::size_t MEMTABLE_LIMIT = 50 * 1024;  // ~50KB

    // --------- Internal helpers ---------
    void background_worker();
    void compact_level(int level, Layout& layout);
    std::string new_filename(int level);

    // Keep each level's FileEntries sorted by min_key for faster search
    static void sort_level_by_min_key(Layout& layout, std::size_t level);
    static void sort_all_levels_by_min_key(Layout& layout);
};