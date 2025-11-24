# Benchmarking Guide

This document describes how to benchmark ZenithDB, interpret results, and understand the performance characteristics driven by its low-level optimizations.

## Running Benchmarks

### Basic Benchmark

```bash
cd build
./zenithdb_bench
```

### Custom Benchmark

Modify `benchmarks/bench_zenithdb.cpp` to adjust:
- Number of keys
- Value sizes
- Number of threads
- Workload patterns

## Benchmark Scenarios

### 1. Sequential Writes

Measures write throughput for sequential key insertions.

**Metrics:**
- Throughput (writes/sec)
- Average latency (μs/op)
- Total time
- Write amplification

**Typical Results:**
- 100K-1M writes/sec (depends on value size)
- 1-10 μs latency
- ~2x write amplification (memtable + WAL)

**Performance Factors:**
- **Memtable writes**: O(log n) insertion into skip list (arena-allocated)
- **WAL writes**: Sequential append (buffered, fast)
- **Bottleneck**: Mutex serialization (design choice) limits write throughput in high-concurrency scenarios
- **Optimization**: Range metadata updates are O(1)

**Low-Level Optimizations Impact:**
- Arena allocator eliminates memory fragmentation and reduces allocation overhead
- Skip list provides better cache locality than balanced trees
- WAL buffering reduces system call overhead
- Memtable size threshold (2x limit) reduces flush frequency

### 2. Random Reads

Measures read performance for random key lookups.

**Metrics:**
- Throughput (reads/sec)
- Average latency (μs/op)
- P50, P95, P99 latency
- Hit rate (memtable vs SSTable)
- Cache hit rate

**Typical Results:**
- Memtable hit: ~100 nanoseconds
- SSTable hit (memory-mapped): ~1-10 microseconds
- SSTable miss (disk I/O): ~10-100 microseconds
- 10K-100K reads/sec (with cache)

**Performance Factors:**
- **Memtable lookups**: O(log n) with skip list (lock-free reads, arena-allocated)
- **SSTable lookups**: O(log(blocks) + log(n/16) + 16) with bloom filter pruning
- **Memory-mapped files**: Zero-copy reads, OS page caching
- **Range pruning**: Skips irrelevant data structures

**Low-Level Optimizations Impact:**
- **RCU (lock-free reads)**: Enables high read concurrency
- **Bloom filters**: ~99% rejection rate for non-existent keys
- **Sparse indexing**: O(log(blocks)) instead of O(entries)
- **Restart points**: O(log(n/16) + 16) instead of O(n) for block search
- **Memory-mapped files**: Eliminates system call overhead
- **Range pruning**: Reduces unnecessary comparisons

**Latency Breakdown (SSTable hit):**
- Bloom filter check: ~50-100 ns
- Sparse index binary search: ~200-500 ns
- Block binary search (restart points): ~100-300 ns
- Linear scan: ~100-500 ns
- Total: ~1-10 μs (memory-mapped)

### 3. Range Scans

Measures performance of range queries.

**Metrics:**
- Throughput (scans/sec)
- Average latency (μs/scan)
- Average rows per scan
- Scan efficiency (rows scanned / rows returned)

**Typical Results:**
- Depends heavily on range size and data distribution
- 1K-10K scans/sec for small ranges
- Range pruning significantly improves performance

**Performance Factors:**
- **Range pruning**: Skips memtables and SSTables outside range
- **Sorted data**: Enables early termination
- **Memory-mapped files**: Efficient sequential access
- **Merge overhead**: Must merge results from multiple sources

**Low-Level Optimizations Impact:**
- **Range metadata**: Enables efficient pruning at every level
- **Sorted blocks**: Early termination when key passed
- **Memory-mapped files**: Efficient sequential reads
- **Sparse indexing**: Quickly identifies relevant blocks

### 4. Concurrent Write/Read

Measures performance under mixed workload.

**Metrics:**
- Write throughput
- Read throughput per thread
- Total reads completed
- Read latency impact from writes
- Write latency impact from reads

**Typical Results:**
- Reads don't significantly impact writes (RCU)
- Writes may impact reads (compaction)
- Good read scalability (lock-free)

**Performance Factors:**
- **RCU**: Reads don't block writes (lock-free)
- **Serialized writes**: Writes thread-safe but serialized via mutex (supports multiple threads)
- **Background compaction**: May cause temporary slowdowns

