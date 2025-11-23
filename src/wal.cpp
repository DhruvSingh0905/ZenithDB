// src/wal.cpp
#include "wal.h"
#include "memtable.h"
#include "utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <cerrno>

WAL::WAL(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    path_ = dir / "wal.log";

    fd_ = open(path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if (fd_ == -1) {
        throw std::runtime_error("Cannot open WAL: " + path_.string() + " " + std::strerror(errno));
    }
}

WAL::~WAL() {
    if (fd_ != -1) {
        sync();
        close(fd_);
    }
}

void WAL::append(const std::string& record) {
    std::string line = record + "\n";
    ssize_t written = write(fd_, line.data(), line.size());
    if (written != static_cast<ssize_t>(line.size())) {
        throw std::runtime_error("WAL write failed");
    }
}

void WAL::sync() {
    if (fd_ != -1) {
        ::fsync(fd_);          // :: to avoid any name clash
    }
}

void WAL::replay(MemTable* memtable) {
    if (lseek(fd_, 0, SEEK_SET) == -1) {
        return;  // empty or error
    }

    char buffer[8192];
    std::string leftover;

    while (true) {
        ssize_t bytes = read(fd_, buffer, sizeof(buffer));
        if (bytes <= 0) break;                     // EOF or error
        leftover.append(buffer, bytes);

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

    // If there's a partial line left without newline, ignore it (corrupted)
}