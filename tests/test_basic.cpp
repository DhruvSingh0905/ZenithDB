// tests/test_basic.cpp
#include <gtest/gtest.h>
#include "../src/db.h"
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

class LSMTest : public ::testing::Test {
protected:
    static inline const std::filesystem::path dir = "test_db_dir";

    void SetUp() override {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directory(dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    std::string path() const { return dir.string(); }
};

TEST_F(LSMTest, BasicPutGet) {
    ZenithDB db(path());
    db.put("hello", "world");
    ASSERT_EQ(db.get("hello").value(), "world");
    ASSERT_FALSE(db.get("nope").has_value());
}

TEST_F(LSMTest, DeleteRemovesValue) {
    ZenithDB db(path());
    db.put("key", "value");
    db.remove("key");
    ASSERT_FALSE(db.get("key").has_value());
}

TEST_F(LSMTest, SurvivesRestart) {
    {
        ZenithDB db(path());
        db.put("persist", "yes");
        db.put("number", "42");
    }  // destructor runs, WAL syncs

    ZenithDB db2(path());
    ASSERT_EQ(db2.get("persist").value(), "yes");
    ASSERT_EQ(db2.get("number").value(), "42");
}

TEST_F(LSMTest, LargeMemtableTriggersFlush) {
    {
        ZenithDB db(path());
        std::string big(1000, 'x');  // ~1KB
        for (int i = 0; i < 6000; ++i) {  // >4MB
            db.put("key" + std::to_string(i), big);
        }
    }  // flush happens in destructor or background thread

    // Restart
    ZenithDB db2(path());
    ASSERT_EQ(db2.get("key0").value(), std::string(1000, 'x'));
    ASSERT_EQ(db2.get("key5000").value(), std::string(1000, 'x'));
}

TEST_F(LSMTest, ScanWorksAcrossMemtableAndSSTables) {
    {
        ZenithDB db(path());
        db.put("user:alice", "25");
        db.put("user:bob", "30");
        db.put("user:carol", "28");

        // Force flush by writing >4MB
        std::string blob(2*1024*1024, 'X');
        db.put("flushme", blob);
    }

    ZenithDB db(path());
    auto results = db.scan("user:", "user:z");

    ASSERT_EQ(results.size(), 3);
    ASSERT_EQ(results[0].first, "user:alice");
    ASSERT_EQ(results[1].first, "user:bob");
    ASSERT_EQ(results[2].first, "user:carol");
}

TEST_F(LSMTest, TombstoneHidesValueAfterFlush) {
    {
        ZenithDB db(path());
        db.put("secret", "topsecret");
    }
    {
        ZenithDB db(path());
        db.remove("secret");
        std::string big(3*1024*1024, 'Y');
        db.put("forceflush", big);  // triggers flush
    }
    {
        ZenithDB db(path());
        ASSERT_FALSE(db.get("secret").has_value());
    }
}

TEST_F(LSMTest, ScanEmptyRange) {
    ZenithDB db(path());
    db.put("a", "1");
    db.put("c", "3");
    auto res = db.scan("b", "b");
    ASSERT_TRUE(res.empty());
}