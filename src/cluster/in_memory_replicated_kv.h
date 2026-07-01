#pragma once

#include "cluster/replicated_kv.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace janus {

/**
 * InMemoryReplicatedKV — drop-in ReplicatedKV implementation for unit
 * tests. The production impl (ReplicatedDB) writes through Raft and
 * persists to RocksDB; this fake keeps everything in a std::map and
 * applies batches atomically under a mutex.
 *
 * From the caller's perspective the semantics match: Get returns
 * false when the key is absent, Batch applies all ops or none (no
 * partial visibility), and writes are durable for the lifetime of
 * the object.
 */
// @safe - In-memory map guarded by a std::mutex; no I/O.
class InMemoryReplicatedKV : public ReplicatedKV {
public:
    InMemoryReplicatedKV() = default;
    ~InMemoryReplicatedKV() override = default;

    bool Put(const std::string& key, const std::string& value) override {
        std::lock_guard<std::mutex> lg(mu_);
        store_[key] = value;
        return true;
    }

    bool Delete(const std::string& key) override {
        std::lock_guard<std::mutex> lg(mu_);
        store_.erase(key);
        return true;
    }

    bool Batch(const std::vector<KVOperation>& ops) override {
        std::lock_guard<std::mutex> lg(mu_);
        for (const auto& op : ops) {
            switch (op.op) {
                case ReplicatedDBOp::PUT:
                    store_[op.key] = op.value;
                    break;
                case ReplicatedDBOp::DELETE:
                    store_.erase(op.key);
                    break;
                case ReplicatedDBOp::BATCH:
                    // Nested batches aren't part of the production
                    // wire format; ignore for fidelity with the real
                    // ReplicatedDB which also doesn't apply them.
                    break;
            }
        }
        return true;
    }

    bool Get(const std::string& key, std::string* value) override {
        if (value == nullptr) return false;
        std::lock_guard<std::mutex> lg(mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return false;
        *value = it->second;
        return true;
    }

    // ---- Test helpers (not part of the ReplicatedKV interface) ----

    std::size_t size() const {
        std::lock_guard<std::mutex> lg(mu_);
        return store_.size();
    }

    bool contains(const std::string& key) const {
        std::lock_guard<std::mutex> lg(mu_);
        return store_.find(key) != store_.end();
    }

    void clear() {
        std::lock_guard<std::mutex> lg(mu_);
        store_.clear();
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, std::string> store_;
};

} // namespace janus
