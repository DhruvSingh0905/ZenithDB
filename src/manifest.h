// src/manifest.h
#pragma once
#include "env.h"
#include "level.h"
#include <string>
#include <vector>
#include <filesystem>
#include <memory>

class Manifest {
public:
    Manifest(Env* env, const std::filesystem::path& dir);

    void add_sstable(int level, const std::string& filename);
    
    void replace(int level,
                 const std::vector<std::string>& old_files,
                 const std::vector<std::string>& new_files);

    std::vector<Level> load();

private:
    Env* env_;
    std::filesystem::path path_;
    std::unique_ptr<WritableFile> manifest_file_; // Kept open for appending
};