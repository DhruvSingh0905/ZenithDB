// benchmarks/bench_zenithdb.cpp
#include "../src/db.h"    // adjust path if your tree is different
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

static std::string bench_dir = "bench_db";

// ---------------------------
// Simple timer helper
// ---------------------------
struct Timer {
    high_resolution_clock::time_point start;
    Timer() : start(high_resolution_clock::now()) {}
    double elapsed_seconds() const {
        auto end = high_resolution_clock::now();
        return duration<double>(end - start).count();
    }
};

void cleanup_dir() {
    std::filesystem::remove_all(bench_dir);
    std::filesystem::create_directory(bench_dir);
}

std::string db_path() {
    return bench_dir;
}

// Zero-padded key helper
static std::string key_of(std::uint64_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k%016llu",
                  static_cast<unsigned long long>(i));
    return std::string(buf);
}

// ----------------------------------------------------------
// 1. Sequential write test (single-threaded)
// ----------------------------------------------------------
void benchmark_sequential_writes(std::uint64_t num_keys, std::size_t value_size) {
    cleanup_dir();
    ZenithDB db(db_path());

    std::string value(value_size, 'X');

    std::cout << "== Sequential writes: " << num_keys
              << " keys, value size = " << value_size << " bytes\n";

    Timer t;
    for (std::uint64_t i = 0; i < num_keys; ++i) {
        db.put(key_of(i), value);
    }
    double secs = t.elapsed_seconds();
    double ops_per_sec = num_keys / secs;

    std::cout << "Time: " << secs << " s\n";
    std::cout << "Throughput: " << ops_per_sec << " writes/sec\n";
    std::cout << "Avg latency: " << (secs * 1e6 / num_keys) << " us/op\n\n";
}

// ----------------------------------------------------------
// 2. Random read test (single-threaded)
// ----------------------------------------------------------
void benchmark_random_reads(std::uint64_t num_keys,
                            std::uint64_t num_reads,
                            std::size_t  value_size) {
    cleanup_dir();
    ZenithDB db(db_path());

    std::string value(value_size, 'Y');

    // Preload
    for (std::uint64_t i = 0; i < num_keys; ++i) {
        db.put(key_of(i), value);
    }

    // Let background worker flush/compact a bit
    std::this_thread::sleep_for(3s);

    std::cout << "== Random reads (single-threaded): " << num_reads
              << " reads from " << num_keys << " keys\n";

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> dist(0, num_keys - 1);

    std::uint64_t found = 0;

    Timer t;
    for (std::uint64_t i = 0; i < num_reads; ++i) {
        auto k = key_of(dist(rng));
        auto v = db.get(k);
        if (v.has_value()) found++;
    }
    double secs = t.elapsed_seconds();
    double ops_per_sec = num_reads / secs;

    std::cout << "Time: " << secs << " s\n";
    std::cout << "Throughput: " << ops_per_sec << " reads/sec\n";
    std::cout << "Avg latency: " << (secs * 1e6 / num_reads) << " us/op\n";
    std::cout << "Found: " << found << "/" << num_reads << "\n\n";
}

// ----------------------------------------------------------
// 3. Range scan test (single-threaded)
// ----------------------------------------------------------
void benchmark_range_scans(std::uint64_t num_keys,
                           std::uint64_t scan_range_size,
                           std::uint64_t num_scans,
                           std::size_t  value_size) {
    cleanup_dir();
    ZenithDB db(db_path());

    std::string value(value_size, 'Z');
    for (std::uint64_t i = 0; i < num_keys; ++i) {
        db.put(key_of(i), value);
    }

    std::this_thread::sleep_for(3s);

    std::cout << "== Range scans (single-threaded): " << num_scans
              << " scans of " << scan_range_size
              << " keys, from universe of " << num_keys << "\n";

    std::mt19937_64 rng(6789);
    std::uniform_int_distribution<std::uint64_t> dist(0, num_keys - scan_range_size - 1);

    std::uint64_t total_returned = 0;

    Timer t;
    for (std::uint64_t s = 0; s < num_scans; ++s) {
        std::uint64_t start_i = dist(rng);
        std::uint64_t end_i   = start_i + scan_range_size - 1;

        auto start_key = key_of(start_i);
        auto end_key   = key_of(end_i);

        auto rows = db.scan(start_key, end_key);
        total_returned += rows.size();
    }
    double secs = t.elapsed_seconds();
    double ops_per_sec = num_scans / secs;

    std::cout << "Time: " << secs << " s\n";
    std::cout << "Throughput: " << ops_per_sec << " scans/sec\n";
    std::cout << "Avg latency: " << (secs * 1e6 / num_scans) << " us/scan\n";
    std::cout << "Avg rows per scan: "
              << (double)total_returned / num_scans << "\n\n";
}

