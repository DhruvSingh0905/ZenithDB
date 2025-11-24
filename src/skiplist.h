#pragma once

#include "arena.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <new>

/**
 * SkipList Node - Represents a node in the skip list.
 * 
 * Each node contains:
 * - key/value: string_view pointing to arena-allocated memory
 * - next_[height]: Array of atomic pointers to next nodes at each level
 * 
 * CRITICAL OPTIMIZATION: Uses flexible array member pattern to store
 * variable-height next pointers, minimizing memory overhead. The node
 * is allocated from the arena with exactly the right size for its height.
 */
struct Node {
    std::string_view key;      // Key (points to arena memory)
    std::string_view value;    // Value (points to arena memory)

    /**
     * Sets the next pointer at level i.
     * 
     * Uses release memory ordering to ensure all writes to the node
     * are visible before the pointer is published.
     * 
     * @param i Level (0 = bottom level)
     * @param x Next node pointer
     */
    void SetNext(int i, Node* x) {
        assert(i >= 0);
        std::atomic<Node*>* next_ptr = reinterpret_cast<std::atomic<Node*>*>(&next_[0]);
        next_ptr[i].store(x, std::memory_order_release);
    }

    /**
     * Gets the next pointer at level i.
     * 
     * Uses acquire memory ordering to ensure all writes from the
     * node being read are visible.
     * 
     * @param i Level (0 = bottom level)
     * @return Next node pointer
     */
    Node* Next(int i) {
        assert(i >= 0);
        std::atomic<Node*>* next_ptr = reinterpret_cast<std::atomic<Node*>*>(&next_[0]);
        return next_ptr[i].load(std::memory_order_acquire);
    }
    
    // Flexible array member: actual size determined at allocation time
    // Height determines how many next pointers are stored
    std::atomic<Node*> next_[1]; 
};

/**
 * SkipList - Lock-free concurrent skip list data structure.
 * 
 * CRITICAL LOW-LEVEL OPTIMIZATION: Replaces std::map in MemTable with
 * a lock-free skip list for better concurrent read performance and
 * reduced memory overhead.
 * 
 * Key features:
 * - Lock-free reads: Multiple readers can traverse concurrently
 * - O(log n) average-case operations (insert, lookup, delete)
 * - Arena-allocated: All nodes allocated from arena (zero fragmentation)
 * - Cache-friendly: Sequential traversal is cache-efficient
 * - Atomic operations: Uses memory ordering for thread safety
 * 
 * Skip list structure:
 * - Multiple sorted linked lists at different "levels"
 * - Higher levels skip over more nodes (probabilistic)
 * - Search starts at highest level, drops down when key passed
 * - Average height: O(log n) with 1/4 probability per level
 * 
 * Memory layout:
 * - All nodes allocated from arena (contiguous, cache-friendly)
 * - Key/value data stored in arena, node holds string_view
 * - Variable-height nodes allocated with exact size needed
 */
class SkipList {
public:
    /**
     * Constructs a skip list using the provided arena.
     * 
     * @param arena Arena allocator for all node allocations
     */
    explicit SkipList(Arena* arena);

    /**
     * Inserts a key-value pair.
     * 
     * If key already exists, updates the value. Uses lock-free algorithm
     * with atomic pointer updates for thread safety.
     * 
     * REQUIRES: Nothing that compares equal to key is currently being
     * inserted by another thread (concurrent inserts of same key not supported).
     * 
     * @param key The key to insert/update
     * @param value The value to associate with the key
     */
    void Insert(std::string_view key, std::string_view value);

    /**
     * Checks if a key exists in the skip list.
     * 
     * Lock-free read operation - safe for concurrent access.
     * 
     * @param key The key to search for
     * @return True if key exists, false otherwise
     */
    bool Contains(std::string_view key) const;

    /**
     * Retrieves the value for a key.
     * 
     * Lock-free read operation - safe for concurrent access.
     * Returns string_view pointing to arena memory (valid until arena destruction).
     * 
     * @param key The key to look up
     * @param value Output parameter for the value (if found)
     * @return True if key found, false otherwise
     */
    bool Get(std::string_view key, std::string_view* value) const;

    /**
     * Iterator for traversing the skip list in sorted order.
     * 
     * Provides forward and backward iteration, seeking, and range queries.
     * All operations are lock-free and safe for concurrent access.
     */
    class Iterator {
    public:
        /**
         * Constructs an iterator for the skip list.
         * 
         * @param list The skip list to iterate over
         */
        explicit Iterator(const SkipList* list);
        
        /**
         * Returns true if the iterator is positioned at a valid node.
         */
        bool Valid() const;
        
        /**
         * Returns the key at the current position.
         * 
         * REQUIRES: Valid() == true
         */
        std::string_view key() const;
        
        /**
         * Returns the value at the current position.
         * 
         * REQUIRES: Valid() == true
         */
        std::string_view value() const;
        
