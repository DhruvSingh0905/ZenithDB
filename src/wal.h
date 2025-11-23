#pragma once
#include <string>
#include <filesystem>

class MemTable;

/**
 * WAL - Write-Ahead Log for durability.
 * 
 * The WAL ensures that all writes are persisted to disk before being
 * acknowledged. This provides durability guarantees: if the process
 * crashes, the WAL can be replayed to recover the active memtable.
 * 
 * Format: Each line is "PUT|key|value" or "DEL|key|"
 */
class WAL {
public:
    /**
     * Opens or creates a WAL file in the given directory.
     * 
     * The WAL file is named "wal.log" and is opened in append mode.
     * 
     * @param dir Directory where the WAL file should be stored
     * @throws std::runtime_error if the file cannot be opened
     */
    explicit WAL(const std::filesystem::path& dir);
    
    /**
     * Destructor that syncs the WAL to disk before closing.
     */
    ~WAL();

    /**
     * Appends a record to the WAL.
     * 
     * The record is written immediately but may be buffered by the OS.
     * Call sync() to ensure it's persisted to disk.
     * 
     * @param record The log record (format: "PUT|key|value" or "DEL|key|")
     * @throws std::runtime_error if the write fails
     */
    void append(const std::string& record);
    
    /**
     * Forces all buffered WAL data to disk.
     * 
     * Uses fsync() to ensure durability. Should be called after
     * critical writes or before shutdown.
     */
    void sync();
    
    /**
     * Replays the WAL to reconstruct a memtable.
     * 
     * Reads all records from the WAL and applies them to the given
     * memtable. Used during database recovery on startup.
     * 
     * @param memtable The memtable to populate with replayed operations
     */
    void replay(MemTable* memtable);

private:
    std::filesystem::path path_;
    int fd_ = -1;
};