**Low-Level Optimizations Impact:**
- **RCU snapshots**: Zero lock contention for reads
- **Copy-on-write**: Readers see consistent state
- **Memory ordering**: Ensures correctness without locks

### 5. Multi-threaded Read-only

Measures read scalability.

**Metrics:**
- Total throughput (reads/sec)
- Average latency
- Scalability (throughput vs threads)
- Cache efficiency

**Typical Results:**
- Good scalability (lock-free reads)
- 100K-1M reads/sec total
- Linear scaling up to CPU cores

**Performance Factors:**
- **RCU**: No lock contention
- **Memory-mapped files**: OS handles page caching
- **CPU cache**: Hot data stays in cache

**Low-Level Optimizations Impact:**
- **Lock-free reads**: Enables perfect read scalability
- **Memory-mapped files**: Efficient shared access
- **Range pruning**: Reduces unnecessary work

## Performance Factors

### Value Size

Larger values:
- Lower write throughput (more data to write)
- Lower read throughput (more data to read)
- Higher space usage
- Higher I/O bandwidth requirements

**Impact on Optimizations:**
- Memory-mapped files handle large values efficiently
- WAL buffering reduces overhead for large writes
- Block size (4KB) optimized for typical value sizes

### Key Distribution

Random keys:
- Better for testing worst-case scenarios
- May cause more compaction
- Bloom filters more effective (better distribution)

Sequential keys:
- Better for range scans
- Less compaction needed
- Range pruning more effective

**Impact on Optimizations:**
- Range pruning less effective with random keys
- Bloom filters work well with any distribution
- Sparse indexing benefits from sorted keys

### Cache Size

Larger cache:
- Better read performance (fewer disk I/O)
- Higher memory usage
- OS page cache handles most caching (memory-mapped files)

**Impact on Optimizations:**
- Memory-mapped files leverage OS page cache automatically
- BlockCache not currently used (deficit)
- Hot data stays in CPU cache (small working set)

### Compaction

Active compaction:
- May cause temporary slowdowns
- Reduces read amplification over time
- Temporary space usage spike (2x during merge)

**Impact on Optimizations:**
- Three-phase compaction minimizes lock time
- Copy-on-write ensures readers not blocked
- Background thread doesn't impact user operations

## Comparison with Other Databases

See the Jupyter notebook `benchmarks/stress_test.ipynb` for detailed comparisons with:
- LevelDB
- RocksDB
- SQLite
- Other embedded databases

### Expected Performance Characteristics

**ZenithDB strengths:**
- Excellent read performance (lock-free, memory-mapped)
- Good write throughput (sequential writes)
- Low read latency (bloom filters, sparse indexing)
- High read concurrency (RCU)

**ZenithDB weaknesses:**
- Mutex serialization limits write throughput (supports multiple threads, but not parallel)
- No compression (higher disk usage)
- Simple compaction (higher write amplification)
- Limited error recovery

## Interpreting Results

### Write Performance

**High throughput**: Good for write-heavy workloads
- Memtable writes are fast (O(log n))
- WAL writes are sequential (buffered)
- Bottleneck: Mutex serialization (supports multiple threads, but not parallel)

**Low latency**: Good for real-time applications
- Typical: 1-10 μs per write
- WAL sync configurable (default: async for lower latency, option: sync for durability)

**Consistent**: Low variance is important
- Compaction can cause latency spikes
- Background thread may lag (deficit: no backpressure)

### Read Performance

**Cache hit rate**: Should be high for hot data
- Memory-mapped files leverage OS page cache
- Hot data stays in CPU cache
- BlockCache not used (deficit)

**Read amplification**: Should be low (ideally < 2x)
- Range pruning reduces unnecessary checks
- Bloom filters reject non-existent keys
- Sparse indexing minimizes block reads

**Latency distribution**: P99 should be reasonable
- Memtable hits: ~100 ns (P50, P95, P99 similar)
- SSTable hits: ~1-10 μs (P50), ~10-50 μs (P95), ~50-200 μs (P99)
- Disk I/O: ~10-100 μs (P50), ~100-500 μs (P95), ~500 μs-5 ms (P99)

**Scalability**: Should scale with threads
- Lock-free reads enable perfect scalability
- Memory-mapped files handle concurrent access well
- CPU cache efficiency important for high throughput

### Space Efficiency