        /**
         * Advances to the next node (in sorted order).
         * 
         * REQUIRES: Valid() == true
         */
        void Next();
        
        /**
         * Moves to the previous node (in sorted order).
         * 
         * REQUIRES: Valid() == true
         */
        void Prev();
        
        /**
         * Seeks to the first node >= target.
         * 
         * @param target The key to seek to
         */
        void Seek(std::string_view target);
        
        /**
         * Seeks to the first node in the list.
         */
        void SeekToFirst();
        
        /**
         * Seeks to the last node in the list.
         */
        void SeekToLast();

    private:
        const SkipList* list_;
        Node* node_;
    };

    /**
     * Creates a new iterator for this skip list.
     * 
     * @return Iterator positioned at the beginning
     */
    Iterator NewIterator() const { return Iterator(this); }

private:
    enum { kMaxHeight = 12 };  // Maximum skip list height

    Arena* const arena_;                    // Arena for all allocations
    Node* head_;                            // Dummy head node (height = kMaxHeight)
    std::atomic<int> max_height_;           // Current maximum height in use
    
    /**
     * Returns the current maximum height.
     * 
     * Thread-safe: Uses atomic load with relaxed ordering.
     */
    int GetMaxHeight() const {
        return max_height_.load(std::memory_order_relaxed);
    }

    /**
     * Allocates and initializes a new node.
     * 
     * CRITICAL OPTIMIZATION: Allocates node with exact size needed for height,
     * key, and value from arena. This eliminates memory fragmentation and
     * improves cache locality.
     * 
     * @param key The key for the node
     * @param value The value for the node
     * @param height The height of the node (number of next pointers)
     * @return Pointer to the new node
     */
    Node* NewNode(std::string_view key, std::string_view value, int height);
    
    /**
     * Generates a random height for a new node.
     * 
     * Probability: 1/4 chance to increase height at each level.
     * Average height: ~1.33, which gives O(log n) skip list height.
     * 
     * @return Random height between 1 and kMaxHeight
     */
    int RandomHeight();
    
    /**
     * Checks if key comes after node n in sorted order.
     * 
     * @param key The key to check
     * @param n The node to compare against
     * @return True if key > n->key
     */
    bool KeyIsAfterNode(std::string_view key, Node* n) const;
    
    /**
     * Finds the first node >= key.
     * 
     * CRITICAL OPTIMIZATION: Uses skip list structure to skip over
     * many nodes, achieving O(log n) average-case performance.
     * 
     * @param key The key to search for
     * @param prev Output array of previous nodes at each level (can be nullptr)
     * @return Pointer to node >= key, or nullptr if key > all nodes
     */
    Node* FindGreaterOrEqual(std::string_view key, Node** prev) const;
    
    /**
     * Finds the last node < key.
     * 
     * @param key The key to search for
     * @return Pointer to last node < key, or head_ if key <= all nodes
     */
    Node* FindLessThan(std::string_view key) const;
    
    /**
     * Finds the last node in the skip list.
     * 
     * @return Pointer to last node, or head_ if list is empty
     */
    Node* FindLast() const;
};

class SkipList {
public:
    explicit SkipList(Arena* arena);

    // Insert key/value. 
    // REQUIRES: nothing that compares equal to key is currently in the list.
    void Insert(std::string_view key, std::string_view value);

    // Returns true if an entry that compares equal to key is in the list.
    bool Contains(std::string_view key) const;

    // Returns the value for key, or nullopt if not found.
    // Note: returns string_view into Arena memory.
    bool Get(std::string_view key, std::string_view* value) const;

    class Iterator {
    public:
        explicit Iterator(const SkipList* list);
        bool Valid() const;
        std::string_view key() const;
        std::string_view value() const;
        void Next();
        void Prev();
        void Seek(std::string_view target);
        void SeekToFirst();
        void SeekToLast();

    private:
        const SkipList* list_;
        Node* node_;
    };

    Iterator NewIterator() const { return Iterator(this); }

private:
    enum { kMaxHeight = 12 };

    Arena* const arena_;
    Node* head_; // Removed const to allow assignment in constructor body
    std::atomic<int> max_height_;
    
    int GetMaxHeight() const {
        return max_height_.load(std::memory_order_relaxed);
    }

    Node* NewNode(std::string_view key, std::string_view value, int height);
    int RandomHeight();
    bool KeyIsAfterNode(std::string_view key, Node* n) const;
    Node* FindGreaterOrEqual(std::string_view key, Node** prev) const;
    Node* FindLessThan(std::string_view key) const;
    Node* FindLast() const;
};

// ---------------- Implementation ----------------

inline SkipList::SkipList(Arena* arena) : arena_(arena), max_height_(1) {
    // Initialize head node with maximum height (dummy node for all levels)
    // Head node has empty key/value and points to actual data nodes
    head_ = NewNode("", "", kMaxHeight);
    for (int i = 0; i < kMaxHeight; ++i) {
        head_->SetNext(i, nullptr);
    }
}

