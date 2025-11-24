# ZenithDB Architecture Deep Dive

## LSM-Tree Fundamentals

ZenithDB implements a Log-Structured Merge Tree (LSM-tree), a data structure optimized for write-heavy workloads. The key insight is to separate fast in-memory writes from slower persistent storage.

### The Problem LSM-trees Solve

Traditional B-trees require random disk I/O for both reads and writes. LSM-trees optimize for writes by:
1. Batching writes in memory
2. Writing sequentially to disk
3. Merging data in the background

### LSM-tree Structure

```
┌─────────────────┐
│  Active MemTable│ ← Writes go here (in-memory)
└────────┬────────┘
         │
         ▼ (when full)
┌─────────────────┐
│ Immutable Chain │ ← Lock-free reads
└────────┬────────┘
         │
         ▼ (background flush)
┌─────────────────┐
│   Level 0       │ ← Small, recent SSTables (may overlap)
└────────┬────────┘
         │
         ▼ (compaction)
┌─────────────────┐
│   Level 1       │ ← Larger, sorted SSTables
└────────┬────────┘
         │
         ▼ (compaction)
┌─────────────────┐
│   Level 2+      │ ← Even larger, sorted SSTables
└─────────────────┘
```

## Component Interactions

### Write Flow

```
User calls db.put(key, value)
    │
    ├─► [Writer Mutex Lock] ← Serializes writes (deficit: single writer)
    │
    ├─► Active MemTable.put(key, value)
    │   └─► Updates std::map<std::string, std::string, std::less<>>
    │   └─► Updates range metadata (min_key, max_key)
    │   └─► Increments size counter
    │
    ├─► WAL.append("PUT|key|value")
    │   └─► Writes to wal.log file (buffered, not synced)
    │
    └─► [Check if memtable too large]
        │
        ├─► [If yes] Freeze memtable
        │   ├─► Create new active memtable
        │   └─► Add frozen memtable to immutable chain (lock-free CAS)
        │
        └─► [Unlock Writer Mutex]
```

**Key Optimizations:**
- `std::less<>` transparent comparator enables zero-allocation lookups
- Range metadata enables efficient pruning
- Lock-free immutable chain insertion (no blocking)

### Read Flow

```
User calls db.get(key)
    │
    ├─► [No locks needed - RCU] ← Lock-free read path
    │
    ├─► Check Active MemTable
    │   └─► [Range pruning] Skip if key outside [min_key, max_key]
    │   └─► [If found] Return value (or nullopt if tombstone)
    │
    ├─► Check Immutable MemTable Chain (lock-free traversal)
    │   └─► Traverse from head (most recent) to tail
    │   └─► [Range pruning] Skip if key outside range
    │   └─► [If found] Return value
    │
    └─► Check SSTables (RCU snapshot)
        │
        ├─► For each level (0 to N):
        │   │
        │   ├─► [Range pruning] Skip files where key outside range
        │   │
        │   ├─► For each file in level:
        │   │   │
        │   │   ├─► [Bloom filter] Skip if key definitely not present
        │   │   │   └─► 7 hash functions, check bits (fast negative test)
        │   │   │
        │   │   ├─► [Sparse index] Binary search to find block
        │   │   │   └─► O(log(blocks)) instead of O(entries)
        │   │   │
        │   │   └─► [Block scan] Binary search restart points, then linear scan
        │   │       └─► O(log(n/16) + 16) instead of O(n)
        │   │       └─► [If found] Return value (or nullopt if tombstone)
        │   │
        │   └─► [If found in any file] Return immediately
        │
        └─► [Not found] Return nullopt
```

**Key Optimizations:**
- RCU snapshots (no locking)
- Range pruning at every level
- Bloom filter negative test
- Sparse index binary search
- Restart points for efficient block search
- Memory-mapped files (zero-copy reads)

### Background Worker Flow

