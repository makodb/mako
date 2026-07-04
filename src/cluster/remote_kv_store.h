#pragma once

#include "kv_store.h"

#include <functional>
#include <string>

namespace janus {

/**
 * RemoteKvStore — the KvStore a non-shard-0 node uses to read cluster
 * config. Shard 0's leader owns the __mako_config__ table; every other
 * node discovers topology by reading those keys *from shard 0*. This
 * adapter turns a KvStore get() into whatever remote read the caller
 * injects — in production a ReadConfigKey RPC to shard 0's leader; in
 * tests a closure over an in-memory store standing in for shard 0.
 *
 * Transport-agnostic on purpose: it depends only on the injected
 * ReadFn, so it stays in cluster/ (no RPC-layer dependency) and is
 * unit-testable standalone. The real RPC wiring lives on the mako side
 * and is passed in as the ReadFn.
 *
 * Read-only: config mutation is shard-0-only (an operator/admin path
 * drives ConfigManager on shard 0's leader). A read-only consumer that
 * tries to Put/Delete is a bug, so we fail loudly (return / no-op)
 * rather than silently diverge from shard 0. Because ConfigManager and
 * ConfigWatcher only call get() when *loading* config, wrapping a
 * ConfigManager around a RemoteKvStore makes
 * ClusterConfig::LoadFromConfigManager work transparently against
 * shard 0 with no changes to either class.
 */
// @safe - delegates reads to an injected function; refuses writes.
class RemoteKvStore : public KvStore {
public:
    // read_fn(key, out_value) -> found. In production this issues a
    // ReadConfigKey RPC to shard 0's leader and fills *out on hit.
    using ReadFn = std::function<bool(const std::string& key,
                                      std::string* out_value)>;

    explicit RemoteKvStore(ReadFn read_fn) : read_fn_(std::move(read_fn)) {}

    // @safe - delegates to the injected reader.
    bool get(const std::string& key, std::string* out) override {
        if (!read_fn_ || out == nullptr) return false;
        return read_fn_(key, out);
    }

    // Read-only consumer: writes are not permitted (no-op).
    void put(const std::string&, const std::string&) override {}
    void remove(const std::string&) override {}

private:
    ReadFn read_fn_;
};

}  // namespace janus
