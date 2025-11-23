#include "db.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>

using namespace std::chrono_literals;

// ----------------------------------------------------
// Free-function wrappers for atomic ops on shared_ptr
// ----------------------------------------------------
/**
 * Atomically loads a shared_ptr with acquire memory ordering.
 * 
 * Used for RCU-style lock-free reads. The acquire ordering ensures
 * that all writes to the pointed-to object are visible after this load.
 * 
 * @param p Pointer to the atomic shared_ptr to load
 * @return A copy of the shared_ptr with acquire semantics
 */
template <typename T>
static std::shared_ptr<T> atomic_load_ptr(const std::shared_ptr<T>* p) {
    // libc++ provides atomic_load_explicit(shared_ptr<T> const*, memory_order)
    return std::atomic_load_explicit(p, std::memory_order_acquire);
}

/**
 * Atomically stores a shared_ptr with release memory ordering.
 * 
 * Used for RCU-style lock-free writes. The release ordering ensures
 * that all writes to the pointed-to object are visible before this store.
 * 
 * @param p Pointer to the atomic shared_ptr to update
 * @param v The new shared_ptr value to store
 */
template <typename T>
static void atomic_store_ptr(std::shared_ptr<T>* p, std::shared_ptr<T> v) {
    // libc++ provides atomic_store_explicit(shared_ptr<T>*, shared_ptr<T>, memory_order)
    std::atomic_store_explicit(p, std::move(v), std::memory_order_release);
}

// ----------------------------------------------------
// Sorting helpers for Layout levels
// ----------------------------------------------------

/**
 * Sorts FileEntries in a specific level by their min_key.
 * 
 * This enables efficient binary search during point lookups and
 * range pruning during scans. Must be called after modifying a level.
 */
void ZenithDB::sort_level_by_min_key(Layout& layout, std::size_t level) {
    if (level >= layout.levels.size()) return;
    auto& vec = layout.levels[level];
    std::sort(vec.begin(), vec.end(),
              [](const Layout::FileEntry& a, const Layout::FileEntry& b) {
                  return a.min_key < b.min_key;
              });
}

/**
 * Sorts all levels in the layout by min_key.
 * 
 * Convenience function to ensure proper ordering across all levels
 * after bulk operations like recovery or major compactions.
 */
void ZenithDB::sort_all_levels_by_min_key(Layout& layout) {
    for (std::size_t lvl = 0; lvl < layout.levels.size(); ++lvl) {
        sort_level_by_min_key(layout, lvl);
    }
}

// ----------------------------------------------------
// Constructor / Destructor
// ----------------------------------------------------

/**
 * Constructs and initializes a ZenithDB instance.
 * 
 * Performs full database recovery including WAL replay, manifest loading,
 * and SSTable reconstruction. Starts the background worker thread.
 */
ZenithDB::ZenithDB(const std::string& dir)
    : data_dir_(dir),
      manifest_(data_dir_) {

    std::filesystem::create_directories(data_dir_);

    // 1) WAL + active memtable
    wal_ = std::make_unique<WAL>(data_dir_.string());

    auto mem = std::make_shared<MemTable>();
    wal_->replay(mem.get());
    atomic_store_ptr(&active_mem_, mem);

    // 2) Load manifest metadata
    auto loaded = manifest_.load();  // std::vector<Level>
    std::size_t level_count = std::max<std::size_t>(7, loaded.size());
    levels_meta_ = std::move(loaded);
    levels_meta_.resize(level_count);

    // 3) Build initial layout snapshot (open SSTables once)
    auto layout = std::make_shared<Layout>();
    layout->levels.resize(level_count);

    for (std::size_t lvl = 0; lvl < levels_meta_.size(); ++lvl) {
        for (const auto& fname : levels_meta_[lvl].files) {
            auto path = data_dir_ / fname;
            if (!std::filesystem::exists(path)) continue;
            try {
                auto sst = std::make_shared<SSTable>(path);
                Layout::FileEntry fe;
                fe.sst     = sst;
                fe.min_key = sst->meta().min_key;
                fe.max_key = sst->meta().max_key;
                layout->levels[lvl].push_back(std::move(fe));
            } catch (const std::exception& e) {
                std::cerr << "[ZenithDB] Failed to open SSTable " << path
                          << ": " << e.what() << "\n";
            }
        }
    }

    // Ensure per-level ordering by min_key
    sort_all_levels_by_min_key(*layout);

    atomic_store_ptr(&layout_, layout);

    // 4) Start background worker
    worker_ = std::thread(&ZenithDB::background_worker, this);
}

/**
 * Destructor that gracefully shuts down the database.
 * 
 * Stops background operations, syncs WAL, and cleans up resources.
 */
ZenithDB::~ZenithDB() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (wal_) {
        wal_->sync();
    }
    // ImmNode objects and old memtables are intentionally leaked for
    // simplicity; a production engine would add epoch-based reclamation.
}

