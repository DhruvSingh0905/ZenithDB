// src/skiplist.h
#pragma once

#include "arena.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <new>

// Forward declaration
struct Node;

/**
 * SkipList - Lock-free concurrent skip list data structure.
 */
class SkipList {
public:
    struct Node {
        std::string_view key;      
        std::string_view value;    

        void SetNext(int i, Node* x) {
            assert(i >= 0);
            std::atomic<Node*>* next_ptr = reinterpret_cast<std::atomic<Node*>*>(&next_[0]);
            next_ptr[i].store(x, std::memory_order_release);
        }

        Node* Next(int i) {
            assert(i >= 0);
            std::atomic<Node*>* next_ptr = reinterpret_cast<std::atomic<Node*>*>(&next_[0]);
            return next_ptr[i].load(std::memory_order_acquire);
        }
        
        std::atomic<Node*> next_[1]; 
    };

    explicit SkipList(Arena* arena);

    void Insert(std::string_view key, std::string_view value);
    bool Contains(std::string_view key) const;
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
    Node* head_;
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
    head_ = NewNode("", "", kMaxHeight);
    for (int i = 0; i < kMaxHeight; ++i) {
        head_->SetNext(i, nullptr);
    }
}

inline SkipList::Node* SkipList::NewNode(std::string_view key, std::string_view value, int height) {
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

    size_t node_mem_size = sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1);
    char* node_mem = arena_->AllocateAligned(node_mem_size);
    
    Node* node = reinterpret_cast<Node*>(node_mem);
    new (node) Node;
    
    node->key = (key_ptr ? std::string_view(key_ptr, key.size()) : std::string_view());
    node->value = (val_ptr ? std::string_view(val_ptr, value.size()) : std::string_view());
    return node;
}

inline int SkipList::RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && ((std::rand() % 4) == 0)) {
        height++;
    }
    return height;
}

inline bool SkipList::KeyIsAfterNode(std::string_view key, Node* n) const {
    return (n != nullptr) && (n->key < key);
}

inline SkipList::Node* SkipList::FindGreaterOrEqual(std::string_view key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (KeyIsAfterNode(key, next)) {
            x = next;
        } else {
            if (prev != nullptr) prev[level] = x;
            if (level == 0) {
                return next;
            } else {
                level--;
            }
        }
    }
}

inline SkipList::Node* SkipList::FindLessThan(std::string_view key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
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

inline SkipList::Node* SkipList::FindLast() const {
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
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    if (x != nullptr && x->key == key) {
        if (value.empty()) {
            x->value = std::string_view();
        } else {
            char* val_ptr = arena_->Allocate(value.size());
            std::memcpy(val_ptr, value.data(), value.size());
            x->value = std::string_view(val_ptr, value.size());
        }
        return;
    }

    int height = RandomHeight();
    if (height > GetMaxHeight()) {
        for (int i = GetMaxHeight(); i < height; i++) {
            prev[i] = head_;
        }
        max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, value, height);
    for (int i = 0; i < height; i++) {
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

// Iterator implementation
inline SkipList::Iterator::Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

inline bool SkipList::Iterator::Valid() const { return node_ != nullptr; }

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
    if (node_ == list_->head_) node_ = nullptr;
}

inline void SkipList::Iterator::Seek(std::string_view target) {
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}

inline void SkipList::Iterator::SeekToFirst() {
    node_ = list_->head_->Next(0);
}

inline void SkipList::Iterator::SeekToLast() {
    node_ = list_->FindLast();
    if (node_ == list_->head_) node_ = nullptr;
}