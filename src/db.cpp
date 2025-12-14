#include "db.h"
#include "crdt.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <thread>
#include <set>

using namespace std::chrono_literals;

extern std::unique_ptr<Env> NewPosixEnv();

template <typename T>
static std::shared_ptr<T> atomic_load_ptr(const std::shared_ptr<T>* p) {
    return std::atomic_load_explicit(p, std::memory_order_acquire);
}

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

// -----------------------------------------------------------------------------
// Constructors
// -----------------------------------------------------------------------------

ZenithDB::ZenithDB(std::unique_ptr<Env> env, const std::string& dir, const std::string& node_id, bool sync_writes)
    : data_dir_(dir),
      env_(std::move(env)),
      sync_writes_(sync_writes),
      node_id_(node_id)
{
    env_->CreateDir(data_dir_);
    manifest_ = std::make_unique<Manifest>(env_.get(), data_dir_);
    wal_ = std::make_unique<WAL>(env_.get(), data_dir_);

    auto mem = std::make_shared<MemTable>();
    wal_->replay(mem.get());
    atomic_store_ptr(&active_mem_, mem);

    auto loaded = manifest_->load();
    std::size_t level_count = std::max<std::size_t>(7, loaded.size());
    levels_meta_ = std::move(loaded);
    levels_meta_.resize(level_count);

    auto layout = std::make_shared<Layout>();
    layout->levels.resize(level_count);

    for (std::size_t lvl = 0; lvl < levels_meta_.size(); ++lvl) {
        for (const auto& fname : levels_meta_[lvl].files) {
            auto path = data_dir_ / fname;
            if (!env_->FileExists(path)) continue;
            try {
                auto sst = std::make_shared<SSTable>(env_.get(), path);
                Layout::FileEntry fe;
                fe.sst     = sst;
                fe.min_key = sst->meta().min_key;
                fe.max_key = sst->meta().max_key;
                layout->levels[lvl].push_back(std::move(fe));
            } catch (const std::exception& e) {
                std::cerr << "[ZenithDB] Failed to open SSTable " << path << ": " << e.what() << "\n";
            }
        }
    }
    sort_all_levels_by_min_key(*layout);
    atomic_store_ptr(&layout_, layout);
    worker_ = std::thread(&ZenithDB::background_worker, this);
}

ZenithDB::ZenithDB(const std::string& dir, const std::string& node_id, bool sync_writes)
    : ZenithDB(NewPosixEnv(), dir, node_id, sync_writes) 
{
}

ZenithDB::~ZenithDB() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    if (wal_) wal_->sync();
}

// -----------------------------------------------------------------------------
// IO Operations
// -----------------------------------------------------------------------------

void ZenithDB::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(writer_mutex_);
    
    // 1. Advance Local Clock (Causality for new local write)
    local_clock_.increment(node_id_);
    
    LWWRegister reg(value, local_clock_);
    std::string serialized = reg.serialize();

    auto mem = atomic_load_ptr(&active_mem_);
    if (!mem) { mem = std::make_shared<MemTable>(); atomic_store_ptr(&active_mem_, mem); }

    mem->put(key, serialized);
    wal_->append("PUT|" + key + "|" + serialized);

    if (sync_writes_) wal_->sync();

    if (mem->approximate_size() > 2 * MEMTABLE_LIMIT) {
        auto frozen = mem;
        auto fresh  = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, fresh);
        ImmNode* old_head = immut_head_.load(std::memory_order_acquire);
        auto* node = new ImmNode{frozen, old_head, false};
        while (!immut_head_.compare_exchange_weak(old_head, node, std::memory_order_release, std::memory_order_acquire)) {
            node->next = old_head;
        }
    }
}

