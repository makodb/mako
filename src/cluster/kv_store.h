#pragma once

#include <string>

namespace janus {

/**
 * KvStore — the minimal key-value port the cluster metadata component
 * depends on. Deliberately tiny (three methods, string keys and raw
 * byte values, stdlib only) so `cluster` compiles and unit-tests with
 * NO dependency on the storage engine.
 *
 * This is a decoupling seam, not a parallel substrate. In production
 * the port is bound to Mako's unified FullOrderedIndex — the
 * __mako_config__ system table on shard 0 — via the OrderedIndexKvStore
 * adapter (src/mako/ordered_index_kv_store.h), which lives on the mako
 * side so cluster never pulls storage headers. Tests bind an in-memory
 * fake (in_memory_kv_store.h). Either way the metadata really lives in
 * the unified store; cluster just talks to it through this narrow port.
 */
// @safe - pure abstract interface
class KvStore {
public:
    virtual ~KvStore() = default;

    // Point read: returns true and fills *out on hit, false on miss.
    virtual bool get(const std::string& key, std::string* out) = 0;

    // Point write: blind overwrite (raw bytes).
    virtual void put(const std::string& key, const std::string& value) = 0;

    // Point delete: no-op if the key is absent.
    virtual void remove(const std::string& key) = 0;
};

}  // namespace janus
