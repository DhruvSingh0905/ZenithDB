// src/wal.h
#pragma once
#include "env.h"
#include <string>
#include <filesystem>
#include <memory>

class MemTable;

class WAL {
public:
    // Uses Env for file operations
    WAL(Env* env, const std::filesystem::path& dir);
    
    ~WAL();

    void append(const std::string& record);
    void sync();
    void replay(MemTable* memtable);

private:
    Env* env_;
    std::filesystem::path path_;
    std::unique_ptr<WritableFile> log_file_;
};