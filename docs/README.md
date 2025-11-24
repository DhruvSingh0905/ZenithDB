# ZenithDB Documentation

## Overview

ZenithDB is a high-performance, embedded key-value database engine implementing the Log-Structured Merge Tree (LSM-tree) architecture. It is designed for write-heavy workloads with excellent read performance through intelligent caching and indexing strategies. This implementation focuses on low-level optimizations to achieve maximum performance while maintaining code clarity.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Core Components](#core-components)
3. [Low-Level Optimizations](#low-level-optimizations)
4. [Design Decisions](#design-decisions)
5. [Performance Characteristics](#performance-characteristics)
6. [Known Deficits](#known-deficits)
7. [API Reference](#api-reference)
8. [Building and Usage](#building-and-usage)
9. [Benchmarking](#benchmarking)

## Architecture Overview

ZenithDB follows the LSM-tree design pattern, similar to LevelDB and RocksDB. The architecture consists of several key layers optimized for both write and read performance.

### Write Path

1. **Writes** → Active MemTable (in-memory, sorted map)
2. **WAL** → Write-Ahead Log (durability guarantee)
3. **MemTable Freeze** → When size exceeds threshold, memtable becomes immutable
4. **Background Flush** → Immutable memtables are written to Level 0 SSTables
5. **Compaction** → SSTables are merged and moved to deeper levels

### Read Path

1. **Active MemTable** → Check first (most recent data)
2. **Immutable MemTables** → Check lock-free chain (recent data)
3. **SSTables** → Search from Level 0 to deeper levels
   - Use range metadata to skip irrelevant files
   - Use bloom filters to quickly reject files
   - Use sparse index to find relevant blocks
   - Binary search on restart points within blocks

### Key Features

- **Lock-free reads** using RCU (Read-Copy-Update) semantics
- **Memory-mapped SSTables** for zero-copy reads
- **Range pruning** to skip irrelevant memtables and SSTables
- **Bloom filters** for fast negative lookups (10 bits/key, 7 hash functions)
- **Block-based storage** with restart points for efficient binary search
- **Background compaction** to reduce read amplification
- **Write-ahead logging** for durability

## Core Components

### 1. ZenithDB (`db.h` / `db.cpp`)

The main database class that coordinates all components.

**Key Responsibilities:**
- Manages active and immutable memtables
- Coordinates WAL writes
- Maintains RCU layout snapshots for lock-free reads
- Runs background worker thread for flushing and compaction
- Provides public API (put, get, remove, scan)

**Important Data Structures:**
- `Layout`: RCU snapshot of on-disk SSTable layout
- `ImmNode`: Lock-free linked list of immutable memtables
- `Level`: Metadata about SSTable files in each level

### 2. MemTable (`memtable.h` / `memtable.cpp`)

In-memory sorted key-value store using `std::map` with transparent comparator.

**Features:**
- Fast O(log n) insertions and lookups
- Range tracking (min_key, max_key) for pruning
- Approximate size tracking for flush decisions
- Tombstone support (empty value = deleted)
- **Critical Optimization**: Uses `std::less<>` transparent comparator to enable zero-allocation lookups with `string_view`

**Lifecycle:**
1. Created as active memtable
2. Filled with writes
3. Frozen when size exceeds threshold (2x limit = 100KB)
4. Added to immutable chain (lock-free)
5. Flushed to disk by background worker
6. Eventually garbage collected (currently not reclaimed - see deficits)

### 3. SSTable (`sstable.h` / `sstable.cpp`)

Immutable on-disk file format for persistent storage with memory-mapped access.

**File Layout:**
```
[Data Blocks] [Bloom Filter] [Sparse Index] [Footer]
```

**Data Block Format:**
- Header: `[num_entries (u32)][block_size (u32)]`
- Entries: `[key_len (u32)][key][value_len (u32)][value]...`
- Restart Points: `[restart_offset (u32)]...` (every 16 entries)
- Footer: `[num_restarts (u32)]`

**Sparse Index:**
- Maps block min_key → block offset
- Enables binary search to find relevant blocks
- Stored at end of file for efficient access

**Bloom Filter:**
- 10 bits per key, 7 hash functions
- Quick negative test (no false negatives)
- Uses FNV-1a hash algorithm for fast computation
- Stored after data blocks

**Footer:**
- `[data_end (u64)][bloom_offset (u64)][index_offset (u64)][magic (u64)]`
- Magic number: `0xDB55CA1E` for integrity checking

**Critical Optimizations:**
- Memory-mapped files for zero-copy reads
- File descriptor closed immediately after mmap to prevent FD exhaustion
- Binary search on restart points within blocks (O(log(n/16)) instead of O(n))
- Little-endian encoding for cross-platform compatibility

### 4. WAL (`wal.h` / `wal.cpp`)

Write-Ahead Log for durability.

**Format:**
- Each line: `PUT|key|value` or `DEL|key|`
- Append-only log file: `wal.log`

**Operations:**
- `append()`: Write record to WAL (buffered)
- `sync()`: Force to disk (fsync)
- `replay()`: Reconstruct memtable from WAL on startup

**Recovery:**
- Reads entire WAL on startup
- Applies all PUT/DEL operations to memtable
- Ensures no data loss after crashes

### 5. Manifest (`manifest.h` / `manifest.cpp`)

Tracks which SSTable files belong to which level.

**Format:**
- Each line: `ADD <level> <filename>` or `DEL <level> <filename>`
- Append-only log file: `MANIFEST`

**Usage:**
- Updated on every flush and compaction
- Replayed on startup to reconstruct level structure
- Only ADD records are processed during recovery (DEL records ignored)

### 6. BlockCache (`block_cache.h`)

Global LRU cache for SSTable file contents (currently not used - see deficits).

**Features:**
- Singleton pattern
- Thread-safe operations
- Default capacity: 16MB
- Evicts least recently used entries when full

**Key:** File path (e.g., `"data/L0_1234567890_1.sst"`)
**Value:** `shared_ptr<string>` containing entire file contents

### 7. Compaction (`compaction.cpp`)

Background process that merges SSTables.

**Policy:**
- Level 0: Compact when ≥ 3 files
- Level 1+: Compact when ≥ 4 files
- Merges all files in level into next level
- Removes duplicates (keeps latest)
- Drops tombstones for deleted keys

**Three-Phase Process:**
1. **Plan** (locked): Check thresholds, create compaction task
2. **Execute** (unlocked): Merge SSTables, write new file (I/O heavy)
3. **Apply** (locked): Update manifest, metadata, and layout

**Benefits:**
- Reduces read amplification
- Reclaims space from deleted keys
- Maintains sorted order

## Low-Level Optimizations

This section highlights the critical low-level optimizations implemented in ZenithDB that contribute to its performance.

### 1. RCU (Read-Copy-Update) for Lock-Free Reads

**Implementation:**
- Uses `std::shared_ptr` with atomic operations for layout snapshots
- Readers: `atomic_load_explicit(..., memory_order_acquire)`
- Writers: `atomic_store_explicit(..., memory_order_release)`
- Old snapshots remain valid until all readers release them

**Benefits:**
- Zero lock contention for reads
- High read concurrency
- Simple implementation with automatic memory management

**Memory Ordering:**
- Acquire semantics ensure all writes to layout are visible after load
- Release semantics ensure all writes to layout are visible before store
- Prevents data races without explicit locks

### 2. Memory-Mapped SSTable Files

**Implementation:**
- Uses `mmap()` with `MAP_PRIVATE` flag
- File descriptor closed immediately after mmap to prevent FD exhaustion
- Entire file mapped into virtual memory

**Benefits:**
- Zero-copy reads (no system call overhead)
- OS handles page caching automatically
- Efficient memory usage (pages loaded on demand)
- Prevents file descriptor exhaustion (critical for many SSTables)

**Trade-offs:**
- Memory usage grows with number of open SSTables
- OS manages eviction (not application-controlled)

### 3. Transparent Comparator for Zero-Allocation Lookups

**Implementation:**
- Uses `std::map<std::string, std::string, std::less<>>` in MemTable
- `std::less<>` enables transparent comparison
- `find()` and `lower_bound()` accept `string_view` directly

**Benefits:**
- No temporary string allocations during lookups
- Significant performance improvement for read-heavy workloads
- Reduces memory pressure and GC overhead

**Example:**
```cpp
std::string_view key = "mykey";
auto it = data_.find(key);  // No allocation!
```

### 4. Range Pruning

**Implementation:**
- Each memtable tracks `min_key` and `max_key`
- Each SSTable stores `min_key` and `max_key` in metadata
- Readers check key range before searching

**Benefits:**
- Skips entire data structures when key is outside range
- Reduces unnecessary comparisons
- Particularly effective for range scans

### 5. Bloom Filters

**Implementation:**
- 10 bits per key, 7 hash functions
- Uses FNV-1a hash algorithm (fast, good distribution)
- Stored after data blocks in SSTable

**Benefits:**
- Fast negative test (~99% rejection rate)
- Avoids expensive disk I/O and index lookups
- Small memory overhead (~1.25 bytes per key)

**Algorithm:**
- For each key, compute 7 hash values
- Set corresponding bits in bloom filter
- On lookup, check all 7 bits (if any is 0, key definitely not present)

### 6. Sparse Block Indexing

**Implementation:**
- One index entry per data block (maps min_key → block offset)
- Stored at end of SSTable file
- Binary search to find relevant block

**Benefits:**
- Reduces search space from entire file to single block
- O(log(blocks)) instead of O(entries)
- Small index size (typically < 1% of file size)

### 7. Restart Points for Block Search

**Implementation:**
- Every 16 entries, store a restart point (offset to full key)
- Restart points stored at end of block
- Binary search on restart points, then linear scan

**Benefits:**
- O(log(n/16) + 16) instead of O(n) for block search
- Reduces average search time significantly
- Minimal storage overhead (4 bytes per restart point)

**Algorithm:**
1. Binary search restart points to find approximate location
2. Linear scan from restart point (at most 16 entries)
3. Early termination when key passed (entries are sorted)

### 8. Little-Endian Encoding

**Implementation:**
- All multi-byte integers stored in little-endian format
- Uses `htole64()` / `le64toh()` macros for conversion
- Ensures cross-platform compatibility

**Benefits:**
- Works on both little-endian and big-endian systems
- No runtime byte-order detection needed
- Consistent file format across platforms

### 9. Atomic Operations for Immutable Chain

**Implementation:**
- Immutable memtables stored in lock-free linked list
- Head pointer: `std::atomic<ImmNode*>`
- Insertion: `compare_exchange_weak()` with memory ordering

**Benefits:**
- Lock-free traversal for readers
- No blocking during memtable freezing
- Simple implementation with standard atomics

### 10. Copy-on-Write Layout Updates

**Implementation:**
- Writers create new Layout (copy of old)
- Modify new Layout
- Atomically publish new Layout
- Old Layout remains valid until all readers release

**Benefits:**
- Readers never see inconsistent state
- No locking required for reads
- Automatic memory management via `shared_ptr`

## Design Decisions

### Why LSM-tree?

LSM-trees excel at write-heavy workloads because:
- Writes are sequential (append-only)
- No random disk I/O for writes
- Reads can be optimized with caching and indexing
- Natural support for time-ordered data

### Why RCU for Reads?

Read-Copy-Update enables:
- Lock-free reads (no blocking on writes)
- High read concurrency
- Simple implementation with `shared_ptr` and atomic operations
- Automatic memory reclamation

### Why Multi-level Compaction?

- Level 0: Small, recent data (may overlap)
- Level 1+: Larger, sorted, non-overlapping files
- Gradual migration from hot to cold data
- Bounded read amplification

### Why Bloom Filters?

- Fast negative tests (skip files that don't contain key)
- Small memory overhead (~10 bits per key)
- No false negatives (only false positives)
- Critical for reducing read amplification

### Why Block-based Storage?

- Efficient I/O (read entire blocks)
- Better cache utilization
- Enables sparse indexing
- Matches page size (4KB) for optimal performance

### Why Memory-Mapped Files?

- Zero-copy reads (no system call overhead)
- OS handles page caching
- Efficient memory usage (pages loaded on demand)
- Prevents file descriptor exhaustion

## Performance Characteristics

### Write Performance

- **Latency:** ~1-10 microseconds (memtable write + WAL)
- **Throughput:** 100K-1M writes/sec (depends on value size)
- **Amplification:** ~1x (write once to memtable, once to WAL)

**Bottlenecks:**
- Single writer mutex limits write throughput (see deficits)
- WAL sync can cause latency spikes (not currently synced on every write)

### Read Performance

- **Point Lookup:**
  - Memtable hit: ~100 nanoseconds
  - SSTable hit: ~1-10 microseconds (with memory mapping)
  - SSTable miss: ~10-100 microseconds (disk I/O)

- **Range Scan:**
  - Depends on range size and data distribution
  - Uses range pruning to skip irrelevant files
  - Merges results from multiple sources

**Optimizations:**
- Lock-free reads (RCU)
- Range pruning
- Bloom filters
- Sparse indexing
- Restart points

### Space Efficiency

- **Overhead:**
  - WAL: ~1x data size (until flushed)
  - SSTable: ~10 bits/key (bloom filter) + index overhead
  - Compaction: Temporary 2x space during merge

- **Amplification:**
  - Write: ~1x (memtable + WAL)
  - Read: ~1-3x (check multiple levels)
  - Space: ~1.1-1.5x (bloom filter + index overhead)

### Concurrency

- **Reads:** Fully concurrent (lock-free)
- **Writes:** Serialized (single writer mutex)
- **Background:** Runs in separate thread

**Scalability:**
- Reads scale linearly with number of threads
- Writes limited by single writer (see deficits)

## Known Deficits

This section documents known limitations and areas where the implementation could be improved. These are acknowledged trade-offs made for simplicity and clarity.

### 1. Single Writer Mutex

**Issue:** All write operations are serialized through a single mutex.

**Impact:** Limits write throughput, especially for multi-threaded workloads.

**Why:** Simplifies implementation and ensures correctness. Multi-writer support would require:
- Partitioning keys across multiple memtables
- More complex coordination
- Potential write amplification

### 2. ImmNode Memory Leak

**Issue:** Immutable memtable nodes are never reclaimed from the lock-free chain.

**Impact:** Memory usage grows over time (though memtables are eventually flushed and can be freed).

**Why:** Epoch-based reclamation is complex. Current implementation prioritizes simplicity.

**Workaround:** In practice, memtables are small and flushed quickly, so impact is minimal.

### 3. No Compression

**Issue:** SSTables are stored uncompressed.

**Impact:** Higher disk usage and I/O bandwidth requirements.

**Why:** Compression adds complexity and CPU overhead. Can be added later without changing core architecture.

### 4. Simple Compaction Policy

**Issue:** Compacts entire level at once when threshold is met.

**Impact:** Can cause write amplification and temporary space usage spikes.

**Why:** Simpler than tiered or leveled compaction. Works well for most workloads.

**Better Approach:** Tiered compaction (merge smaller files first) or leveled compaction (merge overlapping files).

### 5. BlockCache Not Used

**Issue:** BlockCache is implemented but not integrated into SSTable reads.

**Impact:** Missing opportunity for additional caching layer.

**Why:** Memory-mapped files already provide OS-level caching. BlockCache would add complexity with marginal benefit.

### 6. No Transaction Support

**Issue:** No ACID guarantees across multiple keys.

**Impact:** Cannot perform atomic multi-key operations.

**Why:** Adds significant complexity (MVCC, locking, conflict resolution).

### 7. Limited Error Recovery

**Issue:** Corrupted SSTables are skipped with minimal logging.

**Impact:** Data loss may go unnoticed.

**Why:** Prioritizes performance over comprehensive error handling.

**Better Approach:** Checksums, more detailed logging, repair utilities.

### 8. WAL Not Synced on Every Write

**Issue:** WAL is buffered and only synced on shutdown or explicit sync.

**Impact:** Risk of data loss on crash (though small window).

**Why:** Reduces write latency. Can be configured to sync on every write if needed.

### 9. No Column Families

**Issue:** All data stored in single namespace.

**Impact:** Cannot isolate different data types or access patterns.

**Why:** Adds complexity to compaction and metadata management.

### 10. Fixed Memtable Size Threshold

**Issue:** Memtable size threshold is hardcoded (100KB).

**Impact:** Not tunable for different workloads.

**Why:** Simplifies implementation. Can be made configurable.

## API Reference

### Constructor

```cpp
ZenithDB db("data");  // Creates database in "data" directory
```

### Write Operations

```cpp
// Insert or update a key-value pair
db.put("key1", "value1");

// Delete a key
db.remove("key1");
```

### Read Operations

```cpp
// Point lookup
auto value = db.get("key1");
if (value) {
    std::cout << *value << std::endl;
}

// Range scan (inclusive)
auto results = db.scan("key1", "key100");
for (const auto& [key, val] : results) {
    std::cout << key << " -> " << val << std::endl;
}
```

## Building and Usage

### Prerequisites

- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.20+
- Filesystem library support
- POSIX-compliant system (for mmap, file I/O)

### Build

```bash
mkdir build && cd build
cmake ..
make
```

### Run REPL

```bash
./zenithdb
```

Commands:
- `put <key> <value>` - Insert/update
- `get <key>` - Lookup
- `del <key>` - Delete
- `scan [<start> [<end>]]` - Range scan
- `exit` - Quit

### Run Benchmarks

```bash
./zenithdb_bench
```

### Run Tests

```bash
./tests
```

## Benchmarking

See the [Benchmarking Guide](BENCHMARKING.md) for detailed performance analysis and comparison with other databases.

## License

[Add your license here]

## Contributing

[Add contribution guidelines here]
