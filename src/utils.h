#pragma once
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <iterator>

/**
 * Reads an entire file into a string.
 * 
 * @param path File path to read
 * @return File contents as string
 */
inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

/**
 * Splits a string by delimiter.
 * 
 * @param s The string to split
 * @param delim Delimiter character (default: null terminator, splits by whitespace)
 * @return Vector of substrings
 */
inline std::vector<std::string> split(const std::string& s, char delim = '\0') {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty() || delim != '\0')  // Keep empty fields if delim specified
            result.push_back(std::move(item));
    }
    return result;
}