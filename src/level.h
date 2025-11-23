// src/level.h
#pragma once
#include <string>
#include <vector>

struct Level {
    std::vector<std::string> files;
    size_t max_files = 4;        // default for L1+
};