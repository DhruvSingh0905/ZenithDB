#include "db.h"

#include <map>
#include <iostream>

/**
 * Compacts all SSTables in a level into the next level.
 * 
 * This function:
 * 1. Collects all SSTables from the source level
 * 2. Merges their key-value pairs (keeping latest value for duplicates)
 * 3. Removes tombstones and their corresponding keys
 * 4. Writes a new merged SSTable to the next level
 * 5. Updates the manifest to record the file changes
 * 6. Updates the layout with the new SSTable
 * 
 * Compaction reduces read amplification and reclaims space from
 * deleted keys. It's triggered when a level exceeds its file threshold.
 * 
 * @param level The level to compact (must be < levels_meta_.size() - 1)
 * @param layout The layout snapshot to update with new SSTable entries
 */
void ZenithDB::compact_level(int level, Layout& layout) {
    if (level < 0 ||
        static_cast<std::size_t>(level + 1) >= levels_meta_.size()) {
        return;
    }

    std::size_t threshold = (level == 0) ? 3 : 4;
    if (levels_meta_[level].files.size() < threshold) {
        return;
    }

    // Compact *all* SSTables in this level into the next level.
    auto input_files = std::move(levels_meta_[level].files);
    levels_meta_[level].files.clear();

    auto& level_vec = layout.levels[level];

    // Extract the SSTable pointers from FileEntry vector
    std::vector<std::shared_ptr<SSTable>> input_ssts;
    input_ssts.reserve(level_vec.size());
    for (auto& fe : level_vec) {
        if (fe.sst) {
            input_ssts.push_back(std::move(fe.sst));
        }
    }
    level_vec.clear();

    // Merge all keys from these SSTables
    std::map<std::string, std::string> merged;

    for (const auto& sst : input_ssts) {
        if (!sst) continue;
        auto rows = sst->scan("", "\xFF\xFF\xFF\xFF");
        for (auto& [k, v] : rows) {
            if (v.empty()) {
                merged.erase(k);  // tombstone
            } else {
                merged[k] = std::move(v);
            }
        }
    }

    if (merged.empty()) {
        // No live keys; drop these files in manifest
        manifest_.replace(level, input_files, {});
        return;
    }

    // Write merged SSTable into next level
    std::string new_file = new_filename(level + 1);

    std::vector<std::pair<std::string, std::string>> vec;
    vec.reserve(merged.size());
    for (auto& kv : merged) {
        vec.emplace_back(std::move(kv.first), std::move(kv.second));
    }

    SSTable::create(data_dir_ / new_file, vec);

    levels_meta_[level + 1].files.push_back(new_file);
    manifest_.add_sstable(level + 1, new_file);
    manifest_.replace(level, input_files, {});

    // Add to layout
    if (layout.levels.size() <= static_cast<std::size_t>(level + 1)) {
        layout.levels.resize(level + 2);
    }

    try {
        auto sst = std::make_shared<SSTable>(data_dir_ / new_file);
        Layout::FileEntry fe;
        fe.sst     = sst;
        fe.min_key = sst->meta().min_key;
        fe.max_key = sst->meta().max_key;
        layout.levels[level + 1].push_back(std::move(fe));
    } catch (const std::exception& e) {
        std::cerr << "[ZenithDB] Failed to open compacted SSTable "
                  << new_file << ": " << e.what() << "\n";
    }
}