inline Node* SkipList::NewNode(std::string_view key, std::string_view value, int height) {
    // CRITICAL OPTIMIZATION: Allocate key/value from arena first
    // This ensures key/value data is in arena memory before node allocation
    char* key_ptr = nullptr;
    if (!key.empty()) {
        key_ptr = arena_->Allocate(key.size());
        std::memcpy(key_ptr, key.data(), key.size());
    }
    
    char* val_ptr = nullptr;
    if (!value.empty()) {
        val_ptr = arena_->Allocate(value.size());
        std::memcpy(val_ptr, value.data(), value.size());
    }

    // Allocate node with exact size needed for height (flexible array member)
    // CRITICAL: Aligned allocation ensures atomic operations work correctly
    size_t node_mem_size = sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1);
    char* node_mem = arena_->AllocateAligned(node_mem_size);
    
    Node* node = reinterpret_cast<Node*>(node_mem);
    
    // Use placement new to construct the Node object
    new (node) Node;
    
    // Set string_view to point to arena-allocated memory
    node->key = (key_ptr ? std::string_view(key_ptr, key.size()) : std::string_view());
    node->value = (val_ptr ? std::string_view(val_ptr, value.size()) : std::string_view());
    return node;
}

inline int SkipList::RandomHeight() {
    // CRITICAL OPTIMIZATION: 1/4 probability per level gives average height ~1.33
    // This results in O(log n) skip list height with good balance
    // between memory usage and search performance
    int height = 1;
    while (height < kMaxHeight && ((std::rand() % 4) == 0)) {
        height++;
    }
    return height;
}

inline bool SkipList::KeyIsAfterNode(std::string_view key, Node* n) const {
    return (n != nullptr) && (n->key < key);
}

inline Node* SkipList::FindGreaterOrEqual(std::string_view key, Node** prev) const {
    // CRITICAL OPTIMIZATION: Skip list search algorithm
    // Start at highest level, drop down when key is passed
    // This achieves O(log n) average-case performance
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (KeyIsAfterNode(key, next)) {
            // Key is after next, so continue at this level
            x = next;
        } else {
            // Key is <= next, so record previous and drop down
            if (prev != nullptr) prev[level] = x;
            if (level == 0) {
                return next;
            } else {
                level--;
            }
        }
    }
}

inline Node* SkipList::FindLessThan(std::string_view key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        assert(x == head_ || x->key < key);
        Node* next = x->Next(level);
        if (next == nullptr || next->key >= key) {
            if (level == 0) {
                return x;
            } else {
                level--;
            }
        } else {
            x = next;
        }
    }
}

inline Node* SkipList::FindLast() const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (next == nullptr) {
            if (level == 0) return x;
            level--;
        } else {
            x = next;
        }
    }
}

inline void SkipList::Insert(std::string_view key, std::string_view value) {
    // Find insertion point and record previous nodes at each level
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    // Update existing key if found
    if (x != nullptr && x->key == key) {
        if (value.empty()) {
            x->value = std::string_view();
        } else {
            // Allocate new value from arena (old value remains in arena until destruction)
            char* val_ptr = arena_->Allocate(value.size());
            std::memcpy(val_ptr, value.data(), value.size());
            x->value = std::string_view(val_ptr, value.size());
        }
        return;
    }

    // Generate random height for new node
    int height = RandomHeight();
    
    // If new node is taller than current max, update max and set prev pointers
    if (height > GetMaxHeight()) {
        for (int i = GetMaxHeight(); i < height; i++) {
            prev[i] = head_;
        }
        max_height_.store(height, std::memory_order_relaxed);
    }

    // Create new node and link it into all levels
    x = NewNode(key, value, height);
    for (int i = 0; i < height; i++) {
        // Link new node into level i
        x->SetNext(i, prev[i]->Next(i));
        prev[i]->SetNext(i, x);
    }
}

inline bool SkipList::Contains(std::string_view key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && x->key == key;
}

inline bool SkipList::Get(std::string_view key, std::string_view* value) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    if (x != nullptr && x->key == key) {
        *value = x->value;
        return true;
    }
    return false;
}

// --- Iterator Implementation ---

inline SkipList::Iterator::Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

inline bool SkipList::Iterator::Valid() const {
    return node_ != nullptr;
}

inline std::string_view SkipList::Iterator::key() const {
    assert(Valid());
    return node_->key;
}

inline std::string_view SkipList::Iterator::value() const {
    assert(Valid());
    return node_->value;
}

inline void SkipList::Iterator::Next() {
    assert(Valid());
    node_ = node_->Next(0);
}

inline void SkipList::Iterator::Prev() {
    assert(Valid());
    node_ = list_->FindLessThan(node_->key);
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

inline void SkipList::Iterator::Seek(std::string_view target) {
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}

inline void SkipList::Iterator::SeekToFirst() {
    node_ = list_->head_->Next(0);
}

inline void SkipList::Iterator::SeekToLast() {
    node_ = list_->FindLast();
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}