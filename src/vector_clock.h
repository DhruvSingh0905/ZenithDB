#pragma once
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <sstream>

// Comparison results
enum class ClockOrder {
    Equal,
    HappensBefore, // A < B
    HappensAfter,  // A > B
    Concurrent     // A || B
};

class VectorClock {
public:
    using NodeID = std::string;
    using Counter = uint64_t;

    std::map<NodeID, Counter> clock;

    VectorClock() = default;

    // Increment local counter
    void increment(const NodeID& node) {
        clock[node]++;
    }

    // Merge another clock (take max of each entry)
    void merge(const VectorClock& other) {
        for (const auto& [node, counter] : other.clock) {
            clock[node] = std::max(clock[node], counter);
        }
    }

    // Compare this clock with another
    ClockOrder compare(const VectorClock& other) const {
        bool this_has_greater = false;
        bool other_has_greater = false;

        // Union of all keys
        std::vector<NodeID> nodes;
        for (auto& [k, v] : clock) nodes.push_back(k);
        for (auto& [k, v] : other.clock) nodes.push_back(k);
        std::sort(nodes.begin(), nodes.end());
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

        for (const auto& node : nodes) {
            Counter c1 = 0; 
            Counter c2 = 0;
            if (clock.count(node)) c1 = clock.at(node);
            if (other.clock.count(node)) c2 = other.clock.at(node);

            if (c1 > c2) this_has_greater = true;
            if (c2 > c1) other_has_greater = true;
        }

        if (!this_has_greater && !other_has_greater) return ClockOrder::Equal;
        if (this_has_greater && !other_has_greater) return ClockOrder::HappensAfter;
        if (!this_has_greater && other_has_greater) return ClockOrder::HappensBefore;
        return ClockOrder::Concurrent;
    }

    // Serialization: [Count(u32)] [Len(u32)NodeID Val(u64)]...
    std::string serialize() const {
        std::string out;
        uint32_t size = static_cast<uint32_t>(clock.size());
        out.append(reinterpret_cast<const char*>(&size), 4);
        
        for (const auto& [node, count] : clock) {
            uint32_t nlen = static_cast<uint32_t>(node.size());
            out.append(reinterpret_cast<const char*>(&nlen), 4);
            out.append(node);
            out.append(reinterpret_cast<const char*>(&count), 8);
        }
        return out;
    }

    static VectorClock deserialize(std::string_view in) {
        VectorClock vc;
        if (in.size() < 4) return vc;
        
        const char* ptr = in.data();
        uint32_t size = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += 4;

        for (uint32_t i = 0; i < size; ++i) {
            if (ptr + 4 > in.data() + in.size()) break;
            uint32_t nlen = *reinterpret_cast<const uint32_t*>(ptr);
            ptr += 4;
            
            if (ptr + nlen + 8 > in.data() + in.size()) break;
            std::string node(ptr, nlen);
            ptr += nlen;
            
            uint64_t count = *reinterpret_cast<const uint64_t*>(ptr);
            ptr += 8;
            
            vc.clock[node] = count;
        }
        return vc;
    }
};