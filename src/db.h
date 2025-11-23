// src/db.h
#pragma once
#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include "manifest.h"
#include "level.h"
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <filesystem>

class ZenithDB {
public:
    explicit ZenithDB(const std::string& dir = "data");
    ~ZenithDB();

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    void remove(const std::string& key);
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start = "", const std::string& end = "\xFF\xFF") const;

private:
    void background_worker();
    void compact_level(int level);
    std::string new_filename(int level);

    std::filesystem::path data_dir_;
    mutable std::mutex mutex_;
    
    MemTable current_memtable_;
    std::vector<std::unique_ptr<MemTable>> immutable_memtables_;

    std::vector<Level> levels_;           // shared type
    Manifest manifest_;
    std::unique_ptr<WAL> wal_;
    std::thread worker_;
    bool stop_ = false;

 static const size_t MEMTABLE_LIMIT = 50 * 1024;  // 50 KB
};