// FIX: Read-Modify-Write for Incoming Replication
void ZenithDB::put(const std::string& key, const LWWRegister& incoming_crdt) {
    std::lock_guard<std::mutex> lk(writer_mutex_);
    
    // 1. Merge clocks to track causal history from peer
    local_clock_.merge(incoming_crdt.clock);
    
    // 2. Resolve Conflict (Read existing -> Merge -> Write winner)
    LWWRegister final_crdt = incoming_crdt;
    
    // We can call get_crdt() even while holding writer_mutex_ because 
    // get_crdt only uses atomic loads (RCU), no locks.
    auto existing_opt = get_crdt(key);
    
    if (existing_opt) {
        // Merge logic: 'final_crdt' becomes the winner
        final_crdt.merge(*existing_opt);
        
        // Optimization: If the winner is identical to what we already have,
        // we can skip the write to avoid log growth (optional but good).
        // For simplicity/safety in this phase, we just write the winner.
    }

    std::string serialized = final_crdt.serialize();
    
    auto mem = atomic_load_ptr(&active_mem_);
    if (!mem) { mem = std::make_shared<MemTable>(); atomic_store_ptr(&active_mem_, mem); }

    mem->put(key, serialized);
    wal_->append("PUT|" + key + "|" + serialized);

    if (sync_writes_) wal_->sync();

    if (mem->approximate_size() > 2 * MEMTABLE_LIMIT) {
        auto frozen = mem;
        auto fresh  = std::make_shared<MemTable>();
        atomic_store_ptr(&active_mem_, fresh);
        ImmNode* old_head = immut_head_.load(std::memory_order_acquire);
        auto* node = new ImmNode{frozen, old_head, false};
        while (!immut_head_.compare_exchange_weak(old_head, node, std::memory_order_release, std::memory_order_acquire)) {
            node->next = old_head;
        }
    }
}

void ZenithDB::remove(const std::string& key) {
    put(key, "");
}

std::vector<std::pair<std::string, std::string>> ZenithDB::scan(std::string_view start, std::string_view end) const {
    auto crdt_list = scan_crdt(start, end);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(crdt_list.size());
    for(auto& kv : crdt_list) {
        out.emplace_back(kv.first, kv.second.value);
    }
    return out;
}

std::optional<std::string> ZenithDB::get(std::string_view key) const {
    auto crdt = get_crdt(key);
    if (!crdt) return std::nullopt;
    return crdt->value;
}

// -----------------------------------------------------------------------------
// NEW: Raw CRDT Access
// -----------------------------------------------------------------------------

std::optional<LWWRegister> ZenithDB::get_crdt(std::string_view key) const {
    std::string raw_serialized_value;
    bool found = false;

    // 1. Check Active Memtable
    {
        auto mem = atomic_load_ptr(&active_mem_);
        if (mem && mem->has_range()) {
            if (key >= mem->min_key() && key <= mem->max_key()) {
                if (auto v = mem->get(key)) {
                    if (!v->empty()) { raw_serialized_value = *v; found = true; goto deserialize; }
                }
            }
        }
        for (auto* node = immut_head_.load(std::memory_order_acquire); node; node = node->next) {
            auto mt = node->mt;
            if (!mt || !mt->has_range()) continue;
            if (key < mt->min_key() || key > mt->max_key()) continue;
            if (auto v = mt->get(key)) {
                if (!v->empty()) { raw_serialized_value = *v; found = true; goto deserialize; }
            }
        }
    }

    // 2. Check SSTables
    {
        auto snapshot = atomic_load_ptr(&layout_);
        if (snapshot) {
            for (const auto& level_vec : snapshot->levels) {
                if (level_vec.empty()) continue;
                auto it = std::lower_bound(level_vec.begin(), level_vec.end(), key,
                    [](const Layout::FileEntry& fe, std::string_view k) {
                        return !fe.max_key.empty() && fe.max_key < k;
                    });

                for (auto jt = it; jt != level_vec.end(); ++jt) {
                    if (!jt->sst) continue;
                    if (!jt->min_key.empty() && key < jt->min_key) break;
                    auto v = jt->sst->get(key);
                    if (v) {
                        if (!v->empty()) { raw_serialized_value = *v; found = true; goto deserialize; }
                    }
                }
            }
        }
    }

deserialize:
    if (!found) return std::nullopt;
    auto reg = LWWRegister::deserialize(raw_serialized_value);
    if (reg.value.empty()) return std::nullopt;
    return reg;
}