**Space amplification**: Should be low (ideally < 1.5x)
- Bloom filter: ~1.25 bytes per key
- Sparse index: < 1% of file size
- Compaction overhead: Temporary 2x during merge
- No compression (deficit): Higher than compressed databases

**Write amplification**: Should be low (ideally < 2x)
- Memtable + WAL: ~2x
- Compaction: Additional amplification (depends on level structure)
- Simple compaction policy (deficit): Higher than tiered compaction

### Concurrency

**Read scalability**: Should scale with threads
- Lock-free reads (RCU) enable perfect scalability
- Memory-mapped files handle concurrent access
- Typical: Linear scaling up to CPU cores

**Write contention**: Mutex serialization limits throughput
- Design choice: Mutex ensures thread-safe writes with consistent ordering
- Supports multiple write threads, but writes are serialized (not parallel)
- Multi-threaded writes don't scale
- Better for single-threaded or low-concurrency write workloads

## Optimization Tips

1. **Tune memtable size**: Larger = fewer flushes, more memory
   - Current: 100KB (2x limit = 200KB)
   - Deficit: Not configurable

2. **Tune compaction thresholds**: Lower = more frequent, less space
   - Current: Level 0 = 3 files, Level 1+ = 4 files
   - Deficit: Not configurable

3. **Increase cache size**: Better read performance
   - OS page cache handles most caching (memory-mapped files)
   - BlockCache not used (deficit)

4. **Use appropriate value sizes**: Smaller = better performance
   - Block size optimized for 4KB
   - Larger values increase I/O bandwidth

5. **Batch writes**: If possible, batch multiple operations
   - Reduces mutex contention
   - Better for single-threaded workloads

6. **Monitor compaction**: Ensure background thread keeps up
   - Deficit: No backpressure mechanism
   - Level 0 can grow unbounded if compaction lags

## Known Performance Issues

1. **Write amplification**: Each write goes to memtable + WAL (~2x)
   - Acceptable trade-off for durability
   - WAL sync configurable (default: async, option: sync for durability)

2. **Read amplification**: May check multiple levels (~1-3x)
   - Range pruning and bloom filters reduce impact
   - Compaction reduces read amplification over time

3. **Compaction lag**: Background thread may not keep up
   - Deficit: No backpressure mechanism
   - Level 0 can grow unbounded
   - May cause read performance degradation

4. **Serialized writes**: Limits write throughput in high-concurrency scenarios
   - Design choice: Mutex ensures thread-safe writes with consistent ordering
   - Supports multiple write threads, but writes are serialized (not parallel)
   - Multi-threaded writes don't scale
   - Better for single-threaded or low-concurrency workloads

5. **No compression**: Higher disk usage and I/O bandwidth
   - Deficit: SSTables stored uncompressed
   - Can be added later without changing core architecture

6. **Memory usage**: Memory-mapped files consume virtual memory
   - OS handles page caching
   - Many open SSTables can consume significant virtual memory
   - File descriptor management (close after mmap) prevents FD exhaustion

## Reporting Issues

When reporting performance issues, include:
- Benchmark scenario
- System specifications (CPU, RAM, disk)
- Value sizes
- Number of keys
- Number of threads
- Cache size
- Compilation flags (optimization level)
- Full benchmark output
- Profiling data (if available)

## Profiling Tips

To identify performance bottlenecks:

1. **CPU profiling**: Use `perf` or `gprof`
   - Look for hot functions
   - Check for lock contention
   - Identify cache misses

2. **Memory profiling**: Use `valgrind` or `heaptrack`
   - Check for memory leaks
   - Identify allocation hotspots
   - Monitor memory usage

3. **I/O profiling**: Use `iotop` or `strace`
   - Check for excessive disk I/O
   - Identify system call overhead
   - Monitor page cache efficiency

4. **Lock profiling**: Use `perf lock` or `valgrind --tool=helgrind`
   - Check for lock contention
   - Identify deadlocks
   - Monitor mutex wait times

## Benchmarking Best Practices

1. **Warm up**: Run initial writes to populate database
2. **Steady state**: Wait for compaction to stabilize
3. **Multiple runs**: Average results over multiple runs
4. **Isolated system**: Minimize background processes
5. **Consistent environment**: Use same hardware/OS for comparisons
6. **Realistic workloads**: Test with production-like data patterns
7. **Monitor resources**: Track CPU, memory, disk I/O during benchmarks