```
Background Thread Loop (every 100ms)
    │
    ├─► [Snapshot current layout] (atomic_load with acquire)
    │
    ├─► [Writer Mutex Lock]
    │
    ├─► Flush Immutable Memtables
    │   │
    │   ├─► For each immutable memtable:
    │   │   │
    │   │   ├─► [If not flushed] Get sorted entries
    │   │   │
    │   │   ├─► Create new SSTable file
    │   │   │   └─► Write data blocks (4KB target, restart points every 16 entries)
    │   │   │   └─► Write bloom filter (10 bits/key, 7 hash functions)
    │   │   │   └─► Write sparse index (block min_key → offset)
    │   │   │   └─► Write footer (offsets + magic number)
    │   │   │
    │   │   ├─► Add to Level 0 metadata
    │   │   │
    │   │   ├─► Update manifest (append "ADD" record)
    │   │   │
    │   │   ├─► Memory-map new SSTable (zero-copy reads)
    │   │   │   └─► Close FD immediately after mmap (prevent FD exhaustion)
    │   │   │
    │   │   └─► Mark as flushed
    │   │
    │   └─► Update layout snapshot (copy-on-write)
    │       └─► Sort Level 0 by min_key (for efficient binary search)
    │       └─► Publish new layout (atomic_store with release)
    │
    ├─► Compaction
    │   │
    │   ├─► For each level (0 to N-1):
    │   │   │
    │   │   ├─► [Check threshold]
    │   │   │   └─► Level 0: ≥ 3 files
    │   │   │   └─► Level 1+: ≥ 4 files
    │   │   │
    │   │   ├─► [If threshold exceeded]
    │   │   │   │
    │   │   │   ├─► [Phase 1: Plan] Create compaction task
    │   │   │   │   └─► Snapshot filenames and SSTable objects
    │   │   │   │
    │   │   │   ├─► [Phase 2: Execute] (unlocked - I/O heavy)
    │   │   │   │   ├─► Read all SSTables in level
    │   │   │   │   ├─► Merge key-value pairs (keep latest for duplicates)
    │   │   │   │   ├─► Remove tombstones and deleted keys
    │   │   │   │   └─► Write merged SSTable to next level
    │   │   │   │
    │   │   │   └─► [Phase 3: Apply] (locked)
    │   │   │       ├─► Update manifest (DEL old, ADD new)
    │   │   │       ├─► Update levels_meta_
    │   │   │       └─► Update layout snapshot
    │   │   │
    │   │   └─► [Break after one compaction] (deficit: could compact multiple levels)
    │   │
    │   └─► Sort all levels by min_key
    │
    ├─► [Unlock Writer Mutex]
    │
    └─► [Publish new layout] (atomic_store with release)
```

**Key Optimizations:**
- Three-phase compaction (plan/execute/apply) minimizes lock time
- Copy-on-write layout updates (RCU)
- Memory-mapped SSTables (zero-copy)
- File descriptor management (close after mmap)

## RCU (Read-Copy-Update) Implementation

RCU enables lock-free reads by using immutable snapshots.

### How It Works

1. **Readers** load a `shared_ptr` to the layout snapshot (atomic_load with acquire)
2. **Writers** create a new layout, modify it, then publish it (atomic_store with release)
3. **Old snapshots** remain valid until all readers release them
4. **Memory** is reclaimed when last reader releases (via `shared_ptr` reference counting)

### Memory Ordering

- **Acquire** (readers): Ensures all writes to the layout are visible after the load
- **Release** (writers): Ensures all writes to the layout are visible before the store

This creates a happens-before relationship without explicit locks.

### Example

```cpp
// Reader (lock-free)
auto snapshot = atomic_load_ptr(&layout_);  // acquire
// ... use snapshot ...
// snapshot goes out of scope, reference count decrements

// Writer (locked)
auto new_layout = std::make_shared<Layout>(*old_layout);
// ... modify new_layout ...
atomic_store_ptr(&layout_, new_layout);  // release
// old_layout reference count decrements when last reader releases
```

### Benefits

- Zero lock contention for reads
- High read concurrency
- Simple implementation
- Automatic memory management

### Trade-offs

- Memory usage grows with number of concurrent readers (old snapshots kept alive)
- Copy-on-write overhead (but minimal since Layout is small)

## SSTable Format Details

### File Structure

```
┌─────────────────────────────────────────────────────────┐
│                    Data Blocks                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │ Block 0  │  │ Block 1  │  │ Block 2  │  ...       │
│  └──────────┘  └──────────┘  └──────────┘            │
│  (4KB target, restart points every 16 entries)        │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                  Bloom Filter                           │
│  (10 bits per key, 7 hash functions, FNV-1a hash)      │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                  Sparse Index                           │
│  [block_count (u32)]                                    │
│  [key_len (u32)][key][offset (u64)] ...                │
│  (one entry per block, maps min_key → block offset)    │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                    Footer                               │
│  [data_end (u64)]                                       │
│  [bloom_offset (u64)]                                   │
│  [index_offset (u64)]                                   │
│  [magic (u64) = 0xDB55CA1E]                            │
└─────────────────────────────────────────────────────────┘
```