// ----------------------------------------------------------
// 4. Concurrent write + many readers (RCU stress-style)
// ----------------------------------------------------------
void benchmark_concurrent_write_read(std::uint64_t num_writes,
                                     std::uint64_t num_reads_per_thread,
                                     std::size_t  value_size,
                                     unsigned int num_reader_threads) {
    cleanup_dir();
    ZenithDB db(db_path());

    std::cout << "== Concurrent write/read\n";
    std::cout << "Writes: " << num_writes
              << ", Reads per reader: " << num_reads_per_thread
              << ", Reader threads: " << num_reader_threads << "\n";

    std::string value(value_size, 'W');

    std::atomic<bool> writer_done{false};

    // Writer thread
    std::thread writer([&]() {
        Timer t;
        for (std::uint64_t i = 0; i < num_writes; ++i) {
            db.put(key_of(i), value);
        }
        double secs = t.elapsed_seconds();
        std::cout << "[Writer] Time: " << secs << " s, "
                  << (num_writes / secs) << " writes/sec\n";
        writer_done.store(true, std::memory_order_release);
    });

    // Reader threads
    std::vector<std::thread> readers;
    readers.reserve(num_reader_threads);
    std::atomic<std::uint64_t> total_reads{0};

    for (unsigned int t_id = 0; t_id < num_reader_threads; ++t_id) {
        readers.emplace_back([&]() {
            std::mt19937_64 rng(9999 + t_id);
            std::uniform_int_distribution<std::uint64_t> dist(0, num_writes - 1);

            Timer t;
            std::uint64_t reads_done = 0;
            while (reads_done < num_reads_per_thread) {
                auto k = key_of(dist(rng));
                auto v = db.get(k);
                (void)v;
                ++reads_done;
            }
            double secs = t.elapsed_seconds();
            total_reads.fetch_add(reads_done, std::memory_order_relaxed);
            std::cout << "[Reader-" << t_id << "] Time: " << secs << " s, "
                      << (reads_done / secs) << " reads/sec, reads_done="
                      << reads_done << "\n";
        });
    }

    writer.join();
    for (auto& th : readers) th.join();

    std::cout << "[Readers] Total reads: " << total_reads.load() << "\n\n";
}

// ----------------------------------------------------------
// 5. Multi-threaded read-only stress (VERY HEAVY)
// ----------------------------------------------------------
void benchmark_multi_thread_read_only(std::uint64_t num_keys,
                                      std::uint64_t reads_per_thread,
                                      std::size_t  value_size,
                                      unsigned int num_threads) {
    cleanup_dir();
    ZenithDB db(db_path());

    std::string value(value_size, 'R');

    std::cout << "== Prefill for multi-threaded read-only stress: "
              << num_keys << " keys\n";

    {
        Timer t;
        for (std::uint64_t i = 0; i < num_keys; ++i) {
            db.put(key_of(i), value);
        }
        double secs = t.elapsed_seconds();
        std::cout << "Prefill time: " << secs << " s ("
                  << (num_keys / secs) << " writes/sec)\n";
    }

    std::this_thread::sleep_for(5s); // let compaction settle

    std::cout << "== Multi-threaded read-only stress\n";
    std::cout << "Threads: " << num_threads
              << ", Reads/thread: " << reads_per_thread << "\n";

    std::atomic<std::uint64_t> total_reads{0};

    Timer global;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (unsigned int t_id = 0; t_id < num_threads; ++t_id) {
        threads.emplace_back([&]() {
            std::mt19937_64 rng(424242 + t_id);
            std::uniform_int_distribution<std::uint64_t> dist(0, num_keys - 1);

            std::uint64_t local_reads = 0;
            while (local_reads < reads_per_thread) {
                auto k = key_of(dist(rng));
                auto v = db.get(k);
                (void)v;
                ++local_reads;
            }
            total_reads.fetch_add(local_reads, std::memory_order_relaxed);
        });
    }

    for (auto& th : threads) th.join();

    double secs = global.elapsed_seconds();
    double ops_per_sec = total_reads.load() / secs;

    std::cout << "Total time: " << secs << " s\n";
    std::cout << "Total reads: " << total_reads.load() << "\n";
    std::cout << "Throughput: " << ops_per_sec << " reads/sec\n";
    std::cout << "Avg latency: "
              << (secs * 1e6 / total_reads.load()) << " us/op\n\n";
}

// ----------------------------------------------------------
// main: HEAVY + VERY HEAVY scenarios
// ----------------------------------------------------------
int main(int argc, char** argv) {
    std::cout << "ZenithDB stress benchmark\n";
    std::cout << "Data dir: " << bench_dir << "\n\n";

    // You can tweak these to taste based on your machine
    // -----------------------
    // HEAVY scenario
    // -----------------------
    std::uint64_t heavy_num_keys        = 1'000'000;   // 1M
    std::uint64_t heavy_num_reads       = 1'000'000;   // 1M random reads
    std::uint64_t heavy_scan_range      = 1'000;       // 1k keys per scan
    std::uint64_t heavy_num_scans       = 5'000;       // 5k scans
    std::size_t   heavy_value_size      = 64;          // 64-byte values
    unsigned int  heavy_reader_threads  = 4;
    std::uint64_t heavy_reads_per_thread = 500'000;    // 0.5M each => 2M total

    // -----------------------
    // VERY HEAVY scenario
    // -----------------------
    std::uint64_t very_num_keys         = 3'000'000;   // 3M keys
    std::uint64_t very_reads_per_thread = 1'000'000;   // 1M per thread
    unsigned int  very_reader_threads   = 8;
    std::size_t   very_value_size       = 64;

    std::cout << "=== HEAVY SCENARIO ===\n\n";

    // 1. Heavy sequential writes
    benchmark_sequential_writes(heavy_num_keys, heavy_value_size);

    // 2. Heavy random reads (single-threaded)
    benchmark_random_reads(heavy_num_keys, heavy_num_reads, heavy_value_size);

    // 3. Heavy range scans (single-threaded)
    benchmark_range_scans(heavy_num_keys,
                          heavy_scan_range,
                          heavy_num_scans,
                          heavy_value_size);

    // 4. Concurrent write + read (mixed workload)
    benchmark_concurrent_write_read(
        heavy_num_keys,        // writes
        heavy_reads_per_thread,
        heavy_value_size,
        heavy_reader_threads);

    std::cout << "=== VERY HEAVY SCENARIO ===\n\n";

    // 5. Multi-threaded read-only stress over a large dataset
    benchmark_multi_thread_read_only(
        very_num_keys,
        very_reads_per_thread,
        very_value_size,
        very_reader_threads);

    return 0;
}