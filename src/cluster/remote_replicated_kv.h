#pragma once

#include "replicated_kv.h"

#include <functional>
#include <string>
#include <vector>

namespace janus {

/**
 * RemoteReplicatedKV — a ReplicatedKV that reads from a *remote*
 * authoritative store (shard 0's ReplicatedDB) rather than a local one.
 *
 * This is how non-shard-0 nodes consume cluster config. Shard 0's
 * leader owns the __mako_config__ system table in its Raft-replicated
 * ReplicatedDB; every other node discovers topology by reading those
 * keys over the network. Because ConfigManager and ClusterConfig only
 * ever call Get-family methods when *loading* config (never Put/Delete
 * on a consumer), wrapping a ConfigManager around a RemoteReplicatedKV
 * makes ClusterConfig::LoadFromConfigManager work transparently against
 * shard 0 with no changes to either class.
 *
 * The actual transport is injected as a ReadFn so this type has no
 * dependency on the RPC layer: production wires it to a ReadConfigKey
 * RPC against shard 0's leader; tests wire it to a closure over an
 * in-memory store standing in for shard 0.
 *
 * Writes are refused. Config mutation is shard-0-only (an operator or
 * admin RPC drives ConfigManager on shard 0's leader). A read-only
 * consumer that tries to Put/Delete/Batch is a bug, so we fail loudly
 * (return false) rather than silently diverge from shard 0.
 */
// @safe - Delegates reads to an injected function; refuses writes.
class RemoteReplicatedKV : public ReplicatedKV {
public:
    // read_fn(key, out_value) -> found. In production this issues a
    // ReadConfigKey RPC to shard 0's leader and fills out_value on hit.
    using ReadFn = std::function<bool(const std::string& key,
                                      std::string* out_value)>;

    explicit RemoteReplicatedKV(ReadFn read_fn)
        : read_fn_(std::move(read_fn)) {}
    ~RemoteReplicatedKV() override = default;

    // @safe - Delegates to the injected reader.
    bool Get(const std::string& key, std::string* value) override {
        if (!read_fn_ || value == nullptr) return false;
        return read_fn_(key, value);
    }

    // Read-only consumer: writes are not permitted.
    bool Put(const std::string&, const std::string&) override { return false; }
    bool Delete(const std::string&) override { return false; }
    bool Batch(const std::vector<KVOperation>&) override { return false; }

private:
    ReadFn read_fn_;
};

}  // namespace janus
