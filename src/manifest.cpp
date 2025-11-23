// src/manifest.cpp
#include "manifest.h"
#include <fstream>
#include <sstream>

/**
 * Constructs a Manifest that uses "MANIFEST" file in the given directory.
 */
Manifest::Manifest(const std::filesystem::path& dir) : path_(dir / "MANIFEST") {}

/**
 * Appends an "ADD" record to the manifest log.
 */
void Manifest::add_sstable(int level, const std::string& filename) {
    std::ofstream out(path_, std::ios::app);
    if (out) {
        out << "ADD " << level << " " << filename << '\n';
    }
}

/**
 * Records file replacements by writing DEL records for old files
 * and ADD records for new files.
 */
void Manifest::replace(int level,
                      const std::vector<std::string>& old_files,
                      const std::vector<std::string>& new_files) {
    std::ofstream out(path_, std::ios::app);
    if (!out) return;

    for (const auto& f : old_files) {
        out << "DEL " << level << " " << f << '\n';
    }
    for (const auto& f : new_files) {
        out << "ADD " << level << " " << f << '\n';
    }
}

/**
 * Loads the manifest and reconstructs the level structure.
 * 
 * Reads all ADD records and builds a vector of Level structures.
 * DEL records are ignored during recovery (we only care about current state).
 */
std::vector<Level> Manifest::load() {
    std::vector<Level> levels(7);

    if (!std::filesystem::exists(path_)) {
        return levels;
    }

    std::ifstream in(path_);
    std::string line;

    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string op;
        int lvl;
        std::string file;

        if (!(iss >> op >> lvl)) continue;

        if (op == "ADD" && iss >> file) {
            if (lvl >= 0 && lvl < levels.size()) {
                levels[lvl].files.push_back(file);
            }
        }
        // Ignore DEL during recovery
    }
    return levels;
}