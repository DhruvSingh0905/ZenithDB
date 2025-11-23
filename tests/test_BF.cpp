#include <gtest/gtest.h>
#include <filesystem>
#include <set>
#include <thread>
#include <chrono>

#include "../src/sstable.h"
#include "../src/block_cache.h"
#include "../src/db.h"

using namespace std::chrono_literals;

// ------------------------
// SSTABLE + BLOOM TESTS
// ------------------------

class SSTableBloomTest : public ::testing::Test {
protected:
    static inline const std::filesystem::path dir = "bloom_test_db";

    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directory(dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    std::filesystem::path path(const std::string& name) const {
        return dir / name;
    }
};

TEST_F(SSTableBloomTest, KeysInsertedAreFoundAndMayContain) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 1000; ++i) {
        entries.emplace_back("key" + std::to_string(i),
                             "value" + std::to_string(i));
    }

    auto file = path("test.sst");
    SSTable::create(file, entries);

    SSTable sst(file);

    // Every inserted key must:
    // 1) pass may_contain()
    // 2) be returned by get()
    for (int i = 0; i < 1000; ++i) {
        std::string k = "key" + std::to_string(i);
        ASSERT_TRUE(sst.may_contain(k)) << "Bloom false-negative for " << k;
        auto v = sst.get(k);
        ASSERT_TRUE(v.has_value()) << "Missing value for " << k;
        EXPECT_EQ(*v, "value" + std::to_string(i));
    }
}

TEST_F(SSTableBloomTest, NonExistingKeysOftenRejectedByBloom) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 1000; ++i) {
        entries.emplace_back("key" + std::to_string(i),
                             "value" + std::to_string(i));
    }

    auto file = path("test.sst");
    SSTable::create(file, entries);
    SSTable sst(file);

    int negatives = 0;
    const int PROBES = 200;
    for (int i = 0; i < PROBES; ++i) {
        std::string k = "miss" + std::to_string(i + 10'000);
        if (!sst.may_contain(k)) {
            negatives++;
        }
    }

    // We can't enforce an exact FP rate, but we expect some rejections.
    EXPECT_GT(negatives, PROBES / 2)
        << "Bloom filter seems too permissive; not rejecting enough non-keys.";
}

TEST_F(SSTableBloomTest, MinMaxMetadataIsCorrect) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 100; ++i) {
        entries.emplace_back("k" + std::to_string(i),
                             "v" + std::to_string(i));
    }

    auto file = path("meta.sst");
    SSTable::create(file, entries);
    SSTable sst(file);

    const auto& meta = sst.meta();
    EXPECT_EQ(meta.min_key, "k0");
    EXPECT_EQ(meta.max_key, "k99");
    EXPECT_EQ(meta.entry_count, 100u);
    EXPECT_GT(meta.file_size, 0u);
}

TEST_F(SSTableBloomTest, ScanRespectsBounds) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 100; ++i) {
        entries.emplace_back("k" + std::to_string(i),
                             "v" + std::to_string(i));
    }

    auto file = path("scan.sst");
    SSTable::create(file, entries);
    SSTable sst(file);

    auto res = sst.scan("k10", "k19");
    std::set<std::string> keys;
    for (auto& [k, v] : res) {
        keys.insert(k);
    }

    EXPECT_EQ(keys.size(), 10u);
    EXPECT_EQ(keys.count("k10"), 1u);
    EXPECT_EQ(keys.count("k19"), 1u);
    EXPECT_EQ(keys.count("k9"), 0u);
    EXPECT_EQ(keys.count("k20"), 0u);
}

// ------------------------
// BLOCK CACHE TESTS
// ------------------------

TEST(BlockCacheTest, ReturnsCachedValues) {
    BlockCache cache;

    cache.put("a", "block_a");
    cache.put("b", "block_b");

    EXPECT_EQ(cache.get("a"), "block_a");
    EXPECT_EQ(cache.get("b"), "block_b");
    EXPECT_EQ(cache.get("missing"), "");
}

