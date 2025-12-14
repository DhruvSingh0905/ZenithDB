#pragma once

#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include "manifest.h"
#include "level.h"
#include "env.h"

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
    /**
     * Dependency Injection Constructor.
     * * Allows passing a custom Environment (e.g., MockEnv for testing, S3Env for cloud).
     * Takes ownership of the Env pointer.
     * * @param env Unique pointer to the Environment implementation
     * @param dir Directory path for database files
     * @param sync_writes If true, syncs WAL to disk on every write
     */
    ZenithDB(std::unique_ptr<Env> env, const std::string& dir, bool sync_writes = false);

    /**
     * Standard Constructor.
     * * Creates a standard PosixEnv (local disk) and delegates to the main constructor.
     * * @param dir Directory path for database files (default: "data")
     * @param sync_writes If true, syncs WAL to disk on every write (default: false)
     */
    explicit ZenithDB(const std::string& dir = "data", bool sync_writes = false);
    
    ~ZenithDB();

    void put(const std::string& key, const std::string& value);
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

    // CRITICAL: env_ must be declared before manifest_ and wal_ so it is initialized first.
    std::unique_ptr<Env> env_;

    Manifest manifest_;
    std::unique_ptr<WAL> wal_;
    std::vector<Level> levels_meta_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    mutable std::mutex writer_mutex_;
    bool sync_writes_;

    static const std::size_t MEMTABLE_LIMIT = 50 * 1024;

    void background_worker();
    
    std::optional<CompactionTask> plan_compaction(const Layout& layout);
    void execute_compaction(CompactionTask& task);
    void apply_compaction(const CompactionTask& task, Layout& new_layout);
    
    std::string new_filename(int level);

    static void sort_level_by_min_key(Layout& layout, std::size_t level);
    static void sort_all_levels_by_min_key(Layout& layout);
};