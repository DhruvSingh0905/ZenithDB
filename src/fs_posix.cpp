// src/fs_posix.cpp
#include "env.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <vector>
#include <dirent.h>

class PosixSequentialFile : public SequentialFile {
    int fd_;
    std::string filename_;
public:
    PosixSequentialFile(const std::filesystem::path& fname) 
        : filename_(fname.string()) {
        fd_ = open(filename_.c_str(), O_RDONLY);
        if (fd_ < 0) {
            // It's common to fail if file doesn't exist, caller might handle
            // but for now we throw to match previous behavior expectations
            // or let the Env factory handle existence checks.
        }
    }
    ~PosixSequentialFile() override { if(fd_ >= 0) close(fd_); }

    void Read(size_t n, std::string* result, char* scratch) override {
        if (fd_ < 0) {
             result->clear();
             return;
        }
        ssize_t r = read(fd_, scratch, n);
        if (r < 0) throw std::runtime_error("Read failed: " + filename_);
        result->assign(scratch, r);
    }
    
    void Skip(size_t n) override {
        if (fd_ >= 0) {
            if (lseek(fd_, n, SEEK_CUR) == -1) throw std::runtime_error("Skip failed: " + filename_);
        }
    }
    
    bool IsValid() const { return fd_ >= 0; }
};

class PosixRandomAccessFile : public RandomAccessFile {
    int fd_;
    std::string filename_;
public:
    PosixRandomAccessFile(const std::filesystem::path& fname) 
        : filename_(fname.string()) {
        fd_ = open(filename_.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("Failed to open random file: " + filename_);
    }
    ~PosixRandomAccessFile() override { if(fd_ >= 0) close(fd_); }

    void Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
        ssize_t r = pread(fd_, scratch, n, offset);
        if (r < 0) throw std::runtime_error("pread failed: " + filename_);
        *result = std::string_view(scratch, r);
    }
};

class PosixWritableFile : public WritableFile {
    int fd_;
    std::string filename_;
public:
    PosixWritableFile(const std::filesystem::path& fname, bool append) 
        : filename_(fname.string()) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        fd_ = open(filename_.c_str(), flags, 0644);
        if (fd_ < 0) throw std::runtime_error("Failed to open writable file: " + filename_);
    }
    ~PosixWritableFile() override { if(fd_ >= 0) close(fd_); }

    void Append(const std::string_view& data) override {
        size_t left = data.size();
        const char* ptr = data.data();
        while (left > 0) {
            ssize_t written = write(fd_, ptr, left);
            if (written < 0) throw std::runtime_error("Write failed: " + filename_);
            left -= written;
            ptr += written;
        }
    }
    
    void Flush() override { 
        // OS handles buffering usually, strictly fsync is in Sync()
    }
    
    void Sync() override { 
        if (fsync(fd_) < 0) throw std::runtime_error("fsync failed: " + filename_); 
    }
    
    void Close() override { 
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1; 
        }
    }
};

class PosixEnv : public Env {
public:
    std::unique_ptr<SequentialFile> NewSequentialFile(const std::filesystem::path& fname) override {
        return std::make_unique<PosixSequentialFile>(fname);
    }
    
    std::unique_ptr<RandomAccessFile> NewRandomAccessFile(const std::filesystem::path& fname) override {
        return std::make_unique<PosixRandomAccessFile>(fname);
    }
    
    std::unique_ptr<WritableFile> NewWritableFile(const std::filesystem::path& fname) override {
        return std::make_unique<PosixWritableFile>(fname, false);
    }
    
    std::unique_ptr<WritableFile> NewAppendableFile(const std::filesystem::path& fname) override {
        return std::make_unique<PosixWritableFile>(fname, true);
    }
    
    bool FileExists(const std::filesystem::path& fname) override {
        return std::filesystem::exists(fname);
    }
    
    void GetChildren(const std::filesystem::path& dir, std::vector<std::string>* result) override {
        result->clear();
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                result->push_back(entry->d_name);
            }
        }
        closedir(d);
    }
    
    void DeleteFile(const std::filesystem::path& fname) override {
        std::filesystem::remove(fname);
    }
    
    void CreateDir(const std::filesystem::path& dirname) override {
        std::filesystem::create_directories(dirname);
    }
    
    uint64_t GetFileSize(const std::filesystem::path& fname) override {
        struct stat sbuf;
        if (stat(fname.c_str(), &sbuf) != 0) return 0;
        return static_cast<uint64_t>(sbuf.st_size);
    }
};

std::unique_ptr<Env> NewPosixEnv() {
    return std::make_unique<PosixEnv>();
}