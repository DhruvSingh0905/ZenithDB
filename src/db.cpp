#include "db.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <thread>

using namespace std::chrono_literals;

/**
 * Atomic load of shared_ptr with acquire memory ordering.
 * 
 * Ensures all writes to the pointed-to object are visible after this load.
 * Used by readers to take RCU snapshots.
 */
template <typename T>
static std::shared_ptr<T> atomic_load_ptr(const std::shared_ptr<T>* p) {
    return std::atomic_load_explicit(p, std::memory_order_acquire);
}

/**
 * Atomic store of shared_ptr with release memory ordering.
 * 
 * Ensures all writes to the pointed-to object are visible before this store.
 * Used by writers to publish new RCU snapshots.
 */
template <typename T>
static void atomic_store_ptr(std::shared_ptr<T>* p, std::shared_ptr<T> v) {
    std::atomic_store_explicit(p, std::move(v), std::memory_order_release);
}

void ZenithDB::sort_level_by_min_key(Layout& layout, std::size_t level) {
    if (level >= layout.levels.size()) return;
    auto& vec = layout.levels[level];
    std::sort(vec.begin(), vec.end(),
              [](const Layout::FileEntry& a, const Layout::FileEntry& b) {
                  return a.min_key < b.min_key;
              });
}

void ZenithDB::sort_all_levels_by_min_key(Layout& layout) {
    for (std::size_t lvl = 0; lvl < layout.levels.size(); ++lvl) {
        sort_level_by_min_key(layout, lvl);
    }
}

ZenithDB::ZenithDB(const std::string& dir)
    : data_dir_(dir),
      manifest_(data_dir_) {

    // Create database directory if it doesn't exist
    std::filesystem::create_directories(data_dir_);
    
    // Initialize WAL for durability
    wal_ = std::make_unique<WAL>(data_dir_.string());

    // Replay WAL to reconstruct active memtable (recovery)
    auto mem = std::make_shared<MemTable>();
    wal_->replay(mem.get());
    atomic_store_ptr(&active_mem_, mem);

    // Load manifest to reconstruct level structure
    auto loaded = manifest_.load();
    std::size_t level_count = std::max<std::size_t>(7, loaded.size());
    levels_meta_ = std::move(loaded);
    levels_meta_.resize(level_count);

    // Build initial layout snapshot by opening all existing SSTables
    auto layout = std::make_shared<Layout>();
    layout->levels.resize(level_count);

    for (std::size_t lvl = 0; lvl < levels_meta_.size(); ++lvl) {
        for (const auto& fname : levels_meta_[lvl].files) {
            auto path = data_dir_ / fname;
            if (!std::filesystem::exists(path)) continue;
            try {
                // Memory-map the SSTable file (zero-copy)
                auto sst = std::make_shared<SSTable>(path);
                Layout::FileEntry fe;
                fe.sst     = sst;
                fe.min_key = sst->meta().min_key;  // For range pruning
                fe.max_key = sst->meta().max_key;  // For range pruning
                layout->levels[lvl].push_back(std::move(fe));
            } catch (const std::exception& e) {
                // Skip corrupted files (deficit: should log more details)
                std::cerr << "[ZenithDB] Failed to open SSTable " << path << ": " << e.what() << "\n";
            }
        }
    }
    
    // Sort levels by min_key for efficient binary search
    sort_all_levels_by_min_key(*layout);
    
    // Publish initial layout snapshot (RCU)
    atomic_store_ptr(&layout_, layout);
    
    // Start background worker thread for flushing and compaction
    worker_ = std::thread(&ZenithDB::background_worker, this);
}

ZenithDB::~ZenithDB() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    if (wal_) wal_->sync();
}

void ZenithDB::put(const std::string& key, const std::string& value) {
    // Serialize writes (deficit: single writer limits throughput)
    std::lock_guard<std::mutex> lk(writer_mutex_);
    
    // Load current active memtable (RCU snapshot)
    auto mem = atomic_load_ptr(&active_mem_);
    if (!mem) {
        // Create new memtable if none exists (shouldn't happen normally)
        mem = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, mem);
    }

    // Write to memtable (O(log n) insertion into std::map)
    mem->put(key, value);
    
    // Append to WAL for durability (sequential write, fast)
    wal_->append("PUT|" + key + "|" + value);

    // Freeze memtable if it exceeds threshold (2x limit to reduce flushes)
    if (mem->approximate_size() > 2 * MEMTABLE_LIMIT) {
        auto frozen = mem;
        auto fresh  = std::make_shared<MemTable>();
        
        // Atomically swap active memtable (readers see new one immediately)
        atomic_store_ptr(&active_mem_, fresh);

        // Add frozen memtable to immutable chain (lock-free, RCU-style)
        ImmNode* old_head = immut_head_.load(std::memory_order_acquire);
        auto* node        = new ImmNode{frozen, old_head, false};

        // Lock-free insertion at head using compare-and-swap
        while (!immut_head_.compare_exchange_weak(
                   old_head, node,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
            node->next = old_head;  // Update next pointer if CAS failed
        }
        // Background worker will flush this to disk
    }
}

