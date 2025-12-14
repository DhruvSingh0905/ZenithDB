#include "db.h"
#include "crdt.h"
#include <algorithm>
#include <map>
#include <iostream>

std::optional<ZenithDB::CompactionTask> ZenithDB::plan_compaction(const Layout& layout) {
    for (std::size_t lvl = 0; lvl + 1 < levels_meta_.size(); ++lvl) {
        std::size_t threshold = (lvl == 0) ? 3 : 4;
        if (levels_meta_[lvl].files.size() >= threshold) {
            CompactionTask task;
            task.level = static_cast<int>(lvl);
            task.input_filenames = levels_meta_[lvl].files;
            
            const auto& level_vec = layout.levels[lvl];
            for (const auto& fe : level_vec) {
                if (fe.sst) task.input_ssts.push_back(fe.sst);
            }
            task.output_filename = new_filename(task.level + 1);
            return task;
        }
    }
    return std::nullopt;
}

void ZenithDB::execute_compaction(CompactionTask& task) {
    // Map stores Key -> Serialized CRDT
    std::map<std::string, std::string> merged;

    for (const auto& sst : task.input_ssts) {
        auto rows = sst->scan("", "\xFF\xFF\xFF\xFF");
        for (auto& [k, v_serialized] : rows) {
            
            // Deserialize new candidate
            LWWRegister incoming = LWWRegister::deserialize(v_serialized);

            auto it = merged.find(k);
            if (it == merged.end()) {
                // First time seeing this key in this compaction run
                merged[k] = v_serialized;
            } else {
                // Key exists, MUST MERGE
                LWWRegister existing = LWWRegister::deserialize(it->second);
                
                // CRDT Merge
                existing.merge(incoming);
                
                // Store result back
                merged[k] = existing.serialize();
            }
        }
    }

    if (merged.empty()) return;

    std::vector<std::pair<std::string, std::string>> vec;
    vec.reserve(merged.size());
    for (auto& kv : merged) {
        vec.emplace_back(std::move(kv.first), std::move(kv.second));
    }

    auto out_path = data_dir_ / task.output_filename;
    SSTable::create(env_.get(), out_path, vec);

    try {
        task.output_sst = std::make_shared<SSTable>(env_.get(), out_path);
    } catch (...) {}
}

void ZenithDB::apply_compaction(const CompactionTask& task, Layout& new_layout) {
    if (task.output_sst) {
        manifest_->add_sstable(task.level + 1, task.output_filename); // Changed . to ->
    }
    manifest_->replace(task.level, task.input_filenames, {}); // Changed . to ->

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

    if (task.output_sst) {
        levels_meta_[task.level + 1].files.push_back(task.output_filename);
    }

    auto& level_vec = new_layout.levels[task.level];
    std::vector<Layout::FileEntry> kept_entries;
    for (const auto& fe : level_vec) {
        bool is_input = false;
        for (const auto& input_sst : task.input_ssts) {
            if (fe.sst == input_sst) { is_input = true; break; }
        }
        if (!is_input) kept_entries.push_back(fe);
    }
    level_vec = std::move(kept_entries);

    if (task.output_sst) {
        if (new_layout.levels.size() <= static_cast<size_t>(task.level + 1)) {
            new_layout.levels.resize(task.level + 2);
        }
        Layout::FileEntry fe{task.output_sst, task.output_sst->meta().min_key, task.output_sst->meta().max_key};
        new_layout.levels[task.level + 1].push_back(std::move(fe));
    }
}