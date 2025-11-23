#pragma once
#include <string>
#include <fstream>
#include <vector>
#include <sstream>          // ← this was missing
#include <iterator>         // for istreambuf_iterator

inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

inline std::vector<std::string> split(const std::string& s, char delim = '\0') {
    std::vector<std::string> result;
    std::stringstream ss(s);          // now compiles
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty() || delim != '\0')  // keep empty fields if delim specified
            result.push_back(std::move(item));
    }
    return result;
}