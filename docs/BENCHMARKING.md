# Benchmarking Guide

This document describes how to benchmark ZenithDB and interpret the results.

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

**Typical Results:**
- 100K-1M writes/sec (depends on value size)
- 1-10 μs latency

### 2. Random Reads

Measures read performance for random key lookups.

**Metrics:**
- Throughput (reads/sec)
- Average latency (μs/op)
- Hit rate

**Typical Results:**
- 10K-100K reads/sec (with cache)
- 10-100 μs latency

### 3. Range Scans

Measures performance of range queries.

**Metrics:**
- Throughput (scans/sec)
- Average latency (μs/scan)
- Average rows per scan

**Typical Results:**
- Depends heavily on range size
- 1K-10K scans/sec for small ranges

### 4. Concurrent Write/Read

Measures performance under mixed workload.

**Metrics:**
- Write throughput
- Read throughput per thread
- Total reads completed

**Typical Results:**
- Reads don't significantly impact writes (RCU)
- Writes may impact reads (compaction)

### 5. Multi-threaded Read-only

Measures read scalability.

**Metrics:**
- Total throughput (reads/sec)
- Average latency
- Scalability (throughput vs threads)

**Typical Results:**
- Good scalability (lock-free reads)
- 100K-1M reads/sec total

## Performance Factors

### Value Size

Larger values:
- Lower write throughput (more data to write)
- Lower read throughput (more data to read)
- Higher space usage

### Key Distribution

Random keys:
- Better for testing worst-case scenarios
- May cause more compaction

Sequential keys:
- Better for range scans
- Less compaction needed

### Cache Size

Larger cache:
- Better read performance (fewer disk I/O)
- Higher memory usage

### Compaction

Active compaction:
- May cause temporary slowdowns
- Reduces read amplification over time

## Comparison with Other Databases

See the Jupyter notebook `benchmarks/stress_test.ipynb` for detailed comparisons with:
- LevelDB
- RocksDB
- SQLite
- Other embedded databases

## Interpreting Results

### Write Performance

- **High throughput**: Good for write-heavy workloads
- **Low latency**: Good for real-time applications
- **Consistent**: Low variance is important

### Read Performance

- **Cache hit rate**: Should be high for hot data
- **Read amplification**: Should be low (ideally < 2x)
- **Latency distribution**: P99 should be reasonable

### Space Efficiency

- **Space amplification**: Should be low (ideally < 1.5x)
- **Compaction overhead**: Temporary 2x during merge

### Concurrency

- **Read scalability**: Should scale with threads
- **Write contention**: Single writer may limit throughput

## Optimization Tips

1. **Tune memtable size**: Larger = fewer flushes, more memory
2. **Tune compaction thresholds**: Lower = more frequent, less space
3. **Increase cache size**: Better read performance
4. **Use appropriate value sizes**: Smaller = better performance
5. **Batch writes**: If possible, batch multiple operations

## Known Issues

1. **Write amplification**: Each write goes to memtable + WAL
2. **Read amplification**: May check multiple levels
3. **Compaction lag**: Background thread may not keep up
4. **Single writer**: Limits write throughput

## Reporting Issues

When reporting performance issues, include:
- Benchmark scenario
- System specifications
- Value sizes
- Number of keys
- Cache size
- Compilation flags
- Full benchmark output

