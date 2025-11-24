#include "db.h"

#include <algorithm>
#include <map>
#include <iostream>

/**
 * Phase 1: Plan Compaction (Locked)
 * Checks thresholds and creates a task with a snapshot of files to compact.
 */
std::optional<ZenithDB::CompactionTask> ZenithDB::plan_compaction(const Layout& layout) {
    // We only compact one level at a time for simplicity
    for (std::size_t lvl = 0; lvl + 1 < levels_meta_.size(); ++lvl) {
        std::size_t threshold = (lvl == 0) ? 3 : 4;
        
        // If threshold met, create a job
        if (levels_meta_[lvl].files.size() >= threshold) {
            CompactionTask task;
            task.level = static_cast<int>(lvl);
            
            // Snapshot the filenames currently in this level
            task.input_filenames = levels_meta_[lvl].files;
            
            // Gather the corresponding SSTables from the current layout
            // (Note: layout matches levels_meta_ because we hold the lock)
            const auto& level_vec = layout.levels[lvl];
            for (const auto& fe : level_vec) {
                if (fe.sst) {
                    task.input_ssts.push_back(fe.sst);
                }
            }

            // Generate output filename ahead of time
            task.output_filename = new_filename(task.level + 1);
            
            return task;
        }
    }
    return std::nullopt;
}

/**
 * Phase 2: Execute Compaction (Unlocked)
 * Merges data and writes to a new SSTable file.
 * This is the I/O heavy part and runs without holding writer_mutex_.
 */
void ZenithDB::execute_compaction(CompactionTask& task) {
    std::map<std::string, std::string> merged;

    // 1. Merge all input SSTables
    for (const auto& sst : task.input_ssts) {
        // Full scan of the SSTable
        // Note: Optimized implementation would use a merging iterator instead of loading all to map
        auto rows = sst->scan("", "\xFF\xFF\xFF\xFF");
        for (auto& [k, v] : rows) {
            if (v.empty()) {
                merged.erase(k); // Tombstone handling
            } else {
                merged[k] = std::move(v);
            }
        }
    }

    if (merged.empty()) {
        // Everything was deleted/tombstoned
        return;
    }

    // 2. Write to new SSTable file
    std::vector<std::pair<std::string, std::string>> vec;
    vec.reserve(merged.size());
    for (auto& kv : merged) {
        vec.emplace_back(std::move(kv.first), std::move(kv.second));
    }

    auto out_path = data_dir_ / task.output_filename;
    SSTable::create(out_path, vec);

    // 3. Pre-load the new SSTable to ensure it's valid and ready for Layout
    try {
        task.output_sst = std::make_shared<SSTable>(out_path);
    } catch (const std::exception& e) {
        std::cerr << "[ZenithDB] Compaction failed to open new SST " 
                  << task.output_filename << ": " << e.what() << "\n";
        // task.output_sst remains null, handled in apply phase
    }
}

/**
 * Phase 3: Apply Compaction (Locked)
 * Updates manifest, levels metadata, and the RCU layout.
 */
void ZenithDB::apply_compaction(const CompactionTask& task, Layout& new_layout) {
    // 1. Update Manifest (Persist the change)
    if (task.output_sst) {
        manifest_.add_sstable(task.level + 1, task.output_filename);
    }
    manifest_.replace(task.level, task.input_filenames, {});

    // 2. Update levels_meta_ (The source of truth for structure)
    // We must strictly remove ONLY the files we compacted. 
    // New files might have been flushed to L0 while we were compacting.
    
    // Remove inputs from current level
    auto& current_files = levels_meta_[task.level].files;
    std::vector<std::string> remaining;
    for (const auto& f : current_files) {
        bool processed = false;
        for (const auto& done : task.input_filenames) {
            if (f == done) { processed = true; break; }
        }
        if (!processed) remaining.push_back(f);
    }
    levels_meta_[task.level].files = std::move(remaining);

    // Add output to next level
    if (task.output_sst) {
        levels_meta_[task.level + 1].files.push_back(task.output_filename);
    }

    // 3. Update the New Layout (The RCU snapshot)
    
    // Rebuild the vector for the compacted level in the new layout
    // (Filtering out the SSTs we just compacted)
    auto& level_vec = new_layout.levels[task.level];
    std::vector<Layout::FileEntry> kept_entries;
    
    // We can identify SSTs by memory address or filename. 
    // Since Layout doesn't store filename, we rely on the fact that 
    // plan_compaction grabbed specific shared_ptrs.
    for (const auto& fe : level_vec) {
        bool is_input = false;
        for (const auto& input_sst : task.input_ssts) {
            if (fe.sst == input_sst) {
                is_input = true;
                break;
            }
        }
        if (!is_input) {
            kept_entries.push_back(fe);
        }
    }
    level_vec = std::move(kept_entries);

    // Add the new SSTable to the next level
    if (task.output_sst) {
        if (new_layout.levels.size() <= static_cast<size_t>(task.level + 1)) {
            new_layout.levels.resize(task.level + 2);
        }
        
        Layout::FileEntry fe;
        fe.sst = task.output_sst;
        fe.min_key = task.output_sst->meta().min_key;
        fe.max_key = task.output_sst->meta().max_key;
        
        new_layout.levels[task.level + 1].push_back(std::move(fe));
    }
}