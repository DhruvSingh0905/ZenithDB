// src/level.h
#pragma once
#include <string>
#include <vector>

/**
 * Level - Metadata about a level in the LSM-tree.
 * 
 * Tracks which SSTable files belong to this level and the compaction
 * threshold (max_files). Level 0 has a different threshold (3 files)
 * handled in compaction logic.
 */
struct Level {
    std::vector<std::string> files;  // List of SSTable filenames in this level
    size_t max_files = 4;            // Compaction threshold (default for L1+)
};