### Block Format

```
┌─────────────────────────────────────────┐
│  Header: [num_entries (u32)][size (u32)]│
├─────────────────────────────────────────┤
│  Entry 0: [klen (u32)][key][vlen (u32)][value]│
│  Entry 1: [klen (u32)][key][vlen (u32)][value]│
│  ...                                     │
│  (entries are sorted by key)             │
├─────────────────────────────────────────┤
│  Restart Points: [offset (u32)]...      │
│  (every 16 entries, relative to block start)│
├─────────────────────────────────────────┤
│  Footer: [num_restarts (u32)]           │
└─────────────────────────────────────────┘
```

### Lookup Algorithm

```
get(key):
    1. Check bloom filter
       └─► Compute 7 hash values
       └─► Check corresponding bits
       └─► If any bit is 0, return nullopt (definitely not present)
    
    2. Binary search sparse index
       └─► Find block where min_key <= key < next.min_key
       └─► O(log(blocks)) complexity
    
    3. Binary search restart points in block
       └─► Find approximate location
       └─► O(log(n/16)) complexity
    
    4. Linear scan from restart point
       └─► At most 16 entries to check
       └─► Early termination when key passed (entries are sorted)
       └─► O(16) worst case
    
    5. Return value or nullopt
```

**Total Complexity:** O(log(blocks) + log(n/16) + 16) ≈ O(log(n))

### Memory-Mapped Access

SSTables are memory-mapped for zero-copy reads:

```cpp
// Open file
int fd = open(path, O_RDONLY);

// Memory-map entire file
void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

// CRITICAL: Close FD immediately after mmap
close(fd);  // Prevents FD exhaustion

// Access data directly (OS handles page caching)
const char* data = static_cast<const char*>(map);
```

**Benefits:**
- Zero-copy reads (no system call overhead)
- OS handles page caching
- Efficient memory usage (pages loaded on demand)
- Prevents file descriptor exhaustion

## Concurrency Model

### Read-Write Concurrency

- **Writes**: Serialized with `writer_mutex_` (deficit: single writer)
- **Reads**: Fully concurrent, lock-free (RCU)
- **Background**: Runs in separate thread, uses `writer_mutex_`

### Why This Works

1. **Writes are fast**: Only touch memory (memtable + WAL)
2. **Reads don't block writes**: RCU allows concurrent access
3. **Background work is separate**: Doesn't block user operations

### Potential Issues

1. **Write contention**: Single writer mutex limits write throughput (deficit)
2. **Memory growth**: Old layout snapshots kept until all readers release (minor)
3. **Compaction lag**: Background thread may not keep up with writes (deficit: no backpressure)

## Performance Optimizations

### Range Pruning

Skip memtables and SSTables where `key < min_key || key > max_key`.

**Implementation:**
- Each memtable tracks min_key and max_key
- Each SSTable stores min_key and max_key in metadata
- Readers check range before searching

**Impact:** Reduces unnecessary comparisons, especially for range scans.

### Bloom Filters

Quickly reject SSTables that don't contain a key (no false negatives).

**Implementation:**
- 10 bits per key, 7 hash functions
- FNV-1a hash algorithm (fast, good distribution)
- Stored after data blocks

**Impact:** ~99% rejection rate for non-existent keys, avoiding expensive I/O.

### Memory-Mapped Files

Zero-copy reads with OS-managed page caching.

**Implementation:**
- `mmap()` with `MAP_PRIVATE`
- FD closed immediately after mmap
- Direct pointer access to file data

**Impact:** Eliminates system call overhead, leverages OS page cache.

### Sparse Indexing

Binary search on block boundaries instead of scanning entire file.

**Implementation:**
- One index entry per block (maps min_key → offset)
- Stored at end of file
- Binary search to find relevant block

**Impact:** O(log(blocks)) instead of O(entries).

### Restart Points

Binary search on restart points, then linear scan.

**Implementation:**
- Every 16 entries, store restart point (offset to full key)
- Binary search restart points to find approximate location
- Linear scan from restart point (at most 16 entries)

**Impact:** O(log(n/16) + 16) instead of O(n) for block search.

### Transparent Comparator

Zero-allocation lookups with `string_view`.

**Implementation:**
- `std::map<std::string, std::string, std::less<>>`
- `std::less<>` enables transparent comparison
- `find()` and `lower_bound()` accept `string_view` directly

**Impact:** Eliminates temporary string allocations during lookups.

## Failure Recovery

### WAL Replay

