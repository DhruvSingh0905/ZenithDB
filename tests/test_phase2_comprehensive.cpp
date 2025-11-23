// tests/test_phase2_comprehensive.cpp
#include <gtest/gtest.h>
#include "../src/db.h"
#include <filesystem>
#include <thread>
#include <chrono>
#include <random>
#include <set>
#include <algorithm>

using namespace std::chrono_literals;

class Phase2ComprehensiveTest : public ::testing::Test {
protected:
    static inline const std::filesystem::path dir = "phase2_comprehensive_db";

    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directory(dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    std::string path() const { return dir.string(); }
};

// 1. Basic persistence + delete + restart
TEST_F(Phase2ComprehensiveTest, BasicPersistenceAndRestart) {
    {
        ZenithDB db(path());
        db.put("alice", "engineer");
        db.put("bob", "designer");
        db.remove("bob");  // only one argument
    }

    ZenithDB db2(path());
    ASSERT_EQ(db2.get("alice").value(), "engineer");
    ASSERT_FALSE(db2.get("bob").has_value());
}

// 2. High write pressure → no blocking in put()
TEST_F(Phase2ComprehensiveTest, HighWritePressureNoBlocking) {
    ZenithDB db(path());

    const int N = 15000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        db.put("user:" + std::to_string(i), "active");
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();

    ASSERT_LT(elapsed, 1000) << "Put loop too slow — blocking flush suspected";

    std::this_thread::sleep_for(2s);  // let background flush

    ZenithDB db2(path());
    ASSERT_EQ(db2.get("user:42").value(), "active");
    ASSERT_EQ(db2.get("user:14999").value(), "active");
}

// 3. Tombstones eliminated during compaction
TEST_F(Phase2ComprehensiveTest, TombstoneCompaction) {
    {
        ZenithDB db(path());
        for (int i = 0; i < 10000; ++i) {
            db.put("key" + std::to_string(i), "value");
        }
        for (int i = 0; i < 10000; i += 2) {
            db.remove("key" + std::to_string(i));
        }
    }

    std::this_thread::sleep_for(5s);

    ZenithDB db(path());
    int alive = 0;
    for (int i = 1; i < 10000; i += 2) {
        if (db.get("key" + std::to_string(i)).has_value()) alive++;
    }
    ASSERT_EQ(alive, 5000);
}

// 4. Scan works across current + immutable + SSTables
TEST_F(Phase2ComprehensiveTest, ScanAcrossAllLayers) {
    ZenithDB db(path());

    db.put("a:1", "one");
    db.put("a:3", "three");

    // Force immutable memtable
    std::string big(3'500'000, 'X');
    db.put("force_flush", big);

    db.put("b:2", "two");
    db.put("b:4", "four");

    std::this_thread::sleep_for(1s);

    db.put("c:5", "five");

    auto result = db.scan("a:", "c:z");

    std::set<std::string> keys;
    for (const auto& p : result) keys.insert(p.first);

    ASSERT_EQ(keys.size(), 5);
    ASSERT_EQ(keys.count("a:1"), 1);
    ASSERT_EQ(keys.count("a:3"), 1);
    ASSERT_EQ(keys.count("b:2"), 1);
    ASSERT_EQ(keys.count("b:4"), 1);
    ASSERT_EQ(keys.count("c:5"), 1);
}

TEST_F(Phase2ComprehensiveTest, LeveledCompactionKeepsFilesReasonable) {
    ZenithDB db(path());

    // High-ish write load with pauses to let background worker run.
    for (int batch = 0; batch < 5; ++batch) {
        for (int i = 0; i < 2000; ++i) {
            db.put("test:batch" + std::to_string(batch) + ":k" + std::to_string(i),
                   "value");
        }
        std::this_thread::sleep_for(800ms);  // give background thread time
    }

    // Let compaction / flushing catch up.
    std::this_thread::sleep_for(4s);

    int l0_count = 0;
    int l1_count = 0;
    int total_sst = 0;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto name = entry.path().filename().string();
        if (name.starts_with("L0_")) ++l0_count;
        if (name.starts_with("L1_")) ++l1_count;
        if (name.size() >= 4 && name.substr(name.size() - 4) == ".sst") {
            ++total_sst;
        }
    }

