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

}  // namespace mako

#endif  // _MAKO_TABLE_REGISTRY_H_
