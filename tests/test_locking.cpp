#include <gtest/gtest.h>
#include "../src/db.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

using namespace std::chrono_literals;

class LockingTest : public ::testing::Test {
protected:
    static inline const std::filesystem::path dir = "locking_test_db";

    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directory(dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    std::string path() const { return dir.string(); }
};

static std::string key_of(std::uint64_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k%016llu",
                  static_cast<unsigned long long>(i));
    return std::string(buf);
}

TEST_F(LockingTest, NoDeadlockUnderConcurrentLoad) {
    ZenithDB db(path());

    const std::uint64_t NUM_KEYS   = 20'000;
    const std::uint64_t NUM_WRITES = 50'000;
    const std::uint64_t NUM_READS  = 100'000;

    std::atomic<bool> writer_done{false};
    std::atomic<bool> reader_done{false};

    auto start = std::chrono::high_resolution_clock::now();

    std::thread writer([&] {
        for (std::uint64_t i = 0; i < NUM_WRITES; ++i) {
            db.put(key_of(i % NUM_KEYS), "v");
            // occasionally force big values to trigger flushes/compaction
            if (i % 10'000 == 0 && i > 0) {
                db.put(key_of(NUM_KEYS + i),
                       std::string(50'000, 'X')); // 50 KB value
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        std::mt19937_64 rng(12345);
        std::uniform_int_distribution<std::uint64_t> dist(0, NUM_KEYS - 1);

        for (std::uint64_t i = 0; i < NUM_READS; ++i) {
            auto k = key_of(dist(rng));
            (void)db.get(k);

            if (i % 2000 == 0) {
                auto res = db.scan("k0000000000000000", "k9999999999999999");
                (void)res;
            }
        }
        reader_done.store(true, std::memory_order_release);
    });

    const auto TIMEOUT = 15s;
    bool timed_out = false;

    while (!(writer_done.load(std::memory_order_acquire) &&
             reader_done.load(std::memory_order_acquire))) {
        auto now = std::chrono::high_resolution_clock::now();
        if (now - start > TIMEOUT) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    writer.join();
    reader.join();

    ASSERT_FALSE(timed_out) << "Concurrent writer/reader test timed out - "
                            << "possible deadlock or severe starvation.";
}