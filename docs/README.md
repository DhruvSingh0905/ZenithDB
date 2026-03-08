# ZenithDB

**A Distributed, CRDT-Based Key-Value Store for RL-Driven Replication Research.**

ZenithDB is a C++ storage engine that lets an embedded **Reinforcement Learning agent** dynamically tune replication strategies at runtime — switching between eager replication and lazy gossip per key-range based on observed latency, bandwidth, and staleness. Unlike databases that commit to a single consistency model (e.g., Raft or Dynamo), ZenithDB treats replication as a learnable control problem.

Data safety is guaranteed through **State-based CRDTs** (Vector Clocks + Last-Write-Wins Registers), so the RL agent can explore aggressively without risking divergence. The underlying storage engine is built on an LSM-tree with a custom merge-based compaction layer that preserves causal history rather than discarding conflicting writes.

---

## Key Features

### Distributed & Consistent
- **Leaderless Architecture:** A master-less, shared-nothing topology where any node can accept reads and writes.
- **Strong Eventual Consistency:** Implements **State-based CRDTs** (Vector Clocks + Last-Write-Wins Registers) to ensure that all nodes converge to the same state eventually, regardless of message ordering or network partitions.
- **Semantic Compaction:** The LSM-tree compaction engine was rewritten to *merge* conflicting versions of data rather than discarding them, preserving causal history during background operations.

### Storage Engine
- **LSM-Tree Backend:** High-throughput write engine using MemTables (SkipList) and SSTables (Sorted String Tables).
- **Decoupled Storage (VFS):** Implements an `Env` abstraction layer, allowing the engine to run on:
  - **POSIX Filesystem:** Standard local disk storage.
  - **MockEnv:** In-memory storage for deterministic simulation and testing.
  - **Cloud Object Storage:** (Future) Direct mapping to S3/GCS.
- **Performance Optimizations:**
  - **Bloom Filters:** 10 bits/key to minimize disk lookups for non-existent keys.
  - **Lock-Free Reads:** Uses RCU (Read-Copy-Update) for snapshot isolation.
  - **Memory-Mapped I/O:** Zero-copy access to data blocks.

### Research Capabilities
- **Hybrid Replication:** Supports two distinct replication modes that the agent can switch between:
  - **Eager Replication:** Immediate push-to-replica for low-latency reads.
  - **Lazy Gossip:** Background anti-entropy for bandwidth efficiency.
- **Instrumentation:** Built-in metrics for write rates, conflict rates, and latency to feed the RL reward function.

---

## Building and Running

### Prerequisites
- **C++ Compiler:** GCC 10+ or Clang 12+ (C++20 support required).
- **CMake:** Version 3.20 or higher.
- **GoogleTest:** Included via Git submodule.

### Build Instructions

```bash
# 1. Clone the repository
git clone https://github.com/yourusername/zenithdb.git
cd zenithdb

# 2. Configure the build
mkdir build && cd build
cmake ..

# 3. Compile (Release mode recommended for benchmarking)
make -j$(nproc)
```

---

## API Usage

ZenithDB provides a clean C++ API for simulating distributed clusters or running a single node.

### Basic Single-Node Usage

```cpp
#include "db.h"

// Initialize DB with a unique Node ID (for vector clocks)
ZenithDB db("./data_dir", "node_1");

// Write (implicitly adds local vector clock)
db.put("user:100", "Alice");

// Read
auto val = db.get("user:100");
if (val) {
    std::cout << "Value: " << *val << std::endl;
}
```

### Distributed Cluster Simulation

Simulate a networked cluster within a single process using `MockTransport` and `Node` wrappers.

```cpp
#include "node.h"
#include "transport.h"

int main() {
    // 1. Setup the simulated network
    MockTransport transport;

    // 2. Create Nodes (ID, Directory, Transport)
    auto nodeA = std::make_unique<Node>("A", "./db_A", &transport);
    auto nodeB = std::make_unique<Node>("B", "./db_B", &transport);

    // 3. Peer Discovery
    nodeA->AddPeer("B");
    nodeB->AddPeer("A");

    // 4. Client Write to Node A
    // Node A writes locally, then replicates to B (Eager strategy)
    nodeA->Put("key_1", "Value_X");

    // 5. Simulate Network Delivery
    transport.DeliverAll();

    // 6. Read from Node B (Converged)
    auto result = nodeB->Get("key_1");
    assert(*result == "Value_X");

    return 0;
}
```

---

## Architecture Details

### The Merge Pipeline

Unlike standard key-value stores that simply overwrite data on `PUT`, ZenithDB implements a read-repair merge pipeline.

1. An incoming write arrives with a payload consisting of a value and its associated vector clock.
2. The engine performs a local read to retrieve the existing vector clock for the key, if one exists.
3. A causal comparison is then executed:
   - If the incoming vector clock **strictly dominates** the local clock, the incoming value overwrites the local value.
   - If the local vector clock **dominates** the incoming clock, the write is ignored to ensure idempotency.
   - If the clocks are **concurrent**, a deterministic tie-break is applied (lexicographical ordering) and the vector clocks are merged.
4. The resulting winning value is persisted by appending it to the write-ahead log (WAL) and inserting it into the MemTable.

### File Format

The write-ahead log (WAL) is an append-only log of serialized CRDT entries.

SSTables consist of sorted blocks containing serialized records in the following format:

```
[VectorClock Length][VectorClock Bytes][Value]
```

For deeper discussion of system topology, compaction, and storage layout, refer to `docs/ARCHITECTURE.md`.

---

## Project Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Core engine and safety guarantees (LSM-tree, vector clocks, LWW registers, merge-based compaction) | ✅ Complete |
| Phase 2 | Foundational plumbing (node abstraction, message bus, gossip/eager replication) | ✅ Complete |
| Phase 3 | Adaptive intelligence layer (RL controller, bandit algorithms, reward function) | 🔄 In Progress |
| Phase 4 | Large-scale simulation with realistic traces for training and evaluation | 📋 Planned |

---

## License

This project is licensed under the [MIT License](LICENSE).