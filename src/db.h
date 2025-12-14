// src/db.h
#pragma once

#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include "manifest.h"
#include "level.h"
#include "env.h"
#include "crdt.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class ZenithDB {
public:
    ZenithDB(std::unique_ptr<Env> env, const std::string& dir, const std::string& node_id, bool sync_writes = false);
    explicit ZenithDB(const std::string& dir = "data", const std::string& node_id = "node1", bool sync_writes = false);
    
    ~ZenithDB();

    void put(const std::string& key, const std::string& value);
    void put(const std::string& key, const LWWRegister& crdt);
    std::optional<std::string> get(std::string_view key) const;
    void remove(const std::string& key);

    std::vector<std::pair<std::string, std::string>> scan(
        std::string_view start = "",
        std::string_view end   = "\xFF\xFF") const;

private:
    struct Layout {
        struct FileEntry {
            std::shared_ptr<SSTable> sst;
            std::string min_key;
            std::string max_key;
        };
        std::vector<std::vector<FileEntry>> levels;
    };

    struct CompactionTask {
        int level;
        std::vector<std::string> input_filenames;
        std::vector<std::shared_ptr<SSTable>> input_ssts;
        std::string output_filename;
        std::shared_ptr<SSTable> output_sst;
    };

    std::shared_ptr<Layout> layout_;
    std::shared_ptr<MemTable> active_mem_;

    struct ImmNode {
        std::shared_ptr<MemTable> mt;
        ImmNode* next;
        bool flushed; 
    };

    std::atomic<ImmNode*> immut_head_{nullptr};

    std::filesystem::path data_dir_;
    
    // Initialized first
    std::unique_ptr<Env> env_;
    
    // Changed to pointer to control initialization order
    std::unique_ptr<Manifest> manifest_;
    
    std::unique_ptr<WAL> wal_;
    std::vector<Level> levels_meta_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    mutable std::mutex writer_mutex_;
    bool sync_writes_;

    std::string node_id_;
    VectorClock local_clock_; 

    static const std::size_t MEMTABLE_LIMIT = 50 * 1024;

    void background_worker();
    std::optional<CompactionTask> plan_compaction(const Layout& layout);
    void execute_compaction(CompactionTask& task);
    void apply_compaction(const CompactionTask& task, Layout& new_layout);
    std::string new_filename(int level);
    static void sort_level_by_min_key(Layout& layout, std::size_t level);
    static void sort_all_levels_by_min_key(Layout& layout);
};