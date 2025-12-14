// src/env.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <cstdint>
#include <string_view>

class SequentialFile {
public:
    virtual ~SequentialFile() = default;
    virtual void Read(size_t n, std::string* result, char* scratch) = 0;
    virtual void Skip(size_t n) = 0;
};

class RandomAccessFile {
public:
    virtual ~RandomAccessFile() = default;
    virtual void Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const = 0;
};

class WritableFile {
public:
    virtual ~WritableFile() = default;
    virtual void Append(const std::string_view& data) = 0;
    virtual void Flush() = 0;
    virtual void Sync() = 0;
    virtual void Close() = 0;
};

class Env {
public:
    virtual ~Env() = default;
    virtual std::unique_ptr<SequentialFile> NewSequentialFile(const std::filesystem::path& fname) = 0;
    virtual std::unique_ptr<RandomAccessFile> NewRandomAccessFile(const std::filesystem::path& fname) = 0;
    virtual std::unique_ptr<WritableFile> NewWritableFile(const std::filesystem::path& fname) = 0;
    virtual std::unique_ptr<WritableFile> NewAppendableFile(const std::filesystem::path& fname) = 0;
    virtual bool FileExists(const std::filesystem::path& fname) = 0;
    virtual void GetChildren(const std::filesystem::path& dir, std::vector<std::string>* result) = 0;
    virtual void DeleteFile(const std::filesystem::path& fname) = 0;
    virtual void CreateDir(const std::filesystem::path& dirname) = 0;
    virtual uint64_t GetFileSize(const std::filesystem::path& fname) = 0;
};

// Factory function implemented in fs_posix.cpp
std::unique_ptr<Env> NewPosixEnv();