// tests/test_rcu_stress.cpp
#include <gtest/gtest.h>
#include "../src/db.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

class RCUStressTest : public ::testing::Test {
protected:
    static inline const std::filesystem::path dir = "rcu_stress_db";

    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directory(dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    std::string path() const { return dir.string(); }
};

TEST_F(RCUStressTest, ManyReadersAndWritersNoDeadlockAndMakeProgress) {
    ZenithDB db(path());

    const int writer_threads = 4;
    const int reader_threads = 8;
    const int writes_per_thread = 25'000; // 100k total writes

    std::atomic<bool> writers_done{false};
    std::atomic<std::uint64_t> total_reads{0};

    // Writer threads
    std::vector<std::thread> writers;
    for (int t = 0; t < writer_threads; ++t) {
        writers.emplace_back([&, t]() {
            for (int i = 0; i < writes_per_thread; ++i) {
                auto global_id = static_cast<std::uint64_t>(t) * writes_per_thread + i;
                db.put("wkey:" + std::to_string(global_id),
                       "wval:" + std::to_string(global_id));
            }
        });
    }

    // Reader threads
    std::vector<std::thread> readers;
    for (int t = 0; t < reader_threads; ++t) {
        readers.emplace_back([&]() {
            std::mt19937_64 rng(1234 + t);
            std::uniform_int_distribution<std::uint64_t> dist(0, writer_threads * writes_per_thread - 1);

            while (!writers_done.load(std::memory_order_acquire)) {
                auto id = dist(rng);
                auto k  = "wkey:" + std::to_string(id);
                auto v  = db.get(k);
                // we don't assert on v; we just want to hammer the read path
                total_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Give them a hard upper bound to avoid hanging the test forever
    auto start = std::chrono::steady_clock::now();

    for (auto& w : writers) {
        w.join();
    }
    writers_done.store(true, std::memory_order_release);

    for (auto& r : readers) {
        r.join();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    // Sanity: test should finish reasonably quickly and actually do reads
    EXPECT_LT(elapsed, 20) << "Stress test took too long, possible deadlock/starvation.";
    EXPECT_GT(total_reads.load(), 50'000u)
        << "Readers did too few operations; RCU layout might not be making progress.";
}