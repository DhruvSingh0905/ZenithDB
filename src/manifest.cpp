// src/manifest.cpp
#include "manifest.h"
#include <sstream>

Manifest::Manifest(Env* env, const std::filesystem::path& dir) 
    : env_(env), path_(dir / "MANIFEST") {
    // Open in append mode immediately
    manifest_file_ = env_->NewAppendableFile(path_);
}

void Manifest::add_sstable(int level, const std::string& filename) {
    if (manifest_file_) {
        std::string record = "ADD " + std::to_string(level) + " " + filename + "\n";
        manifest_file_->Append(record);
        manifest_file_->Flush(); // Manifest should generally flush often
    }
}

void Manifest::replace(int level,
                      const std::vector<std::string>& old_files,
                      const std::vector<std::string>& new_files) {
    if (!manifest_file_) return;

    for (const auto& f : old_files) {
        std::string rec = "DEL " + std::to_string(level) + " " + f + "\n";
        manifest_file_->Append(rec);
    }
    for (const auto& f : new_files) {
        std::string rec = "ADD " + std::to_string(level) + " " + f + "\n";
        manifest_file_->Append(rec);
    }
    manifest_file_->Flush();
}

std::vector<Level> Manifest::load() {
    std::vector<Level> levels(7);

    if (!env_->FileExists(path_)) {
        return levels;
    }

    auto reader = env_->NewSequentialFile(path_);
    char scratch[4096];
    std::string buffer;
    std::string leftover;

    while (true) {
        reader->Read(sizeof(scratch), &buffer, scratch);
        if (buffer.empty()) break; 
        
        leftover.append(buffer);
        size_t pos = 0;
        
        while (true) {
            size_t nl = leftover.find('\n', pos);
            if (nl == std::string::npos) {
                leftover.erase(0, pos);
                break;
            }
            std::string line = leftover.substr(pos, nl - pos);
            pos = nl + 1;
            
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string op;
            int lvl;
            std::string file;

            if (!(iss >> op >> lvl)) continue;

            if (op == "ADD" && iss >> file) {
                if (lvl >= 0 && lvl < (int)levels.size()) {
                    levels[lvl].files.push_back(file);
                }
            }
            // Ignore DEL during recovery (assuming simplistic replay)
        }
    }
    return levels;
}