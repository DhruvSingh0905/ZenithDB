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
    ├─► [Writer Mutex Lock]
    │
    ├─► Active MemTable.put(key, value)
    │   └─► Updates std::map
    │   └─► Updates range metadata
    │   └─► Increments size counter
    │
    ├─► WAL.append("PUT|key|value")
    │   └─► Writes to wal.log file
    │
    └─► [Check if memtable too large]
        │
        ├─► [If yes] Freeze memtable
        │   ├─► Create new active memtable
        │   └─► Add frozen memtable to immutable chain (lock-free)
        │
        └─► [Unlock Writer Mutex]
```

### Read Flow

```
User calls db.get(key)
    │
    ├─► [No locks needed - RCU]
    │
    ├─► Check Active MemTable
    │   └─► [Range pruning] Skip if key outside range
    │   └─► [If found] Return value (or nullopt if tombstone)
    │
    ├─► Check Immutable MemTable Chain (lock-free)
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
        │   │   │
        │   │   ├─► [Sparse index] Binary search to find block
        │   │   │
        │   │   └─► [Block scan] Linear search within block
        │   │       └─► [If found] Return value (or nullopt if tombstone)
        │   │
        │   └─► [If found in any file] Return immediately
        │
        └─► [Not found] Return nullopt
```

### Background Worker Flow

```
Background Thread Loop (every 100ms)
    │
    ├─► [Snapshot current layout] (atomic_load)
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
    │   │   │   └─► Write data blocks
    │   │   │   └─► Write bloom filter
    │   │   │   └─► Write sparse index
    │   │   │   └─► Write footer
    │   │   │
    │   │   ├─► Add to Level 0 metadata
    │   │   │
    │   │   ├─► Update manifest
    │   │   │
    │   │   └─► Mark as flushed
    │   │
    │   └─► Update layout snapshot
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
    │   │   │   ├─► Read all SSTables in level
    │   │   │   │
    │   │   │   ├─► Merge key-value pairs
    │   │   │   │   └─► Keep latest value for duplicates
    │   │   │   │   └─► Remove tombstones and deleted keys
    │   │   │   │
    │   │   │   ├─► Write merged SSTable to next level
    │   │   │   │
    │   │   │   ├─► Update manifest (DEL old, ADD new)
    │   │   │   │
    │   │   │   └─► Update layout snapshot
    │   │   │
    │   │   └─► [Break after one compaction]
    │   │
    │   └─► Sort all levels by min_key
    │
    ├─► [Unlock Writer Mutex]
    │
    └─► [Publish new layout] (atomic_store)
```

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

## SSTable Format Details

### File Structure

```
┌─────────────────────────────────────────────────────────┐
│                    Data Blocks                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │ Block 0  │  │ Block 1  │  │ Block 2  │  ...       │
│  └──────────┘  └──────────┘  └──────────┘            │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                  Bloom Filter                           │
│  (10 bits per key, 7 hash functions)                   │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                  Sparse Index                           │
│  [block_count (u32)]                                    │
│  [key_len (u32)][key][offset (u64)] ...                │
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
└─────────────────────────────────────────┘
```

### Lookup Algorithm

```
get(key):
    1. Check bloom filter
       └─► If false, return nullopt (definitely not present)
    
    2. Binary search sparse index
       └─► Find block where min_key <= key < next.min_key
    
    3. Linear search within block
       └─► Since entries are sorted, can stop early if key passed
    
    4. Return value or nullopt
```

## Concurrency Model

### Read-Write Concurrency

- **Writes**: Serialized with `writer_mutex_`
- **Reads**: Fully concurrent, lock-free (RCU)
- **Background**: Runs in separate thread, uses `writer_mutex_`

### Why This Works

1. **Writes are fast**: Only touch memory (memtable + WAL)
2. **Reads don't block writes**: RCU allows concurrent access
3. **Background work is separate**: Doesn't block user operations

### Potential Issues

1. **Write contention**: Single writer mutex limits write throughput
2. **Memory growth**: Old layout snapshots kept until all readers release
3. **Compaction lag**: Background thread may not keep up with writes

## Performance Optimizations

### Range Pruning

Skip memtables and SSTables where `key < min_key || key > max_key`.

### Bloom Filters

Quickly reject SSTables that don't contain a key (no false negatives).

### Block Caching

Cache entire SSTable files in memory to avoid repeated disk reads.

### Sparse Indexing

Binary search on block boundaries instead of scanning entire file.

### Sorted Blocks

Stop early when scanning blocks (keys are sorted).

## Failure Recovery

### WAL Replay

On startup:
1. Open WAL file
2. Read all records
3. Apply PUT/DEL operations to memtable
4. Active memtable now contains all unflushed writes

### Manifest Replay

On startup:
1. Read MANIFEST file
2. For each ADD record, add file to level
3. Reconstruct level structure
4. Open all SSTable files and build layout snapshot

### Corruption Handling

- **WAL corruption**: Partial records are ignored
- **SSTable corruption**: File is skipped with error message
- **Manifest corruption**: Only valid records are processed

## Limitations and Trade-offs

### Current Limitations

1. **Single writer**: Write operations are serialized
2. **No compression**: SSTables stored uncompressed
3. **Simple compaction**: All files in level merged at once
4. **No transactions**: No ACID guarantees across multiple keys
5. **Memory leaks**: ImmNodes not reclaimed (intentional for simplicity)

### Design Trade-offs

1. **Write amplification**: Each write goes to memtable + WAL
2. **Read amplification**: May need to check multiple levels
3. **Space amplification**: Multiple copies during compaction
4. **Latency spikes**: Compaction can cause temporary slowdowns

## Future Enhancements

1. **Multi-threaded writes**: Partition keys across multiple memtables
2. **Compression**: Snappy or Zstd for SSTables
3. **Tiered compaction**: More sophisticated compaction policies
4. **Column families**: Separate namespaces
5. **Transactions**: MVCC or 2PC
6. **Epoch-based reclamation**: Proper cleanup of ImmNodes