On startup:
1. Open WAL file
2. Read all records (buffered read with partial line handling)
3. Apply PUT/DEL operations to memtable
4. Active memtable now contains all unflushed writes

**Deficit:** No checksums or corruption detection in WAL.

### Manifest Replay

On startup:
1. Read MANIFEST file
2. For each ADD record, add file to level
3. Reconstruct level structure
4. Open all SSTable files and build layout snapshot

**Deficit:** Corrupted SSTables are skipped with minimal logging.

### Corruption Handling

- **WAL corruption**: Partial records are ignored (deficit: should validate checksums)
- **SSTable corruption**: File is skipped with error message (deficit: should attempt repair)
- **Manifest corruption**: Only valid records are processed (deficit: should validate format)

## Limitations and Trade-offs

### Current Limitations

1. **Single writer**: Write operations are serialized (deficit)
2. **No compression**: SSTables stored uncompressed (deficit)
3. **Simple compaction**: All files in level merged at once (deficit)
4. **No transactions**: No ACID guarantees across multiple keys (deficit)
5. **Memory leaks**: ImmNodes not reclaimed (deficit: intentional for simplicity)
6. **No backpressure**: Compaction may lag behind writes (deficit)
7. **Limited error recovery**: Corrupted files skipped (deficit)
8. **WAL not synced**: Risk of data loss on crash (deficit)
9. **Fixed thresholds**: Not tunable (deficit)
10. **No column families**: Single namespace (deficit)

### Design Trade-offs

1. **Write amplification**: Each write goes to memtable + WAL (~2x)
2. **Read amplification**: May need to check multiple levels (~1-3x)
3. **Space amplification**: Multiple copies during compaction (~1.1-1.5x)
4. **Latency spikes**: Compaction can cause temporary slowdowns
5. **Memory usage**: Memory-mapped files consume virtual memory
6. **Complexity vs Performance**: Chose simplicity over advanced features

## Known Deficits

### 1. Single Writer Mutex

**Issue:** All write operations serialized through single mutex.

**Impact:** Limits write throughput, especially for multi-threaded workloads.

**Why:** Simplifies implementation and ensures correctness.

**Better Approach:** Partition keys across multiple memtables, use per-partition locks.

### 2. ImmNode Memory Leak

**Issue:** Immutable memtable nodes never reclaimed from lock-free chain.

**Impact:** Memory usage grows over time (though minimal in practice).

**Why:** Epoch-based reclamation is complex.

**Better Approach:** Implement epoch-based reclamation or hazard pointers.

### 3. No Compression

**Issue:** SSTables stored uncompressed.

**Impact:** Higher disk usage and I/O bandwidth.

**Why:** Compression adds complexity and CPU overhead.

**Better Approach:** Add Snappy or Zstd compression with configurable levels.

### 4. Simple Compaction Policy

**Issue:** Compacts entire level at once.

**Impact:** Write amplification and space spikes.

**Why:** Simpler than tiered/leveled compaction.

**Better Approach:** Implement tiered compaction (merge smaller files first).

### 5. BlockCache Not Used

**Issue:** BlockCache implemented but not integrated.

**Impact:** Missing additional caching layer.

**Why:** Memory-mapped files already provide OS-level caching.

**Better Approach:** Use BlockCache for frequently accessed blocks.

### 6. No Transaction Support

**Issue:** No ACID guarantees across multiple keys.

**Impact:** Cannot perform atomic multi-key operations.

**Why:** Adds significant complexity.

**Better Approach:** Implement MVCC or 2PC for transactions.

### 7. Limited Error Recovery

**Issue:** Corrupted SSTables skipped with minimal logging.

**Impact:** Data loss may go unnoticed.

**Why:** Prioritizes performance over comprehensive error handling.

**Better Approach:** Add checksums, detailed logging, repair utilities.

### 8. WAL Not Synced

**Issue:** WAL buffered, only synced on shutdown.

**Impact:** Risk of data loss on crash.

**Why:** Reduces write latency.

**Better Approach:** Add configurable sync policy (every write, every N writes, etc.).

### 9. No Backpressure

**Issue:** Compaction may lag behind writes.

**Impact:** Level 0 can grow unbounded.

**Why:** No mechanism to throttle writes.

**Better Approach:** Add backpressure mechanism (pause writes when Level 0 too large).

### 10. Fixed Thresholds

**Issue:** Memtable size and compaction thresholds hardcoded.

**Impact:** Not tunable for different workloads.

**Why:** Simplifies implementation.

**Better Approach:** Make thresholds configurable via constructor or config file.
