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
#include <map>
#include <tuple>
#include <string_view>
#include <cstdlib>
#include <cstdint>
#include <utility>
#include <rusty/mutex.hpp>
#include <rusty/arc.hpp>
#include <rusty/option.hpp>

class FullOrderedIndex;

namespace mako {

// Immutable after publication; aliases never infer a physical table-id window.
struct NativeTableBinding {
    uint64_t table;
    rusty::Option<std::string> fixed_coordinate;
    uint32_t kind; // authoritative catalog: governed=0, replicated=1, static=2
};

/**
 * @brief Global registry for table_id ↔ table_name mappings.
 *
 * Used by ShardClient to look up table names for policy-based routing.
 * Tables are registered during open_index() in mbta_wrapper.
 */
class TableRegistry {
private:
    struct State {
        std::unordered_map<int, std::string> id_to_name;
        std::unordered_map<int, int> id_to_owner;
        std::unordered_map<int, bool> id_is_physical;
        std::unordered_map<int, FullOrderedIndex*> handles;
        std::unordered_map<std::string, int> name_to_id;
        std::unordered_map<std::string, rusty::Arc<NativeTableBinding>> native;
        std::map<std::tuple<uint64_t, int, bool>,
                 std::map<std::string, int, std::less<>>> physical;
    };
    mutable rusty::Mutex<State> state_{State{}};

    // @unsafe - catalog/index bridge; only registration mutates this reverse map.
    static void index_binding(State& state, int id, int owner,
                              const NativeTableBinding& binding) {
        const bool fixed = binding.fixed_coordinate.is_some();
        const std::string key = fixed ? binding.fixed_coordinate.as_ref().unwrap() : std::string();
        auto& aliases = state.physical[{binding.table, owner, fixed}];
        auto old = aliases.find(key);
        if (old != aliases.end() && old->second != id) std::abort();
        aliases.emplace(key, id);
    }

public:

    // @unsafe - only actual physical indexes are returned, never remote proxies.
    rusty::Option<FullOrderedIndex*> physical_handle(int id) const {
        auto state = state_.lock().unwrap();
        auto physical = state->id_is_physical.find(id);
        auto handle = state->handles.find(id);
        if (physical == state->id_is_physical.end() || !physical->second
            || handle == state->handles.end()) return rusty::None;
        return rusty::Some(handle->second);
    }

    // @unsafe - immutable canonical binding and handle lookup under one mutex.
    rusty::Option<FullOrderedIndex*> native_handle(uint64_t table, int owner, bool fixed,
                                                  std::string_view coordinate) const {
        auto state = state_.lock().unwrap();
        auto group = state->physical.find({table, owner, fixed});
        if (group == state->physical.end()) return rusty::None;
        auto found = group->second.find(fixed ? coordinate : std::string_view());
        if (found == group->second.end()) return rusty::None;
        auto handle = state->handles.find(found->second);
        if (handle == state->handles.end()) return rusty::None;
        return rusty::Some(handle->second);
    }
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
    // @unsafe - legacy string/map registry surgery under a data-owning mutex.
    void register_table(int table_id, const std::string& table_name, int owner,
                        bool physical = true, FullOrderedIndex* handle = nullptr) {
        auto state = state_.lock().unwrap();
        auto previous = state->id_to_name.find(table_id);
        if (previous != state->id_to_name.end() && state->native.count(previous->second)) {
            if (previous->second != table_name || state->id_to_owner.at(table_id) != owner
                || state->id_is_physical.at(table_id) != physical)
                std::abort(); // a published canonical address cannot be rebound
        }
        if (handle && physical) {
            auto old = state->handles.find(table_id);
            if (old != state->handles.end() && old->second != handle
                && state->id_is_physical.at(table_id))
                std::abort(); // distinct physical indexes cannot claim one local address
            state->handles[table_id] = handle;
        }
        state->id_to_name[table_id] = table_name;
        state->name_to_id.emplace(table_name, table_id);
        state->id_to_owner[table_id] = owner;
        state->id_is_physical[table_id] = physical;
        auto binding = state->native.find(table_name);
        if (physical && binding != state->native.end())
            index_binding(*state, table_id, owner, *binding->second);
    }

    // @unsafe - bootstrap publishes an immutable canonical binding exactly once.
    bool bind_native(const std::string& name, uint64_t table,
                     rusty::Option<std::string> coordinate, uint32_t kind = 0) {
        if (kind > 2) return false;
        auto state = state_.lock().unwrap();
        auto old = state->native.find(name);
        if (old != state->native.end())
            return false;
        auto binding = rusty::Arc<NativeTableBinding>::make(
            NativeTableBinding{table, std::move(coordinate), kind});
        for (const auto& entry : state->id_to_name)
            if (entry.second == name && state->id_is_physical.at(entry.first))
                index_binding(*state, entry.first, state->id_to_owner.at(entry.first), *binding);
        state->native.emplace(name, std::move(binding));
        return true;
    }

    // @unsafe - immutable owner keeps a coordinate alive after releasing the lock.
    rusty::Option<rusty::Arc<NativeTableBinding>> native_binding(int id) const {
        auto state = state_.lock().unwrap();
        auto name = state->id_to_name.find(id);
        if (name == state->id_to_name.end()) return rusty::None;
        auto binding = state->native.find(name->second);
        if (binding == state->native.end()) return rusty::None;
        return rusty::Some(binding->second.clone());
    }

    // @unsafe - exact local physical lookup by distributed catalog identity.
    rusty::Option<int> native_table(uint64_t table, int owner, bool fixed,
                                   std::string_view coordinate) const {
        auto state = state_.lock().unwrap();
        auto group = state->physical.find({table, owner, fixed});
        if (group == state->physical.end()) return rusty::None;
        auto found = group->second.find(fixed ? coordinate : std::string_view());
        if (found == group->second.end()) return rusty::None;
        return rusty::Some(found->second);
    }

    /**
     * Look up table_name from table_id.
     *
     * @param table_id The numeric table ID
     * @return Option containing table_name if found, None otherwise
     */
    // @safe - Read-only under lock
    rusty::Option<std::string> get_table_name(int table_id) const {
        auto state = state_.lock().unwrap();
        auto it = state->id_to_name.find(table_id);
        if (it != state->id_to_name.end()) {
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
        auto state = state_.lock().unwrap();
        auto it = state->name_to_id.find(table_name);
        if (it != state->name_to_id.end()) {
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
        auto state = state_.lock().unwrap();
        return state->id_to_name.find(table_id) != state->id_to_name.end();
    }

    /**
     * Get the number of registered tables.
     *
     * @return Number of registered table_id → table_name mappings
     */
    // @safe - Read-only under lock
    size_t size() const {
        auto state = state_.lock().unwrap();
        return state->id_to_name.size();
    }

    /**
     * Clear all registrations.
     */
    // @safe - Modifies internal state under lock
    void clear() {
        auto state = state_.lock().unwrap();
        state->id_to_name.clear();
        state->name_to_id.clear();
        state->native.clear();
        state->id_to_owner.clear();
        state->id_is_physical.clear();
        state->physical.clear();
        state->handles.clear();
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
