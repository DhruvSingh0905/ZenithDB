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

/**
 * ZenithDB - A high-performance LSM-tree based key-value database.
 * 
 * This class implements a Log-Structured Merge Tree (LSM-tree) storage engine
 * similar to LevelDB/RocksDB. It provides ACID-like guarantees with:
 * - Write-ahead logging (WAL) for durability
 * - Memtable-based in-memory writes for low latency
 * - Multi-level SSTable storage on disk
 * - Background compaction for space efficiency
 * - RCU (Read-Copy-Update) for lock-free reads
 * - Bloom filters for fast negative lookups
 */
class ZenithDB {
public:
    /**
     * Constructs a new ZenithDB instance.
     * 
     * Initializes the database by:
     * - Creating the data directory if it doesn't exist
     * - Replaying the WAL to recover the active memtable
     * - Loading the manifest to reconstruct the SSTable layout
     * - Opening all existing SSTables and building the RCU layout snapshot
     * - Starting the background worker thread for flushing and compaction
     * 
     * @param dir The directory path where database files will be stored (default: "data")
     */
    explicit ZenithDB(const std::string& dir = "data");
    
    /**
     * Destructor that gracefully shuts down the database.
     * 
     * Stops the background worker thread, syncs the WAL to disk,
     * and cleans up resources. Note: ImmNode objects are intentionally
     * leaked for simplicity (production would use epoch-based reclamation).
     */
    ~ZenithDB();

    /**
     * Inserts or updates a key-value pair in the database.
     * 
     * The write operation:
     * - Appends to the active memtable (in-memory)
     * - Writes to the WAL for durability
     * - Automatically freezes the memtable when it exceeds 2x MEMTABLE_LIMIT
     * - Frozen memtables are added to the immutable chain for background flushing
     * 
     * @param key The key to insert/update (must not be empty)
     * @param value The value to associate with the key
     */
    void put(const std::string& key, const std::string& value);
    
    /**
     * Retrieves the value associated with a key.
     * 
     * Performs a point lookup by searching in order:
     * 1. Active memtable (with range pruning)
     * 2. Immutable memtable chain (lock-free, with range pruning)
     * 3. On-disk SSTables via RCU snapshot (with bloom filter and range pruning)
     * 
     * Returns nullopt if the key is not found or has been deleted (tombstone).
     * This operation is lock-free for readers using RCU semantics.
     * 
     * @param key The key to look up
     * @return Optional containing the value if found, nullopt otherwise
     */
    std::optional<std::string> get(const std::string& key) const;
    
    /**
     * Deletes a key from the database.
     * 
     * Deletion is implemented as a tombstone (empty value) in the memtable.
     * The tombstone will be propagated through compaction and eventually
     * remove the key from all SSTables. Until compaction completes, the
     * tombstone prevents the key from being visible.
     * 
     * @param key The key to delete
     */
    void remove(const std::string& key);

    /**
     * Performs a range scan over keys in the database.
     * 
     * Returns all key-value pairs where start <= key <= end (inclusive).
     * The scan:
     * - Searches active and immutable memtables
     * - Searches all relevant SSTables using range pruning
     * - Deduplicates results (latest value wins)
     * - Filters out tombstones
     * 
     * @param start The inclusive start key (default: empty string = beginning)
     * @param end The inclusive end key (default: "\xFF\xFF" = end)
     * @return Vector of (key, value) pairs in sorted order
     */
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
    
    /**
     * Background worker thread that handles flushing and compaction.
     * 
     * This thread runs continuously until stop_ is set, performing:
     * 1. Flushing immutable memtables to Level 0 SSTables
     * 2. Compacting levels when they exceed their thresholds
     * 3. Updating the manifest with file changes
     * 4. Publishing new RCU layout snapshots for readers
     * 
     * Runs every 100ms to balance responsiveness and CPU usage.
     */
    void background_worker();
    
    /**
     * Compacts all SSTables in a given level into the next level.
     * 
     * Compaction merges multiple SSTables, removes duplicates (keeping latest),
     * drops tombstones for deleted keys, and writes a new merged SSTable
     * to the next level. This reduces read amplification and reclaims space.
     * 
     * @param level The level to compact (must be < levels_meta_.size() - 1)
     * @param layout The layout snapshot to update with new SSTable entries
     */
    void compact_level(int level, Layout& layout);
    
    /**
     * Generates a unique filename for a new SSTable.
     * 
     * Format: "L{level}_{timestamp}_{counter}.sst"
     * The counter ensures uniqueness even if multiple files are created
     * in the same second.
     * 
     * @param level The level number for the SSTable
     * @return A unique filename string
     */
    std::string new_filename(int level);

    /**
     * Sorts the FileEntries in a specific level by min_key.
     * 
     * This enables binary search and range pruning during reads.
     * Should be called after adding new SSTables to a level.
     * 
     * @param layout The layout to modify
     * @param level The level index to sort
     */
    static void sort_level_by_min_key(Layout& layout, std::size_t level);
    
    /**
     * Sorts all levels in the layout by min_key.
     * 
     * Convenience function to ensure all levels are properly ordered
     * after bulk operations like compaction or recovery.
     * 
     * @param layout The layout to sort
     */
    static void sort_all_levels_by_min_key(Layout& layout);
};