// src/wal.cpp
#include "wal.h"
#include "memtable.h"
#include "utils.h"

#include <stdexcept>
#include <vector>

WAL::WAL(Env* env, const std::filesystem::path& dir) 
    : env_(env) {
    env_->CreateDir(dir);
    path_ = dir / "wal.log";
    log_file_ = env_->NewAppendableFile(path_);
}

WAL::~WAL() {
    if (log_file_) {
        sync();
        log_file_->Close();
    }
}

void WAL::append(const std::string& record) {
    std::string line = record + "\n";
    log_file_->Append(line);
}

void WAL::sync() {
    if (log_file_) {
        log_file_->Sync();
    }
}

void WAL::replay(MemTable* memtable) {
    if (!env_->FileExists(path_)) {
        return; 
    }

    auto reader = env_->NewSequentialFile(path_);
    char scratch[8192];
    std::string buffer;
    std::string leftover;

    while (true) {
        reader->Read(sizeof(scratch), &buffer, scratch);
        if (buffer.empty()) break; // EOF
        
        leftover.append(buffer);

        size_t pos = 0;
        while (true) {
            size_t nl = leftover.find('\n', pos);
            if (nl == std::string::npos) {
                // keep the partial line for next iteration
                leftover.erase(0, pos);
                break;
            }

            std::string line = leftover.substr(pos, nl - pos);
            pos = nl + 1;

            if (line.empty()) continue;

            auto parts = split(line, '|');
            if (parts.size() >= 2) {
                if (parts[0] == "PUT" && parts.size() == 3) {
                    memtable->put(parts[1], parts[2]);
                } else if (parts[0] == "DEL" && parts.size() == 2) {
                    memtable->remove(parts[1]);
                }
            }
        }
    }
}