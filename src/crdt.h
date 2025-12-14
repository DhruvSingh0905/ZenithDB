// src/crdt.h
#pragma once
#include "vector_clock.h"
#include <string>

// Abstract CRDT Interface
class CRDTValue {
public:
    virtual ~CRDTValue() = default;
    virtual void merge(const CRDTValue& other) = 0;
    virtual std::string serialize() const = 0;
};

// Concrete Implementation: Last-Write-Wins Register
class LWWRegister : public CRDTValue {
public:
    std::string value;
    VectorClock clock;

    LWWRegister() = default;
    LWWRegister(std::string v, VectorClock c) : value(std::move(v)), clock(std::move(c)) {}

    void merge(const CRDTValue& other_base) override {
        const LWWRegister* other = dynamic_cast<const LWWRegister*>(&other_base);
        if (!other) return;

        ClockOrder order = clock.compare(other->clock);

        if (order == ClockOrder::HappensBefore) {
            // They are strictly newer, overwrite us completely
            value = other->value;
            clock = other->clock;
        } else if (order == ClockOrder::Concurrent) {
            // Concurrent: Use tie-breaker for value, but MERGE clocks to preserve history
            if (other->value > value) {
                value = other->value;
            }
            clock.merge(other->clock);
        } else {
            // We are newer or equal, just merge clocks (fast-forward them if needed)
            clock.merge(other->clock);
        }
    }

    // Wrapper serialization: [VectorClock serialized] [Value]
    std::string serialize() const override {
        std::string vc_bytes = clock.serialize();
        uint32_t vc_len = static_cast<uint32_t>(vc_bytes.size());
        
        std::string out;
        out.reserve(4 + vc_len + value.size());
        out.append(reinterpret_cast<const char*>(&vc_len), 4);
        out.append(vc_bytes);
        out.append(value);
        return out;
    }

    static LWWRegister deserialize(std::string_view in) {
        if (in.size() < 4) {
            return LWWRegister(std::string(in), VectorClock());
        }

        const char* ptr = in.data();
        uint32_t vc_len = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += 4;

        if (in.size() < 4 + vc_len) {
            return LWWRegister(std::string(in), VectorClock());
        }

        std::string_view vc_view(ptr, vc_len);
        VectorClock vc = VectorClock::deserialize(vc_view);
        ptr += vc_len;

        std::string val(ptr, in.size() - 4 - vc_len);
        return LWWRegister(std::move(val), std::move(vc));
    }
};