void ZenithDB::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(writer_mutex_);
    auto mem = atomic_load_ptr(&active_mem_);
    if (!mem) {
        mem = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, mem);
    }
    mem->remove(key);
    wal_->append("DEL|" + key + "|");
    if (mem->approximate_size() > 2 * MEMTABLE_LIMIT) {
        auto frozen = mem;
        auto fresh  = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, fresh);
        ImmNode* old_head = immut_head_.load(std::memory_order_acquire);
        auto* node        = new ImmNode{frozen, old_head, false};
        while (!immut_head_.compare_exchange_weak(old_head, node, std::memory_order_release, std::memory_order_acquire)) {
            node->next = old_head;
        }
    }
}

std::optional<std::string> ZenithDB::get(std::string_view key) const {
    // LOCK-FREE READ PATH (RCU) - No mutexes acquired
    
    // Step 1: Check active memtable (most recent data)
    {
        auto mem = atomic_load_ptr(&active_mem_);  // RCU snapshot
        if (mem && mem->has_range()) {
            // Range pruning: skip if key outside memtable's key range
            if (key >= mem->min_key() && key <= mem->max_key()) {
                if (auto v = mem->get(key)) {
                    // Empty value = tombstone (deleted)
                    if (!v->empty()) return v;
                    return std::nullopt;
                }
            }
        }
        
        // Step 2: Check immutable memtable chain (lock-free traversal)
        // Traverse from head (most recent) to tail (oldest)
        for (auto* node = immut_head_.load(std::memory_order_acquire); node; node = node->next) {
            auto mt = node->mt;
            if (!mt || !mt->has_range()) continue;
            
            // Range pruning: skip if key outside this memtable's range
            if (key < mt->min_key() || key > mt->max_key()) continue;
            
            if (auto v = mt->get(key)) {
                if (!v->empty()) return v;
                return std::nullopt;
            }
        }
    }

    // Step 3: Check SSTables (RCU snapshot of layout)
    auto snapshot = atomic_load_ptr(&layout_);  // RCU snapshot
    if (!snapshot) return std::nullopt;

    // Search from Level 0 (most recent) to deeper levels
    for (const auto& level_vec : snapshot->levels) {
        if (level_vec.empty()) continue;
        
        // Binary search to find first file that might contain key
        // (Files are sorted by min_key, so we can skip files where max_key < key)
        auto it = std::lower_bound(level_vec.begin(), level_vec.end(), key,
            [](const Layout::FileEntry& fe, std::string_view k) {
                return !fe.max_key.empty() && fe.max_key < k;
            });

        // Check files starting from the candidate (may need to check multiple
        // in Level 0 due to overlapping key ranges)
        for (auto jt = it; jt != level_vec.end(); ++jt) {
            const auto& fe = *jt;
            if (!fe.sst) continue;
            
            // Range pruning: skip if key before this file's min_key
            if (!fe.min_key.empty() && key < fe.min_key) break;
            
            // SSTable::get() performs:
            // - Bloom filter check (fast negative test)
            // - Sparse index binary search to find block
            // - Block scan with restart points for efficient search
            auto v = fe.sst->get(key);
            if (v) {
                if (!v->empty()) return v;
                return std::nullopt;  // Tombstone found
            }
        }
    }
    return std::nullopt;  // Key not found
}