// ----------------------------------------------------
// Writes
// ----------------------------------------------------

/**
 * Inserts or updates a key-value pair.
 * 
 * Writes are fast because they only touch memory (memtable + WAL).
 * When the memtable grows too large, it's automatically frozen and
 * a new one is created. Frozen memtables are flushed in the background.
 */
void ZenithDB::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(writer_mutex_);

    auto mem = atomic_load_ptr(&active_mem_);
    if (!mem) {
        mem = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, mem);
    }

    mem->put(key, value);
    wal_->append("PUT|" + key + "|" + value);

    if (mem->approximate_size() > 2 * MEMTABLE_LIMIT) {
        // Freeze current memtable and install a new one
        auto frozen = mem;
        auto fresh  = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, fresh);

        // Prepend to immutable chain lock-free
        ImmNode* old_head = immut_head_.load(std::memory_order_acquire);
        auto* node        = new ImmNode{frozen, old_head, false};

        while (!immut_head_.compare_exchange_weak(
                   old_head, node,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
            node->next = old_head;
        }
    }
}

/**
 * Deletes a key by inserting a tombstone.
 * 
 * The tombstone will hide the key immediately and eventually
 * cause it to be removed during compaction.
 */
void ZenithDB::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(writer_mutex_);

    auto mem = atomic_load_ptr(&active_mem_);
    if (!mem) {
        mem = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, mem);
    }

    mem->remove(key);  // tombstone (empty string)
    wal_->append("DEL|" + key + "|");

    if (mem->approximate_size() > 2 * MEMTABLE_LIMIT) {
        auto frozen = mem;
        auto fresh  = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, fresh);

        ImmNode* old_head = immut_head_.load(std::memory_order_acquire);
        auto* node        = new ImmNode{frozen, old_head, false};

        while (!immut_head_.compare_exchange_weak(
                   old_head, node,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
            node->next = old_head;
        }
    }
}

// ----------------------------------------------------
// Reads
// ----------------------------------------------------

/**
 * Performs a point lookup for a key.
 * 
 * Uses range pruning to skip memtables and SSTables that cannot
 * contain the key, and bloom filters to quickly reject SSTables.
 * Lock-free for readers using RCU semantics.
 */
std::optional<std::string> ZenithDB::get(const std::string& key) const {
    // 1) Active memtable (range-aware)
    {
        auto mem = atomic_load_ptr(&active_mem_);
        if (mem && mem->has_range()) {
            // Skip if key is definitely outside this memtable's range
            if (key >= mem->min_key() && key <= mem->max_key()) {
                if (auto v = mem->get(key)) {
                    if (!v->empty()) return v;
                    // empty string = tombstone => treat as not found
                    return std::nullopt;
                }
            }
        }

        // 2) Immutable memtables (lock-free chain, range-aware)
        for (auto* node = immut_head_.load(std::memory_order_acquire);
             node != nullptr;
             node = node->next) {

            auto mt = node->mt;
            if (!mt || !mt->has_range()) continue;

            // If key is outside this memtable's range, skip it
            if (key < mt->min_key() || key > mt->max_key()) {
                continue;
            }

            if (auto v = mt->get(key)) {
                if (!v->empty()) return v;
                return std::nullopt;
            }
        }
    }

    // 3) On-disk layout (RCU snapshot) – unchanged, still tree/range + bloom
    auto snapshot = atomic_load_ptr(&layout_);
    if (!snapshot) {
        return std::nullopt;
    }

    for (const auto& level_vec : snapshot->levels) {
        if (level_vec.empty()) continue;

        auto it = std::lower_bound(
            level_vec.begin(), level_vec.end(), key,
            [](const Layout::FileEntry& fe, const std::string& k) {
                if (!fe.max_key.empty()) {
                    return fe.max_key < k;
                }
                return false;
            });

        for (auto jt = it; jt != level_vec.end(); ++jt) {
            const auto& fe = *jt;
            if (!fe.sst) continue;

            if (!fe.min_key.empty() && key < fe.min_key) {
                break;
            }

            auto v = fe.sst->get(key);
            if (v) {
                if (!v->empty()) return v;
                return std::nullopt;
            }
        }
    }

    return std::nullopt;
}
/**
 * Performs a range scan over keys.
 * 
 * Searches all memtables and SSTables, merges results, deduplicates
 * by key (keeping latest), and filters tombstones. Results are sorted.
 */
