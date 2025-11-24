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
#include <string_view>
#include <thread>
#include <vector>

/**
 * ZenithDB - High-performance embedded key-value database engine.
 * 
 * Implements an LSM-tree (Log-Structured Merge Tree) architecture optimized
 * for write-heavy workloads with excellent read performance through:
 * - Lock-free reads using RCU (Read-Copy-Update) semantics
 * - Memory-mapped SSTable files for zero-copy reads
 * - Bloom filters for fast negative lookups
 * - Sparse block indexing with restart points for efficient binary search
 * - Range pruning to skip irrelevant data structures
 * 
 * Thread Safety:
 * - Reads: Fully concurrent and lock-free (RCU)
 * - Writes: Serialized via writer_mutex_ (single writer)
 * - Background operations: Run in separate thread with writer_mutex_
 */
class ZenithDB {
public:
    /**
     * Constructs a ZenithDB instance.
     * 
     * Opens or creates a database in the specified directory. On startup:
     * - Replays WAL to reconstruct active memtable
     * - Loads manifest to reconstruct level structure
     * - Opens all existing SSTable files and builds layout snapshot
     * - Starts background worker thread for flushing and compaction
     * 
     * @param dir Directory path for database files (default: "data")
     */
    explicit ZenithDB(const std::string& dir = "data");
    
    /**
     * Destructor.
     * 
     * Stops background worker, syncs WAL to disk, and cleans up resources.
     */
    ~ZenithDB();

    /**
     * Inserts or updates a key-value pair.
     * 
     * Write path:
     * 1. Acquires writer_mutex_ (serializes writes)
     * 2. Writes to active memtable (O(log n) insertion)
     * 3. Appends to WAL for durability
     * 4. If memtable exceeds threshold, freezes it and adds to immutable chain
     * 
     * @param key The key to insert/update
     * @param value The value to associate with the key
     */
    void put(const std::string& key, const std::string& value);
    
    /**
     * Retrieves a value by key.
     * 
     * Read path (lock-free, uses RCU):
     * 1. Checks active memtable (with range pruning)
     * 2. Checks immutable memtable chain (lock-free traversal)
     * 3. Checks SSTables from Level 0 to deeper levels:
     *    - Range pruning to skip irrelevant files
     *    - Bloom filter to quickly reject files
     *    - Sparse index binary search to find block
     *    - Block scan with restart points for efficient search
     * 
     * Returns nullopt if key not found or deleted (tombstone).
     * 
     * @param key The key to look up (string_view avoids allocation)
     * @return Optional containing the value, or nullopt if not found
     */
    std::optional<std::string> get(std::string_view key) const;
    
    /**
     * Deletes a key by inserting a tombstone (empty value).
     * 
     * The tombstone will be removed during compaction when it reaches
     * a level where no older versions exist.
     * 
     * @param key The key to delete
     */
    void remove(const std::string& key);

    /**
     * Performs a range scan (inclusive on both ends).
     * 
     * Scans all memtables and SSTables, merges results, removes duplicates
     * (keeping latest), and filters out tombstones.
     * 
     * @param start Start key (inclusive), default: "" (beginning)
     * @param end End key (inclusive), default: "\xFF\xFF" (end)
     * @return Vector of key-value pairs in sorted order
     */
    std::vector<std::pair<std::string, std::string>> scan(
        std::string_view start = "",
        std::string_view end   = "\xFF\xFF") const;

private:
    /**
     * Layout - RCU snapshot of the on-disk SSTable structure.
     * 
     * This structure is atomically swapped to enable lock-free reads.
     * Readers take a snapshot via atomic_load_ptr, use it, then release.
     * Writers create a new Layout, modify it, then publish via atomic_store_ptr.
     * Old snapshots remain valid until all readers release them.
     */
    struct Layout {
        struct FileEntry {
            std::shared_ptr<SSTable> sst;      // Memory-mapped SSTable file
            std::string min_key;                // Minimum key in this file (for range pruning)
            std::string max_key;                // Maximum key in this file (for range pruning)
        };
        std::vector<std::vector<FileEntry>> levels;  // One vector per level
    };

