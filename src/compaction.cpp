#include "db.h"
#include <algorithm>
#include <set>

void ZenithDB::compact_level(int level) {
    if (level >= levels_.size() - 1) return;

    auto& curr = levels_[level];
    auto& next = levels_[level + 1];

    if (curr.files.size() < curr.max_files) return;

    // simple: take all files in level
    std::vector<std::unique_ptr<SSTable>> tables;
    for (const auto& f : curr.files) {
        tables.emplace_back(std::make_unique<SSTable>(data_dir_ / f));
    }

    std::vector<std::pair<std::string, std::string>> merged;
    std::set<std::string> seen;

    // merge + tombstone elimination
    for (auto& table : tables) {
        auto entries = table->scan("", "\xFF\xFF\xFF\xFF");
        for (auto& [k, v] : entries) {
            if (v.empty()) seen.insert(k);           // tombstone
            else if (seen.find(k) == seen.end()) {
                merged.emplace_back(k, v);
            }
        }
    }

    // remove tombstones
    merged.erase(
        std::remove_if(merged.begin(), merged.end(),
            [&](auto& p) { return seen.count(p.first); }),
        merged.end());

    // write_new_level(level + 1, merged);

    manifest_.replace(level, curr.files, {});
    manifest_.replace(level + 1, next.files, {new_filename(level + 1)});
    curr.files.clear();
}