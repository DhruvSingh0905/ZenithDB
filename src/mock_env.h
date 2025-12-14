#pragma once
#include "env.h"
#include <map>
#include <string>
#include <mutex>
#include <iostream>

// Simple in-memory file simulation
class MockEnv : public Env {
    struct MemFile {
        std::string data;
    };
    std::map<std::string, MemFile> file_map_;
    std::mutex mutex_;

public:
    // --- Implement SequentialFile (Reader) ---
    class MockSequentialFile : public SequentialFile {
        std::string data_;
        size_t pos_ = 0;
    public:
        MockSequentialFile(std::string d) : data_(std::move(d)) {}
        void Read(size_t n, std::string* result, char* scratch) override {
            size_t available = data_.size() - pos_;
            size_t to_read = std::min(n, available);
            result->assign(data_.data() + pos_, to_read);
            pos_ += to_read;
        }
        void Skip(size_t n) override { pos_ += n; }
    };

    // --- Implement RandomAccessFile (Reader) ---
    class MockRandomAccessFile : public RandomAccessFile {
        std::string data_;
    public:
        MockRandomAccessFile(std::string d) : data_(std::move(d)) {}
        void Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
            if (offset >= data_.size()) { *result = ""; return; }
            size_t available = data_.size() - offset;
            size_t to_read = std::min(n, available);
            // Must copy to scratch because string_view needs a pointer valid for the caller
            memcpy(scratch, data_.data() + offset, to_read);
            *result = std::string_view(scratch, to_read);
        }
    };

    // --- Implement WritableFile (Writer) ---
    class MockWritableFile : public WritableFile {
        std::string* target_;
    public:
        MockWritableFile(std::string* t) : target_(t) {}
        void Append(const std::string_view& data) override { target_->append(data); }
        void Flush() override {}
        void Sync() override {}
        void Close() override {}
    };

    // --- Env Factory Methods ---
    std::unique_ptr<SequentialFile> NewSequentialFile(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        return std::make_unique<MockSequentialFile>(file_map_[fname.string()].data);
    }

    std::unique_ptr<RandomAccessFile> NewRandomAccessFile(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (file_map_.find(fname.string()) == file_map_.end()) 
            throw std::runtime_error("File not found: " + fname.string());
        return std::make_unique<MockRandomAccessFile>(file_map_[fname.string()].data);
    }

    std::unique_ptr<WritableFile> NewWritableFile(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        file_map_[fname.string()] = MemFile{}; // Create empty
        return std::make_unique<MockWritableFile>(&file_map_[fname.string()].data);
    }

    std::unique_ptr<WritableFile> NewAppendableFile(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        // If exists, append to it; if not, create
        return std::make_unique<MockWritableFile>(&file_map_[fname.string()].data);
    }

    bool FileExists(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        return file_map_.count(fname.string());
    }

    void GetChildren(const std::filesystem::path& dir, std::vector<std::string>* result) override {
        // Naive implementation: just return everything since we have a flat map
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [name, val] : file_map_) {
            result->push_back(name); 
        }
    }

    void CreateDir(const std::filesystem::path& dirname) override {} // No-op for in-memory
    void DeleteFile(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        file_map_.erase(fname.string());
    }
    uint64_t GetFileSize(const std::filesystem::path& fname) override {
        std::lock_guard<std::mutex> lk(mutex_);
        return file_map_[fname.string()].data.size();
    }
};