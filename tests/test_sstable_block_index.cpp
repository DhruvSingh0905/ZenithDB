#include <gtest/gtest.h>
#include "../src/sstable.h"

#include <filesystem>
#include <random>
#include <cstdio>

static std::filesystem::path tmp_dir = "sstable_block_test_db";

static std::filesystem::path make_path(const std::string& name) {
    std::filesystem::create_directories(tmp_dir);
    return tmp_dir / name;
}

// Zero-padded keys ensure lexicographic ordering matches numeric ordering.
static std::string key_for(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k%04d", i);
    return std::string(buf);
}

class SSTableBlockIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::remove_all(tmp_dir);
        std::filesystem::create_directory(tmp_dir);
    }
    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }
};

TEST_F(SSTableBlockIndexTest, BasicGetAndScan) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 1000; ++i) {
        entries.emplace_back(key_for(i), "v" + std::to_string(i));
    }

    auto path = make_path("basic.sst");
    SSTable::create(path, entries);
    SSTable sst(path);

    EXPECT_EQ(sst.get(key_for(0)).value(),   "v0");
    EXPECT_EQ(sst.get(key_for(10)).value(),  "v10");
    EXPECT_EQ(sst.get(key_for(999)).value(), "v999");
    EXPECT_FALSE(sst.get(key_for(1000)).has_value());

    auto rows = sst.scan(key_for(10), key_for(19));

    ASSERT_EQ(rows.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(rows[i].first,  key_for(10 + i));
        EXPECT_EQ(rows[i].second, "v" + std::to_string(10 + i));
    }
}

TEST_F(SSTableBlockIndexTest, BloomMayContainTrueForInsertedKeys) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 5000; ++i) {
        entries.emplace_back("key_" + std::to_string(i),
                             "value_" + std::to_string(i));
    }

    auto path = make_path("bloom.sst");
    SSTable::create(path, entries);
    SSTable sst(path);

    for (int i = 0; i < 500; ++i) {
        EXPECT_TRUE(sst.may_contain("key_" + std::to_string(i * 10)));
    }

    int definitely_not = 0;
    for (int i = 5000; i < 6000; ++i) {
        if (!sst.may_contain("missing_" + std::to_string(i)))
            definitely_not++;
    }
    EXPECT_GT(definitely_not, 0);
}

TEST_F(SSTableBlockIndexTest, BinarySearchMatchesLinearGet) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 20000; ++i) {
        entries.emplace_back(key_for(i), "v" + std::to_string(i));
    }

    auto path = make_path("many_blocks.sst");
    SSTable::create(path, entries);
    SSTable sst(path);

    for (int i = 0; i < 20000; i += 137) {
        auto k = key_for(i);
        auto v = sst.get(k);
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(v.value(), "v" + std::to_string(i));
    }

    EXPECT_FALSE(sst.get("k-001").has_value());
    EXPECT_FALSE(sst.get("k99999").has_value());
}