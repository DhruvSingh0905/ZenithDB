#include "node.h"
#include <iostream>

// Helper factory for PosixEnv
extern std::unique_ptr<Env> NewPosixEnv();

Node::Node(std::string node_id, std::string db_dir, Transport* transport)
    : node_id_(node_id), transport_(transport) 
{
    // Initialize DB with this node's ID for vector clocks
    db_ = std::make_unique<ZenithDB>(NewPosixEnv(), db_dir, node_id_);
    
    // Register with transport
    transport_->Register(node_id_, [this](const Message& msg) {
        this->OnMessage(msg);
    });
}

void Node::Put(const std::string& key, const std::string& value) {
    // 1. Local Write (advances vector clock)
    db_->put(key, value);

    // 2. Fetch the created CRDT (with the new clock)
    auto crdt_opt = db_->get_crdt(key);
    if (crdt_opt) {
        PushToReplicas(key, *crdt_opt);
    }
}

std::optional<std::string> Node::Get(const std::string& key) {
    return db_->get(key);
}

void Node::PushToReplicas(const std::string& key, const LWWRegister& crdt) {
    Message msg;
    msg.type = Message::PUT;
    msg.sender = node_id_;
    msg.key = key;
    // Send the serialized CRDT (Value + Clock)
    msg.payload = crdt.serialize();

    for (const auto& peer : peers_) {
        msg.receiver = peer;
        transport_->Send(msg);
    }
}

void Node::TriggerAntiEntropy(const std::string& target) {
    // 1. Scan local DB for all CRDTs
    auto results = db_->scan_crdt("", "\xFF");
    
    // 2. Send GOSSIP messages
    for (const auto& kv : results) {
        Message msg;
        msg.type = Message::GOSSIP; 
        msg.sender = node_id_;
        msg.receiver = target;
        msg.key = kv.first;
        msg.payload = kv.second.serialize(); // Preserves Clock info
        transport_->Send(msg);
    }
}

void Node::OnMessage(const Message& msg) {
    if (msg.type == Message::PUT || msg.type == Message::GOSSIP) {
        // Deserialize incoming CRDT
        LWWRegister incoming = LWWRegister::deserialize(msg.payload);
        
        // Merge into local DB
        // ZenithDB::put(key, LWWRegister) handles the merge logic
        db_->put(msg.key, incoming);
    }
}