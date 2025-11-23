# ZenithDB Documentation

## Overview

ZenithDB (also known as AbsoluteDB) is a high-performance, embedded key-value database engine implementing the Log-Structured Merge Tree (LSM-tree) architecture. It is designed for write-heavy workloads with excellent read performance through intelligent caching and indexing strategies.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Core Components](#core-components)
3. [Design Decisions](#design-decisions)
4. [Performance Characteristics](#performance-characteristics)
5. [API Reference](#api-reference)
6. [Building and Usage](#building-and-usage)
7. [Benchmarking](#benchmarking)

## Architecture Overview

ZenithDB follows the LSM-tree design pattern, similar to LevelDB and RocksDB. The architecture consists of several key layers:

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
   - Linear search within blocks

### Key Features

- **Lock-free reads** using RCU (Read-Copy-Update) semantics
- **Range pruning** to skip irrelevant memtables and SSTables
- **Bloom filters** for fast negative lookups
- **Block-based storage** for efficient I/O
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

In-memory sorted key-value store using `std::map`.

**Features:**
- Fast O(log n) insertions and lookups
- Range tracking (min_key, max_key) for pruning
- Approximate size tracking for flush decisions
- Tombstone support (empty value = deleted)

**Lifecycle:**
1. Created as active memtable
2. Filled with writes
3. Frozen when size exceeds threshold
4. Added to immutable chain
5. Flushed to disk by background worker
6. Eventually garbage collected

### 3. SSTable (`sstable.h` / `sstable.cpp`)

Immutable on-disk file format for persistent storage.

**File Layout:**
```
[Data Blocks] [Bloom Filter] [Sparse Index] [Footer]
```

**Data Block Format:**
- Header: `[num_entries (u32)][block_size (u32)]`
- Entries: `[key_len (u32)][key][value_len (u32)][value]...`

**Sparse Index:**
- Maps block min_key → block offset
- Enables binary search to find relevant blocks

**Bloom Filter:**
- 10 bits per key, 7 hash functions
- Quick negative test (no false negatives)

**Footer:**
- `[data_end (u64)][bloom_offset (u64)][index_offset (u64)][magic (u64)]`

### 4. WAL (`wal.h` / `wal.cpp`)

Write-Ahead Log for durability.

**Format:**
- Each line: `PUT|key|value` or `DEL|key|`
- Append-only log file: `wal.log`

**Operations:**
- `append()`: Write record to WAL
- `sync()`: Force to disk (fsync)
- `replay()`: Reconstruct memtable from WAL on startup

### 5. Manifest (`manifest.h` / `manifest.cpp`)

Tracks which SSTable files belong to which level.

**Format:**
- Each line: `ADD <level> <filename>` or `DEL <level> <filename>`
- Append-only log file: `MANIFEST`

**Usage:**
- Updated on every flush and compaction
- Replayed on startup to reconstruct level structure

### 6. BlockCache (`block_cache.h`)

Global LRU cache for SSTable file contents.

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
- Drops tombstones and deleted keys

**Benefits:**
- Reduces read amplification
- Reclaims space from deleted keys
- Maintains sorted order

## Design Decisions

### Why LSM-tree?

LSM-trees excel at write-heavy workloads because:
- Writes are sequential (append-only)
- No random disk I/O for writes
- Reads can be optimized with caching and indexing

### Why RCU for Reads?

Read-Copy-Update enables:
- Lock-free reads (no blocking on writes)
- High read concurrency
- Simple implementation with `shared_ptr` and atomic operations

### Why Multi-level Compaction?

- Level 0: Small, recent data (may overlap)
- Level 1+: Larger, sorted, non-overlapping files
- Gradual migration from hot to cold data
- Bounded read amplification

### Why Bloom Filters?

- Fast negative tests (skip files that don't contain key)
- Small memory overhead (~10 bits per key)
- No false negatives (only false positives)

### Why Block-based Storage?

- Efficient I/O (read entire blocks)
- Better cache utilization
- Enables sparse indexing

## Performance Characteristics

### Write Performance

- **Latency:** ~1-10 microseconds (memtable write + WAL)
- **Throughput:** 100K-1M writes/sec (depends on value size)
- **Amplification:** ~1x (write once to memtable, once to WAL)

### Read Performance

- **Point Lookup:**
  - Memtable hit: ~100 nanoseconds
  - SSTable hit: ~1-10 microseconds (with cache)
  - SSTable miss: ~10-100 microseconds (disk I/O)

- **Range Scan:**
  - Depends on range size and data distribution
  - Uses range pruning to skip irrelevant files

### Space Efficiency

- **Overhead:**
  - WAL: ~1x data size (until flushed)
  - SSTable: ~10 bits/key (bloom filter) + index overhead
  - Compaction: Temporary 2x space during merge

### Concurrency

- **Reads:** Fully concurrent (lock-free)
- **Writes:** Serialized (single writer mutex)
- **Background:** Runs in separate thread

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

## Future Improvements

- [ ] Multi-threaded writes
- [ ] Compression (Snappy, Zstd)
- [ ] Column families
- [ ] Transactions
- [ ] Backup/restore
- [ ] Metrics and monitoring
- [ ] Epoch-based reclamation for ImmNodes
- [ ] More sophisticated compaction policies

## License

[Add your license here]

## Contributing

[Add contribution guidelines here]

