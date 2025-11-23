// src/manifest.h
#pragma once
#include "level.h"
#include <string>
#include <vector>
#include <filesystem>

class Manifest {
public:
    explicit Manifest(const std::filesystem::path& dir);

    void add_sstable(int level, const std::string& filename);
    void replace(int level,
                 const std::vector<std::string>& old_files,
                 const std::vector<std::string>& new_files);

    std::vector<Level> load();  // returns shared Level

private:
    std::filesystem::path path_;
};