    // 1) We should have created *some* SSTables — otherwise flushing is broken.
    ASSERT_GT(total_sst, 0) 
        << "No SSTable files found at all — memtables never flushed to disk.";

    // 2) L0 should not accumulate an unbounded number of files.
    //    A small handful of L0 files is totally reasonable in a leveled design.
    ASSERT_LE(l0_count, 5)
        << "Too many files remaining in L0 — compaction / flushing not keeping up.";

    // 3) Optionally, we *prefer* to see some L1 files, but don't require it.
    //    This logs for debugging but doesn't fail the test if L1==0.
    if (l1_count == 0) {
        std::cerr << "[INFO] No L1 files yet — workload / thresholds may not have "
                  << "triggered compaction in this run.\n";
    }
}TEST_F(Phase2ComprehensiveTest, StressTestWithDeletes) {
    const int TOTAL = 80000;
    {
        ZenithDB db(path());
        for (int i = 0; i < TOTAL; ++i) {
            std::string k = "stress:" + std::to_string(i);
            db.put(k, "data");
            if (i % 9 == 0) {
                db.remove(k);
            }
        }
    }

    std::this_thread::sleep_for(6s);  // let all compaction finish

    ZenithDB db(path());

    int deleted = TOTAL / 9;                    // 8888
    int expected = TOTAL - deleted-1;             // 71112
    int actual = 0;

    for (int i = 0; i < TOTAL; ++i) {
        if (db.get("stress:" + std::to_string(i)).has_value()) {
            actual++;
        }
    }

    ASSERT_EQ(actual, expected);
}
// 7. Concurrent read/write safety
TEST_F(Phase2ComprehensiveTest, ConcurrentReadWrite) {
    ZenithDB db(path());

    std::thread writer([&db] {
        for (int i = 0; i < 30000; ++i) {
            db.put("thread:" + std::to_string(i), "safe");
        }
    });

    std::thread reader([&db] {
        for (int i = 0; i < 500; ++i) {
            db.scan("thread:", "thread:z");
            std::this_thread::sleep_for(2ms);
        }
    });

    writer.join();
    reader.join();

    ASSERT_TRUE(db.get("thread:12345").has_value());
}

TEST_F(Phase2ComprehensiveTest, ForcesL1Creation) {
    ZenithDB db(path());

    // Big value to force memtable flushes quickly.
    std::string big_value(10'000, 'X');

    const int batches = 6;
    const int entries_per_batch = 20;

    for (int batch = 0; batch < batches; ++batch) {
        for (int i = 0; i < entries_per_batch; ++i) {
            db.put(
                "L1test:batch" + std::to_string(batch) + ":k" + std::to_string(i),
                big_value
            );
        }
        // Give background thread some space to flush between bursts.
        std::this_thread::sleep_for(300ms);
    }

    // Now poll for L1 creation for up to ~10 seconds.
    int l1_count = 0;
    int total_sst = 0;

    auto compute_counts = [&](int& l1, int& total) {
        l1 = 0;
        total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            auto name = entry.path().filename().string();
            if (name.starts_with("L1_")) ++l1;
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".sst") {
                ++total;
            }
        }
    };

    const int max_attempts = 20;      // 20 * 500ms = 10s
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        compute_counts(l1_count, total_sst);
        if (l1_count >= 1) break;
        std::this_thread::sleep_for(500ms);
    }

    // Sanity: we must have created *some* SSTables at all.
    ASSERT_GT(total_sst, 0)
        << "No SSTable files found — memtables never flushed to disk.";

    // Key assertion: at least one L1 file was eventually produced.
    ASSERT_GE(l1_count, 1)
        << "Expected at least one L1 SSTable, but found none after waiting.";
}