std::vector<std::pair<std::string, std::string>> ZenithDB::scan(
    std::string_view start, std::string_view end) const
{
    std::vector<std::pair<std::string, std::string>> result;
    if (start > end) return result;

    {
        auto mem = atomic_load_ptr(&active_mem_);
        if (mem) {
            auto part = mem->scan(start, end);
            result.insert(result.end(), part.begin(), part.end());
        }
        for (auto* node = immut_head_.load(std::memory_order_acquire); node; node = node->next) {
            if (!node->mt) continue;
            auto part = node->mt->scan(start, end);
            result.insert(result.end(), part.begin(), part.end());
        }
    }

    auto snapshot = atomic_load_ptr(&layout_);
    if (snapshot) {
        for (const auto& level_vec : snapshot->levels) {
            for (const auto& fe : level_vec) {
                if (!fe.sst) continue;
                const auto& meta = fe.sst->meta();
                if (!meta.max_key.empty() && start > meta.max_key) continue;
                if (!meta.min_key.empty() && end < meta.min_key) continue;
                auto part = fe.sst->scan(start, end);
                result.insert(result.end(), part.begin(), part.end());
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), result.end());
    result.erase(std::remove_if(result.begin(), result.end(), [](const auto& kv) { return kv.second.empty(); }), result.end());
    return result;
}

void ZenithDB::background_worker() {
    // Main background worker loop - runs until shutdown
    while (!stop_.load(std::memory_order_acquire)) {
        bool did_work = false;
        
        // Phase 1: Flush immutable memtables to Level 0 SSTables
        {
            std::lock_guard<std::mutex> lk(writer_mutex_);
            ImmNode* head = immut_head_.load(std::memory_order_acquire);
            
            // Check if any memtables need flushing
            bool needs_flush = false;
            for (auto* node = head; node; node = node->next) {
                if (!node->flushed) {
                    needs_flush = true;
                    break;
                }
            }

            if (needs_flush) {
                // Take RCU snapshot of current layout
                auto old_layout = atomic_load_ptr(&layout_);
                if (!old_layout) {
                    old_layout = std::make_shared<Layout>();
                    old_layout->levels.resize(7);
                }
                
                // Create new layout (copy-on-write)
                auto new_layout = std::make_shared<Layout>(*old_layout);

                // Flush each unflushed memtable
                for (auto* node = head; node; node = node->next) {
                    if (!node->mt || node->flushed) continue;
                    
                    // Get sorted entries from memtable
                    auto entries = node->mt->sorted_entries();
                    if (entries.empty()) {
                        node->flushed = true;
                        continue;
                    }

                    // Create new SSTable file (writes data blocks, bloom filter, index, footer)
                    std::string filename = new_filename(0);
                    SSTable::create(data_dir_ / filename, entries);
                    
                    // Update metadata
                    levels_meta_[0].files.push_back(filename);
                    manifest_.add_sstable(0, filename);

                    // Add to new layout snapshot
                    try {
                        if (new_layout->levels.size() < levels_meta_.size()) {
                            new_layout->levels.resize(levels_meta_.size());
                        }
                        // Memory-map the new SSTable (zero-copy reads)
                        auto sst = std::make_shared<SSTable>(data_dir_ / filename);
                        Layout::FileEntry fe{sst, sst->meta().min_key, sst->meta().max_key};
                        new_layout->levels[0].push_back(std::move(fe));
                    } catch (...) {
                        // Skip if SSTable creation failed (deficit: should log error)
                    }
                    node->flushed = true;
                }
                
                // Sort Level 0 by min_key for efficient binary search
                sort_all_levels_by_min_key(*new_layout);
                
                // Publish new layout (RCU - old readers can continue using old snapshot)
                atomic_store_ptr(&layout_, new_layout);
                did_work = true;
            }
        }

        // Phase 2: Plan compaction (check thresholds)
        std::optional<CompactionTask> task;
        {
            std::lock_guard<std::mutex> lk(writer_mutex_);
            auto current_layout = atomic_load_ptr(&layout_);
            task = plan_compaction(*current_layout);
        }

        // Phase 3: Execute compaction (runs without lock - I/O heavy)
        if (task) {
            did_work = true;
            execute_compaction(*task);
            
            // Phase 4: Apply compaction results (with lock)
            {
                std::lock_guard<std::mutex> lk(writer_mutex_);
                auto old_layout = atomic_load_ptr(&layout_);
                auto new_layout = std::make_shared<Layout>(*old_layout);
                apply_compaction(*task, *new_layout);
                sort_all_levels_by_min_key(*new_layout);
                atomic_store_ptr(&layout_, new_layout);
            }
        }
        
        // Sleep if no work was done (avoid busy-waiting)
        if (!did_work) std::this_thread::sleep_for(100ms);
    }
}

std::string ZenithDB::new_filename(int level) {
    static std::atomic<std::uint64_t> counter{0};
    auto id = counter.fetch_add(1, std::memory_order_relaxed);
    return "L" + std::to_string(level) + "_" +
           std::to_string(static_cast<unsigned long long>(std::time(nullptr))) +
           "_" + std::to_string(static_cast<unsigned long long>(id)) + ".sst";
}