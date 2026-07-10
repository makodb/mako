/**
 * @file table_registry.h
 * @brief Global registry for table_id ↔ table_name mappings.
 *
 * This registry enables policy-based shard routing by allowing
 * ShardClient to look up table names from table IDs.
 *
 * Thread-safety: All methods are thread-safe via internal synchronization.
 */

#ifndef _MAKO_TABLE_REGISTRY_H_
#define _MAKO_TABLE_REGISTRY_H_

#include <string>
#include <unordered_map>
#include <mutex>
#include <rusty/option.hpp>

namespace mako {

/**
 * @brief Global registry for table_id ↔ table_name mappings.
 *
 * Used by ShardClient to look up table names for policy-based routing.
 * Tables are registered during open_index() in mbta_wrapper.
 */
class TableRegistry {
private:
    // @safe - Maps table_id to table_name
    std::unordered_map<int, std::string> id_to_name_;

    // @safe - Maps table_name to table_id (first registered)
    std::unordered_map<std::string, int> name_to_id_;

    // @safe - Routing alias: physical index -> (logical table, fixed routing key).
    // TPC-C opens one physical index per warehouse per table ("customer_0",
    // "customer_remote_5", ...), so the index identity -- not the key bytes,
    // which carry the shard-LOCAL warehouse id -- names the global warehouse.
    // The alias lets the router treat all those indexes as one logical table
    // ("customer") partitioned by an encoded global-warehouse routing key,
    // which is CONSTANT per index. Granularity note: an index spanning
    // several warehouses (nthreads < warehouses) gets its first warehouse's
    // key; routing granularity equals physical-index granularity by design.
    std::unordered_map<int, std::pair<std::string, std::string>> id_to_route_;

    // @safe - Mutex for thread-safe access
    mutable std::mutex mutex_;

public:
    // @safe - Default constructor
    TableRegistry() = default;

    // Non-copyable (has mutex)
    TableRegistry(const TableRegistry&) = delete;
    TableRegistry& operator=(const TableRegistry&) = delete;

    /**
     * Register a table_id → table_name mapping.
     *
     * @param table_id The numeric table ID
     * @param table_name The string table name
     */
    // @safe - Modifies internal state under lock
    void register_table(int table_id, const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        id_to_name_[table_id] = table_name;
        // Only store first registration for name_to_id
        // (same table may be registered on multiple shards with different IDs)
        if (name_to_id_.find(table_name) == name_to_id_.end()) {
            name_to_id_[table_name] = table_id;
        }
    }

    /**
     * Look up table_name from table_id.
     *
     * @param table_id The numeric table ID
     * @return Option containing table_name if found, None otherwise
     */
    // @safe - Read-only under lock
    rusty::Option<std::string> get_table_name(int table_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = id_to_name_.find(table_id);
        if (it != id_to_name_.end()) {
            return rusty::Some(it->second);
        }
        return rusty::None;
    }

    /**
     * Look up table_id from table_name.
     * Returns the first registered ID for this table name.
     *
     * @param table_name The string table name
     * @return Option containing table_id if found, None otherwise
     */
    // @safe - Read-only under lock
    rusty::Option<int> get_table_id(const std::string& table_name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = name_to_id_.find(table_name);
        if (it != name_to_id_.end()) {
            return rusty::Some(it->second);
        }
        return rusty::None;
    }

    /**
     * Register a routing alias for a physical index: the logical table it
     * belongs to plus its fixed routing key (encoded global warehouse for
     * TPC-C). The router prefers this over the physical name when the
     * ClusterConfig governs `route_table` (see shard_router.cc).
     */
    // @safe - Modifies internal state under lock
    void register_route(int table_id, const std::string& route_table,
                        const std::string& route_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        id_to_route_[table_id] = std::make_pair(route_table, route_key);
    }

    /**
     * Look up the routing alias's logical table for a table_id.
     */
    // @safe - Read-only under lock
    rusty::Option<std::string> get_route_table(int table_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = id_to_route_.find(table_id);
        if (it != id_to_route_.end()) {
            return rusty::Some(it->second.first);
        }
        return rusty::None;
    }

    /**
     * Look up the routing alias's fixed routing key for a table_id.
     */
    // @safe - Read-only under lock
    rusty::Option<std::string> get_route_key(int table_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = id_to_route_.find(table_id);
        if (it != id_to_route_.end()) {
            return rusty::Some(it->second.second);
        }
        return rusty::None;
    }

    /**
     * Check if a table_id is registered.
     *
     * @param table_id The numeric table ID
     * @return true if registered, false otherwise
     */
    // @safe - Read-only under lock
    bool has_table(int table_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return id_to_name_.find(table_id) != id_to_name_.end();
    }

    /**
     * Get the number of registered tables.
     *
     * @return Number of registered table_id → table_name mappings
     */
    // @safe - Read-only under lock
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return id_to_name_.size();
    }

    /**
     * Clear all registrations.
     */
    // @safe - Modifies internal state under lock
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        id_to_name_.clear();
        name_to_id_.clear();
        id_to_route_.clear();
    }
};

/**
 * Get the global table registry instance.
 * Thread-safe via internal synchronization.
 */
// @safe - Returns reference to static instance
inline TableRegistry& get_table_registry() {
    static TableRegistry instance;
    return instance;
}

/**
 * Fixed 4-byte big-endian encoding of a global warehouse id: the routing-key
 * format for warehouse-partitioned logical tables. Big-endian makes
 * lexicographic order over keys equal numeric order over warehouse ids, so
 * partition split points sit exactly at warehouse boundaries. Used for both
 * the registry's routing aliases and the seeded partition-table segments —
 * they must agree byte-for-byte.
 */
// @safe - Pure function
inline std::string warehouse_route_key(int global_wid) {
    unsigned char b[4];
    b[0] = static_cast<unsigned char>((global_wid >> 24) & 0xff);
    b[1] = static_cast<unsigned char>((global_wid >> 16) & 0xff);
    b[2] = static_cast<unsigned char>((global_wid >> 8) & 0xff);
    b[3] = static_cast<unsigned char>(global_wid & 0xff);
    return std::string(reinterpret_cast<const char*>(b), 4);
}

}  // namespace mako

#endif  // _MAKO_TABLE_REGISTRY_H_
