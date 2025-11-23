// src/manifest.h
#pragma once
#include "level.h"
#include <string>
#include <vector>
#include <filesystem>

/**
 * Manifest - Tracks the set of SSTable files in each level.
 * 
 * The manifest is an append-only log file that records all changes
 * to the SSTable layout (additions and deletions). It's used during
 * recovery to reconstruct which files belong to which level.
 * 
 * Format: Each line is "ADD <level> <filename>" or "DEL <level> <filename>"
 */
class Manifest {
public:
    /**
     * Opens or creates a manifest file in the given directory.
     * 
     * The manifest file is named "MANIFEST".
     * 
     * @param dir Directory where the manifest file should be stored
     */
    explicit Manifest(const std::filesystem::path& dir);

    /**
     * Records that a new SSTable was added to a level.
     * 
     * Appends an "ADD" record to the manifest log.
     * 
     * @param level The level number (0-based)
     * @param filename The SSTable filename
     */
    void add_sstable(int level, const std::string& filename);
    
    /**
     * Records that SSTables were replaced in a level.
     * 
     * Appends "DEL" records for old files and "ADD" records for new files.
     * Used during compaction when multiple files are merged into one.
     * 
     * @param level The level number
     * @param old_files Files being removed
     * @param new_files Files being added
     */
    void replace(int level,
                 const std::vector<std::string>& old_files,
                 const std::vector<std::string>& new_files);

    /**
     * Loads the manifest and reconstructs the level structure.
     * 
     * Reads all records from the manifest and builds a vector of Level
     * structures, one per level, containing the list of SSTable files.
     * 
     * @return Vector of Level structures, one per level
     */
    std::vector<Level> load();

private:
    std::filesystem::path path_;
};