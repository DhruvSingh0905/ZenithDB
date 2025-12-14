#pragma once
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <queue>
#include <mutex>
#include <iostream>
#include <thread>
#include <condition_variable>

// Message definitions
struct Message {
    enum Type { 
        PUT,      // Eager replication push
        GOSSIP    // Anti-entropy push
    };

    Type type;
    std::string sender;
    std::string receiver;
    std::string key;
    std::string payload; // Serialized CRDT
};

// Abstract Transport Interface
class Transport {
public:
    using MessageCallback = std::function<void(const Message&)>;

    virtual ~Transport() = default;
    virtual void Register(const std::string& node_id, MessageCallback cb) = 0;
    virtual void Send(const Message& msg) = 0;
};

// Mock Transport for Simulation (In-Memory Message Bus)
class MockTransport : public Transport {
public:
    void Register(const std::string& node_id, MessageCallback cb) override {
        std::lock_guard<std::mutex> lk(mutex_);
        callbacks_[node_id] = cb;
    }

    void Send(const Message& msg) override {
        std::lock_guard<std::mutex> lk(mutex_);
        // Simulate network delay or partitioning here if needed
        // For now, deliver immediately to a queue to be processed
        queue_.push_back(msg);
        cv_.notify_all();
    }

    // Process all pending messages (run this in a separate thread or loop)
    void DeliverAll() {
        while (true) {
            Message msg;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (queue_.empty()) break;
                msg = queue_.front();
                queue_.pop_front();
            }
            
            // Deliver
            if (callbacks_.count(msg.receiver)) {
                if (partitioned_.count(msg.receiver) && partitioned_.at(msg.receiver)) {
                    // Drop message if node is partitioned (simulates network failure)
                    continue;
                }
                callbacks_[msg.receiver](msg);
            }
        }
    }

    void Partition(const std::string& node_id, bool is_partitioned) {
        std::lock_guard<std::mutex> lk(mutex_);
        partitioned_[node_id] = is_partitioned;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Message> queue_;
    std::map<std::string, MessageCallback> callbacks_;
    std::map<std::string, bool> partitioned_;
};