std::vector<std::pair<std::string, LWWRegister>> ZenithDB::scan_crdt(
    std::string_view start, std::string_view end) const
{
    std::vector<std::pair<std::string, std::string>> raw_results;
    if (start > end) return {};

    // Collect Raw
    {
        auto mem = atomic_load_ptr(&active_mem_);
        if (mem) {
            auto part = mem->scan(start, end);
            raw_results.insert(raw_results.end(), part.begin(), part.end());
        }
        for (auto* node = immut_head_.load(std::memory_order_acquire); node; node = node->next) {
            if (!node->mt) continue;
            auto part = node->mt->scan(start, end);
            raw_results.insert(raw_results.end(), part.begin(), part.end());
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
                raw_results.insert(raw_results.end(), part.begin(), part.end());
            }
        }
    }

    // Sort by Key
    std::sort(raw_results.begin(), raw_results.end(), 
        [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<std::string, std::string>> final_raw;
    // Merge Logic (Last Write Wins)
    for (const auto& kv : raw_results) {
        if (final_raw.empty() || final_raw.back().first != kv.first) {
            final_raw.push_back(kv);
        } else {
            std::string& existing_raw = final_raw.back().second;
            const std::string& incoming_raw = kv.second;
            LWWRegister existing = LWWRegister::deserialize(existing_raw);
            LWWRegister incoming = LWWRegister::deserialize(incoming_raw);
            existing.merge(incoming);
            existing_raw = existing.serialize();
        }
    }

    // Deserialize and filter
    std::vector<std::pair<std::string, LWWRegister>> out;
    out.reserve(final_raw.size());
    for (const auto& kv : final_raw) {
        LWWRegister reg = LWWRegister::deserialize(kv.second);
        if (!reg.value.empty()) {
            out.emplace_back(kv.first, std::move(reg));
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Background Worker
// -----------------------------------------------------------------------------

void ZenithDB::background_worker() {
    while (!stop_.load(std::memory_order_acquire)) {
        bool did_work = false;
        {
            std::lock_guard<std::mutex> lk(writer_mutex_);
            ImmNode* head = immut_head_.load(std::memory_order_acquire);
            bool needs_flush = false;
            for (auto* node = head; node; node = node->next) { if (!node->flushed) { needs_flush = true; break; } }

            if (needs_flush) {
                auto old_layout = atomic_load_ptr(&layout_);
                if (!old_layout) { old_layout = std::make_shared<Layout>(); old_layout->levels.resize(7); }
                auto new_layout = std::make_shared<Layout>(*old_layout);

                for (auto* node = head; node; node = node->next) {
                    if (!node->mt || node->flushed) continue;
                    auto entries = node->mt->sorted_entries();
                    if (entries.empty()) { node->flushed = true; continue; }

                    std::string filename = new_filename(0);
                    SSTable::create(env_.get(), data_dir_ / filename, entries);
                    levels_meta_[0].files.push_back(filename);
                    manifest_->add_sstable(0, filename);

                    try {
                        if (new_layout->levels.size() < levels_meta_.size()) new_layout->levels.resize(levels_meta_.size());
                        auto sst = std::make_shared<SSTable>(env_.get(), data_dir_ / filename);
                        Layout::FileEntry fe{sst, sst->meta().min_key, sst->meta().max_key};
                        new_layout->levels[0].push_back(std::move(fe));
                    } catch (...) {}
                    node->flushed = true;
                }
                sort_all_levels_by_min_key(*new_layout);
                atomic_store_ptr(&layout_, new_layout);
                did_work = true;
            }
        }

        std::optional<CompactionTask> task;
        {
            std::lock_guard<std::mutex> lk(writer_mutex_);
            auto current_layout = atomic_load_ptr(&layout_);
            task = plan_compaction(*current_layout);
        }

        if (task) {
            did_work = true;
            execute_compaction(*task);
            {
                std::lock_guard<std::mutex> lk(writer_mutex_);
                auto old_layout = atomic_load_ptr(&layout_);
                auto new_layout = std::make_shared<Layout>(*old_layout);
                apply_compaction(*task, *new_layout);
                sort_all_levels_by_min_key(*new_layout);
                atomic_store_ptr(&layout_, new_layout);
            }
        }
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