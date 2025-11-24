#pragma once

#include <vector>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>

/**
 * Arena - Fast memory pool allocator for zero-fragmentation allocations.
 * 
 * This is a critical low-level optimization that eliminates memory fragmentation
 * and reduces allocation overhead. The arena allocates memory in fixed-size blocks
 * (4KB) and serves allocations from the current block. When a block is exhausted,
 * a new block is allocated. All memory is freed when the arena is destroyed.
 * 
 * Key optimizations:
 * - Zero fragmentation: All allocations from contiguous blocks
 * - Fast allocation: O(1) for most allocations (no free list traversal)
 * - Cache-friendly: Sequential allocations are in contiguous memory
 * - Thread-safe memory usage tracking: Uses atomic for concurrent reads
 * 
 * Used by SkipList to store all node data, eliminating per-node allocations
 * and improving cache locality.
 */
class Arena {
public:
    /**
     * Constructs an empty arena.
     * 
     * Initializes with no allocated blocks. First allocation will trigger
     * block allocation.
     */
    Arena() : memory_usage_(0) {
        alloc_ptr_ = nullptr;
        alloc_bytes_remaining_ = 0;
    }

    /**
     * Destructor.
     * 
     * Frees all allocated blocks. This is the only way to free memory
     * allocated from the arena (no individual deallocation).
     */
    ~Arena() {
        for (char* ptr : blocks_) {
            delete[] ptr;
        }
    }

    /**
     * Allocates a block of memory of the specified size.
     * 
     * CRITICAL OPTIMIZATION: Most allocations are O(1) - just increment
     * a pointer. No free list traversal, no heap metadata overhead.
     * 
     * @param bytes Number of bytes to allocate (must be > 0)
     * @return Pointer to allocated memory (valid until arena destruction)
     */
    char* Allocate(size_t bytes) {
        // The semantics of what to return are a bit messy if we allow
        // 0-byte allocations, so we disallow them here (we don't need
        // them for our internal data structures).
        assert(bytes > 0);
        if (bytes <= alloc_bytes_remaining_) {
            char* result = alloc_ptr_;
            alloc_ptr_ += bytes;
            alloc_bytes_remaining_ -= bytes;
            return result;
        }
        return AllocateFallback(bytes);
    }

    /**
     * Allocates aligned memory (8-byte or pointer-size aligned).
     * 
     * CRITICAL OPTIMIZATION: Ensures proper alignment for atomic operations
     * and cache line boundaries. Uses pointer arithmetic to find alignment
     * within the current block, avoiding unnecessary block allocation.
     * 
     * @param bytes Number of bytes to allocate
     * @return Pointer to aligned memory (valid until arena destruction)
     */
    char* AllocateAligned(size_t bytes) {
        const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
        static_assert((align & (align - 1)) == 0, "Pointer size should be a power of 2");
        
        size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
        size_t slop = (current_mod == 0 ? 0 : align - current_mod);
        size_t needed = bytes + slop;
        
        char* result;
        if (needed <= alloc_bytes_remaining_) {
            result = alloc_ptr_ + slop;
            alloc_ptr_ += needed;
            alloc_bytes_remaining_ -= needed;
        } else {
            // AllocateFallback always returns aligned memory
            result = AllocateFallback(bytes);
        }
        assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
        return result;
    }

    /**
     * Returns the total memory usage of the arena.
     * 
     * Thread-safe: Uses atomic load with relaxed ordering for concurrent reads.
     * 
     * @return Total bytes allocated (including block overhead)
     */
    size_t MemoryUsage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    /**
     * Fallback allocation when current block doesn't have enough space.
     * 
     * Strategy:
     * - Large allocations (> 1KB): Allocate dedicated block to avoid waste
     * - Small allocations: Allocate new 4KB block, use remainder for future allocations
     * 
     * @param bytes Number of bytes needed
     * @return Pointer to allocated memory
     */
    char* AllocateFallback(size_t bytes) {
        if (bytes > kBlockSize / 4) {
            // Object is more than a quarter of our block size.  Allocate it separately
            // to avoid wasting too much space in the remainder of the current block.
            char* result = AllocateNewBlock(bytes);
            return result;
        }

        // We waste the remaining space in the current block.
        alloc_ptr_ = AllocateNewBlock(kBlockSize);
        alloc_bytes_remaining_ = kBlockSize;

        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    /**
     * Allocates a new block and tracks it.
     * 
     * @param block_bytes Size of block to allocate
     * @return Pointer to the new block
     */
    char* AllocateNewBlock(size_t block_bytes) {
        char* result = new char[block_bytes];
        blocks_.push_back(result);
        memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
        return result;
    }

    // Allocation state - current block being used
    char* alloc_ptr_;                    // Current allocation pointer
    size_t alloc_bytes_remaining_;       // Bytes remaining in current block

    // Array of all allocated blocks (freed in destructor)
    std::vector<char*> blocks_;

    // Total memory usage (thread-safe for concurrent reads)
    std::atomic<size_t> memory_usage_;

    // Block size: 4KB matches page size for optimal OS page cache behavior
    static const int kBlockSize = 4096;
};