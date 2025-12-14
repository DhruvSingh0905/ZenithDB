cmake_minimum_required(VERSION 3.20)
project(AbsoluteDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------
# GoogleTest
# ------------------------------
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
include(GoogleTest)

# ------------------------------
# Core library (all engine code)
# ------------------------------
add_library(zenithdb_lib STATIC
    src/db.cpp
    src/memtable.cpp
    src/wal.cpp
    src/sstable.cpp
    src/manifest.cpp
    src/compaction.cpp
    src/fs_posix.cpp  # <--- CRITICAL FIX: Added this file
)

target_include_directories(zenithdb_lib PUBLIC src)

# ------------------------------
# REPL executable — THIS WAS MISSING!
# ------------------------------
add_executable(zenithdb src/main.cpp)
target_link_libraries(zenithdb PRIVATE zenithdb_lib)

# ------------------------------
# Tests
# ------------------------------
add_executable(tests
    tests/test_vfs.cpp


)
add_executable(zenithdb_bench
    benchmarks/bench_zenithdb.cpp
)

target_link_libraries(zenithdb_bench PRIVATE zenithdb_lib)
target_link_libraries(tests PRIVATE zenithdb_lib gtest_main gtest)
gtest_discover_tests(tests)