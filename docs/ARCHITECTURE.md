# Architecture Overview

ZenithDB is a distributed, causal-consistent key-value store. It separates the **Safety Layer** (CRDTs) from the **Performance Policy** (Replication Strategy), allowing an agent to dynamically tune consistency without risking data divergence.

## 1. System Topology

The system uses a leaderless, shared-nothing architecture where every node can accept writes. Nodes communicate via a mesh network for replication and gossip.

┌──────────────────┐ ┌──────────────────┐ │ Client │ │ Client │ └────────┬─────────┘ └────────┬─────────┘ │ │ ▼ ▼ ┌──────────────────┐ ┌──────────────────┐ │ Node A │◄────────►│ Node B │ │ [Learned Agent] │ Gossip │ [Learned Agent] │ │ [Storage Engine] │ │ [Storage Engine] │ └────────┬─────────┘ └────────┬─────────┘ │ │ ▼ ▼ [Virtual FS] [Virtual FS] (Disk / S3) (Disk / S3)



## 2. Key Features & Optimizations

ZenithDB combines high-level distributed safety with low-level storage optimizations.

### Distributed Features
* **Leaderless Replication:** No single point of failure; writes can be accepted by any node.
* **CRDTs (Conflict-Free Replicated Data Types):** Guarantees strong eventual consistency mathematically.
* **Hybrid Replication:** Supports both **Eager** (push-to-replica) and **Lazy** (anti-entropy gossip) strategies.
* **Virtual File System (VFS):** Decouples the engine from the OS, allowing operation on local disk, RAM (`MockEnv`), or cloud object storage (S3).

### Storage Engine Optimizations
* **LSM-Tree Architecture:** Optimized for high write throughput using MemTables and SSTables.
* **Bloom Filters:** Uses 10 bits/key with 7 hash functions to eliminate unnecessary disk lookups for non-existent keys.
* **Lock-Free Reads (RCU):** Uses Read-Copy-Update synchronization for snapshot isolation without blocking writers.
* **Memory-Mapped I/O:** Uses `mmap` (where supported) or buffered reads for zero-copy access to data blocks.
* **Sparse Indexing:** Keeps a sparse index of data blocks in memory to minimize metadata overhead.
* **Arena Allocation:** Uses a custom arena allocator for the SkipList to eliminate memory fragmentation and improve cache locality.

## 3. Core Components

### The Node (`src/node.cpp`)
The entry point for distributed operations. It wraps the local storage engine and handles:
-   **Peer Management:** Tracking cluster members.
-   **Replication:** Decides *how* to send data (Eager Push vs. Lazy Gossip).
-   **Anti-Entropy:** Periodically scans the local DB to push updates to peers.

### The Storage Engine (`src/db.cpp`)
A modified LSM-tree optimized for CRDTs.
-   **Write Path:** Writes are serialized `LWWRegister` objects (Value + Vector Clock).
-   **Read Path:** Deserializes raw bytes back into CRDTs.
-   **Compaction:** Unlike standard databases, compaction **merges** concurrent versions of a key instead of discarding older ones. This ensures no data is lost during conflict resolution.

## 4. Data Consistency (CRDTs)

We use **State-based CRDTs** to guarantee convergence.

* **Vector Clock:** Tracks causal history (`{NodeA: 1, NodeB: 5}`). Used to determine if one update happened before another.
* **LWW-Register (Last-Write-Wins):** The fundamental data type.
    * **Merge Logic:** If `Clock A > Clock B`, keep A. If concurrent, allow deterministic tie-breaking (lexicographical) while merging clock history to preserve causality.

## 5. Request Lifecycle

1.  **Client Write:** `NodeA->Put("key", "val")`
2.  **Local Commit:** Node A increments its Vector Clock, wraps "val" in a CRDT, and writes to WAL/MemTable.
3.  **Replication (Policy Decision):**
    * *Eager:* Immediately send `PUT` message to Node B.
    * *Lazy:* Wait for periodic gossip.
4.  **Remote Merge:** Node B receives the message, reads its local version of "key", calls `CRDT::Merge(local, incoming)`, and writes the result.