    /**
     * CompactionTask - Represents a compaction job.
     * 
     * Contains all information needed to compact a level:
     * - Which level to compact
     * - Which files to merge (filenames and SSTable objects)
     * - Where to write the output
     */
    struct CompactionTask {
        int level;                                              // Level being compacted
        std::vector<std::string> input_filenames;              // Filenames being merged
        std::vector<std::shared_ptr<SSTable>> input_ssts;      // SSTable objects (for reading)
        std::string output_filename;                            // Output filename
        std::shared_ptr<SSTable> output_sst;                    // Output SSTable (after creation)
    };

    // RCU layout snapshot - atomically swapped for lock-free reads
    std::shared_ptr<Layout> layout_;
    
    // Active memtable - current write target (atomically swapped)
    std::shared_ptr<MemTable> active_mem_;

    /**
     * ImmNode - Node in lock-free linked list of immutable memtables.
     * 
     * When a memtable is frozen, it's added to this chain. The background
     * worker flushes them to disk. Once flushed, they remain in the chain
     * until garbage collected (currently not reclaimed - known deficit).
     */
    struct ImmNode {
        std::shared_ptr<MemTable> mt;  // The immutable memtable
        ImmNode* next;                  // Next node in chain
        bool flushed;                   // Whether this has been flushed to disk
    };

    // Lock-free head of immutable memtable chain (RCU-style)
    std::atomic<ImmNode*> immut_head_{nullptr};

    std::filesystem::path data_dir_;    // Database directory
    Manifest manifest_;                  // Tracks SSTable files per level
    std::unique_ptr<WAL> wal_;          // Write-ahead log for durability
    std::vector<Level> levels_meta_;    // Metadata about each level (file lists)

    std::thread worker_;                 // Background worker thread
    std::atomic<bool> stop_{false};     // Shutdown flag
    mutable std::mutex writer_mutex_;   // Serializes writes and background operations

    // Memtable size threshold (50KB) - when exceeded, memtable is frozen
    static const std::size_t MEMTABLE_LIMIT = 50 * 1024;

    /**
     * Background worker thread main loop.
     * 
     * Continuously:
     * 1. Flushes immutable memtables to Level 0 SSTables
     * 2. Plans and executes compactions when thresholds are met
     * 3. Updates layout snapshots atomically
     * 
     * Runs every 100ms or when work is available.
     */
    void background_worker();
    
    /**
     * Plans a compaction task if thresholds are met.
     * 
     * Checks each level for compaction triggers:
     * - Level 0: ≥ 3 files
     * - Level 1+: ≥ 4 files
     * 
     * Returns a CompactionTask if compaction is needed, nullopt otherwise.
     * 
     * @param layout Current layout snapshot
     * @return CompactionTask if compaction needed, nullopt otherwise
     */
    std::optional<CompactionTask> plan_compaction(const Layout& layout);
    
    /**
     * Executes a compaction task (I/O heavy, runs without lock).
     * 
     * Merges all input SSTables, removes duplicates (keeps latest),
     * drops tombstones, and writes merged result to new SSTable.
     * 
     * @param task The compaction task to execute
     */
    void execute_compaction(CompactionTask& task);
    
    /**
     * Applies compaction results to layout and metadata (runs with lock).
     * 
     * Updates manifest, levels_meta_, and creates new layout snapshot
     * with old files removed and new file added.
     * 
     * @param task The completed compaction task
     * @param new_layout The new layout to update
     */
    void apply_compaction(const CompactionTask& task, Layout& new_layout);
    
    /**
     * Generates a unique filename for a new SSTable.
     * 
     * Format: "L{level}_{timestamp}_{counter}.sst"
     * 
     * @param level The level number
     * @return Unique filename
     */
    std::string new_filename(int level);

    /**
     * Sorts a level's FileEntry vector by min_key for efficient binary search.
     * 
     * @param layout The layout to modify
     * @param level The level to sort
     */
    static void sort_level_by_min_key(Layout& layout, std::size_t level);
    
    /**
     * Sorts all levels by min_key.
     * 
     * @param layout The layout to modify
     */
    static void sort_all_levels_by_min_key(Layout& layout);
};