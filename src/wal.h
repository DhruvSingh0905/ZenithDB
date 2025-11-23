#pragma once
#include <string>
#include <filesystem>

class MemTable;

class WAL {
public:
    explicit WAL(const std::filesystem::path& dir);
    ~WAL();

    void append(const std::string& record);
    void sync();
    void replay(MemTable* memtable);

private:
    std::filesystem::path path_;
    int fd_ = -1;
};