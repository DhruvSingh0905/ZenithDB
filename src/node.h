#pragma once
#include "db.h"
#include "transport.h"
#include "crdt.h"
#include <vector>
#include <string>
#include <memory>

class Node {
public:
    Node(std::string node_id, std::string db_dir, Transport* transport);

    // Client API
    void Put(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key);

    // Replication Actions
    void PushToReplicas(const std::string& key, const LWWRegister& reg);
    void TriggerAntiEntropy(const std::string& target_node_id);

    // Network Callback
    void OnMessage(const Message& msg);

    std::string id() const { return node_id_; }

    // Helper for testing
    ZenithDB* db() { return db_.get(); }

private:
    std::string node_id_;
    std::unique_ptr<ZenithDB> db_;
    Transport* transport_;
    std::vector<std::string> peers_;

public:
    void AddPeer(const std::string& peer_id) {
        if (peer_id != node_id_) peers_.push_back(peer_id);
    }
};