TEST(BlockCacheTest, EvictsLeastRecentlyUsed) {
    BlockCache cache;

    // Fill up to capacity (in terms of blocks)
    for (size_t i = 0; i < BlockCache::MAX_BLOCKS; ++i) {
        cache.put("k" + std::to_string(i), "v" + std::to_string(i));
    }

    // Access some keys to make them MRU
    (void)cache.get("k0");
    (void)cache.get("k1");

    // Insert an extra block enough times to cause evictions
    for (int i = 0; i < 10; ++i) {
        cache.put("extra" + std::to_string(i), "vx" + std::to_string(i));
    }

    // At least some early keys should have been evicted.
    int still_present = 0;
    for (size_t i = 2; i < 50; ++i) {
        if (!cache.get("k" + std::to_string(i)).empty()) {
            still_present++;
        }
    }

    EXPECT_LT(still_present, 48)
        << "LRU cache did not evict any of the older entries as expected.";
}

// ------------------------
// DB + BLOOM + COMPACTION TESTS
// ------------------------

class DBBloomIntegrationTest : public ::testing::Test {
protected:
    static inline const std::filesystem::path dir = "db_bloom_integration";

    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directory(dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    std::string path() const { return dir.string(); }
};

TEST_F(DBBloomIntegrationTest, ManyWritesSurviveRestartAndAreQueryable) {
    {
        ZenithDB db(path());
        for (int batch = 0; batch < 10; ++batch) {
            for (int i = 0; i < 1000; ++i) {
                db.put("user:" + std::to_string(batch) + ":" + std::to_string(i),
                       "value:" + std::to_string(batch) + ":" + std::to_string(i));
            }
        }
    }

    // Reopen to exercise manifest + SSTables only
    ZenithDB db(path());

    EXPECT_EQ(db.get("user:0:0").value(), "value:0:0");
    EXPECT_EQ(db.get("user:5:123").value(), "value:5:123");
    EXPECT_FALSE(db.get("user:999:999").has_value());
}

TEST_F(DBBloomIntegrationTest, RangeScanAcrossManyTablesIsCorrect) {
    {
        ZenithDB db(path());
        for (int i = 0; i < 5000; ++i) {
            // Zero-padded 4-digit keys: k0000, k0001, ..., k4999
            char buf[16];
            std::snprintf(buf, sizeof(buf), "k%04d", i);
            db.put(buf, "v" + std::to_string(i));
        }
    }

    ZenithDB db(path());

    // Matching padded range
    auto res = db.scan("k1000", "k1999");
    std::set<std::string> keys;
    for (auto& [k, v] : res) keys.insert(k);

    EXPECT_EQ(keys.size(), 1000u);
    EXPECT_EQ(keys.count("k1000"), 1u);
    EXPECT_EQ(keys.count("k1500"), 1u);
    EXPECT_EQ(keys.count("k1999"), 1u);
    EXPECT_EQ(keys.count("k0999"), 0u);
    EXPECT_EQ(keys.count("k2000"), 0u);
}

TEST_F(DBBloomIntegrationTest, CompactionStillPreservesValues) {
    {
        ZenithDB db(path());

        // Enough writes to force multiple memtable flushes and compaction.
        std::string big(10'000, 'X');
        for (int batch = 0; batch < 8; ++batch) {
            for (int i = 0; i < 200; ++i) {
                db.put("cpt:" + std::to_string(batch) + ":" + std::to_string(i),
                       "val:" + std::to_string(batch) + ":" + std::to_string(i));
            }
            std::this_thread::sleep_for(200ms); // give background thread time
        }
    }

    // Let background worker catch up and compact
    std::this_thread::sleep_for(4s);

    ZenithDB db(path());

    // Spot-check some keys
    EXPECT_EQ(db.get("cpt:0:0").value(), "val:0:0");
    EXPECT_EQ(db.get("cpt:3:123").value(), "val:3:123");
    EXPECT_EQ(db.get("cpt:7:199").value(), "val:7:199");
}

TEST_F(DBBloomIntegrationTest, BloomAndMinMaxDoNotBreakMissingKeyBehavior) {
    {
        ZenithDB db(path());
        for (int i = 0; i < 3000; ++i) {
            db.put("present:" + std::to_string(i),
                   "vp:" + std::to_string(i));
        }
    }

    ZenithDB db(path());

    // This key was never inserted → should stay missing
    EXPECT_FALSE(db.get("absent:1234").has_value());

    // Neighboring existing key still returns correctly
    EXPECT_EQ(db.get("present:100").value(), "vp:100");
}