std::vector<std::pair<std::string, std::string>> ZenithDB::scan(
    const std::string& start,
    const std::string& end) const
{
    std::vector<std::pair<std::string, std::string>> result;
    if (start > end) return result;

    // ---- 1) Memtables (active + immutable) ----
    {
        auto mem = atomic_load_ptr(&active_mem_);
        if (mem) {
            auto part = mem->scan(start, end);
            result.insert(result.end(), part.begin(), part.end());
        }

        for (auto* node = immut_head_.load(std::memory_order_acquire);
             node != nullptr;
             node = node->next) {
            if (!node->mt) continue;
            auto part = node->mt->scan(start, end);
            result.insert(result.end(), part.begin(), part.end());
        }
    }

    // ---- 2) Disk layout (RCU snapshot) ----
    auto snapshot = atomic_load_ptr(&layout_);
    if (snapshot) {
        for (const auto& level_vec : snapshot->levels) {
            for (const auto& fe : level_vec) {
                if (!fe.sst) continue;

                const auto& meta = fe.sst->meta();

                // Range pruning with min/max if available
                if (!meta.max_key.empty() && start > meta.max_key) continue;
                if (!meta.min_key.empty() && end   < meta.min_key) continue;

                auto part = fe.sst->scan(start, end);
                result.insert(result.end(), part.begin(), part.end());
            }
        }
    }

    // ---- 3) Deduplicate by key; latest wins ----
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });

    result.erase(
        std::unique(result.begin(), result.end(),
                    [](const auto& a, const auto& b) {
                        return a.first == b.first;
                    }),
        result.end()
    );

    // Drop tombstones
    result.erase(
        std::remove_if(result.begin(), result.end(),
                       [](const auto& kv) { return kv.second.empty(); }),
        result.end()
    );

    return result;
}

// ----------------------------------------------------
// Background worker & compaction
// ----------------------------------------------------

/**
 * Background worker thread that handles flushing and compaction.
 * 
 * Runs in a loop, periodically:
 * 1. Flushing immutable memtables to Level 0
 * 2. Compacting levels that exceed their thresholds
 * 3. Publishing new RCU layout snapshots
 * 
 * Uses writer_mutex_ to serialize all write-side operations.
 */
void ZenithDB::background_worker() {
    while (!stop_.load(std::memory_order_acquire)) {
        // Snapshot current layout
        auto old_layout = atomic_load_ptr(&layout_);
        if (!old_layout) {
            old_layout = std::make_shared<Layout>();
            old_layout->levels.resize(7);
        }

        auto new_layout = std::make_shared<Layout>(*old_layout);

        {
            // Serialize flush + compaction + manifest updates
            std::lock_guard<std::mutex> lk(writer_mutex_);

            // 1) Flush immutable memtables to Level 0
            ImmNode* head = immut_head_.load(std::memory_order_acquire);

            for (auto* node = head; node != nullptr; node = node->next) {
                if (!node->mt || node->flushed) continue;

                auto entries = node->mt->sorted_entries();
                if (entries.empty()) {
                    node->flushed = true;
                    continue;
                }

                std::string filename = new_filename(0);
                SSTable::create(data_dir_ / filename, entries);

                levels_meta_[0].files.push_back(filename);
                manifest_.add_sstable(0, filename);

                try {
                    if (new_layout->levels.size() < levels_meta_.size()) {
                        new_layout->levels.resize(levels_meta_.size());
                    }
                    auto sst = std::make_shared<SSTable>(data_dir_ / filename);
                    Layout::FileEntry fe;
                    fe.sst     = sst;
                    fe.min_key = sst->meta().min_key;
                    fe.max_key = sst->meta().max_key;
                    new_layout->levels[0].push_back(std::move(fe));
                } catch (const std::exception& e) {
                    std::cerr << "[ZenithDB] Failed to open flushed SSTable "
                              << filename << ": " << e.what() << "\n";
                }

                node->flushed = true;
            }

            // 2) Simple compaction policy: at most one compaction per iteration
            for (std::size_t lvl = 0; lvl + 1 < levels_meta_.size(); ++lvl) {
                std::size_t threshold = (lvl == 0) ? 3 : 4;
                if (levels_meta_[lvl].files.size() >= threshold) {
                    compact_level(static_cast<int>(lvl), *new_layout);
                    break;
                }
            }
        }

        // Ensure levels remain sorted by min_key after flush/compaction
        sort_all_levels_by_min_key(*new_layout);

        // 3) Publish new layout snapshot
        atomic_store_ptr(&layout_, new_layout);

        std::this_thread::sleep_for(100ms);
    }
}

// ----------------------------------------------------
// Helpers
// ----------------------------------------------------

/**
 * Generates a unique SSTable filename.
 * 
 * Format ensures uniqueness and includes level number for organization.
 */
std::string ZenithDB::new_filename(int level) {
    static std::atomic<std::uint64_t> counter{0};
    auto id = counter.fetch_add(1, std::memory_order_relaxed);

    return "L" + std::to_string(level) + "_" +
           std::to_string(static_cast<unsigned long long>(std::time(nullptr))) +
           "_" + std::to_string(static_cast<unsigned long long>(id)) + ".sst";
}