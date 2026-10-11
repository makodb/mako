#ifndef MAKO_TPCC_WAREHOUSE_BRIDGE_H
#define MAKO_TPCC_WAREHOUSE_BRIDGE_H

#include <string>
#include <limits>
#include <rusty/option.hpp>
#include "benchmarks/tpcc_sharding.h"
#include "lib/table_registry.h"
#include "lib/sharding_leases.h"
#include "storage/abstract_db.h"

namespace mako {

// @unsafe - legacy registry string boundary. Published bindings are immutable;
// repeated setup of another in-process runner must agree with the first one.
inline void tpcc_bind_index(FullOrderedIndex* index, const std::string& name,
                            uint64_t table, rusty::Option<std::string> coordinate,
                            uint32_t kind) {
    auto& registry = get_table_registry();
    auto previous = registry.native_binding(index->get_table_id());
    if (previous.is_some()) {
        const auto& binding = previous.as_ref().unwrap();
        if (binding->table != table || binding->kind != kind
            || binding->fixed_coordinate.is_some() != coordinate.is_some()
            || (coordinate.is_some()
                && binding->fixed_coordinate.as_ref().unwrap() != coordinate.as_ref().unwrap()))
            throw abstract_db::abstract_abort_exception();
        return;
    }
    if (!registry.bind_native(name, table, std::move(coordinate), kind))
        throw abstract_db::abstract_abort_exception();
}

// @unsafe - sole opaque engine opener. Rust owns which handle is needed and
// serializes calls; the engine owns each result until shutdown. Physical owner
// is part of the name and allocation, so source cleanup cannot touch destination
// bytes in multi-shard single-process mode. A proxy is allocated on its ORIGIN
// and omitted from the reverse physical index; no destination table-id arithmetic.
inline uint32_t tpcc_open_warehouse(void* context, uint64_t table, uint32_t warehouse,
                                    uint32_t owner, uint32_t proxy, uintptr_t* handle) {
    try {
        auto* db = static_cast<abstract_db*>(context);
        const std::string name = "tpcc_native_" + std::to_string(table)
            + "_warehouse_" + std::to_string(warehouse)
            + "_owner_" + std::to_string(owner) + (proxy ? "_proxy" : "_bytes");
        auto* index = db->open_index(name, static_cast<int>(owner));
        auto& registry = get_table_registry();
        if (registry.native_binding(index->get_table_id()).is_none()) {
            if (proxy) sharding_mark_proxy(index);
            registry.register_table(index->get_table_id(), name,
                                     static_cast<int>(owner), proxy == 0, index);
        }
        if (index->get_is_remote() != (proxy != 0)) return MAKO_SHARD_INVALID;
        // Wire-coordinate serialization only; Rust owns routing and selection.
        const char bytes[4] = {static_cast<char>(warehouse >> 24),
            static_cast<char>(warehouse >> 16), static_cast<char>(warehouse >> 8),
            static_cast<char>(warehouse)};
        tpcc_bind_index(index, name, table, rusty::Some(std::string(bytes, 4)), 0);
        *handle = reinterpret_cast<uintptr_t>(index);
        return MAKO_SHARD_OK;
    } catch (...) {
        // No exception may cross the native Rust callback boundary.
        return MAKO_SHARD_IO;
    }
}

// @unsafe - setup-only FFI and process-lifetime database borrow. The native
// catalog is configured before load, while participant activation follows load.
inline void tpcc_initialize_native(abstract_db* db, uint32_t participant,
                                    size_t warehouses_per_shard, size_t total,
                                    bool micro) {
    if (!tpcc_native_warehouses_enabled()) return;
    if (warehouses_per_shard > std::numeric_limits<uint32_t>::max()
        || total > std::numeric_limits<uint32_t>::max())
        throw abstract_db::abstract_abort_exception();
    if (mako_sharding_catalog_tpcc(micro ? 1 : 0) != MAKO_SHARD_OK
        || mako_sharding_warehouse_init(warehouses_per_shard, total) != MAKO_SHARD_OK
        || mako_sharding_warehouse_opener(participant, db, tpcc_open_warehouse) != MAKO_SHARD_OK)
        throw abstract_db::abstract_abort_exception();
}

// @unsafe - physical local materialization is independent of the published
// route. The migration data plane uses this same FFI before destination copy.
inline FullOrderedIndex* tpcc_local_warehouse(uint64_t table, uint32_t warehouse,
                                              uint32_t owner) {
    uintptr_t handle = 0;
    if (mako_sharding_warehouse_local(table, warehouse, owner, &handle) != MAKO_SHARD_OK
        || !handle) throw abstract_db::abstract_abort_exception();
    return reinterpret_cast<FullOrderedIndex*>(handle);
}

// @unsafe - explicit native catalog exemption, never an unbound-table bypass.
inline void tpcc_bind_static_index(FullOrderedIndex* index, TpccTableIdentity identity) {
    if (!tpcc_native_warehouses_enabled() || index == nullptr) return;
    auto name = get_table_registry().get_table_name(index->get_table_id());
    if (name.is_none() || identity.kind == 0)
        throw abstract_db::abstract_abort_exception();
    tpcc_bind_index(index, name.as_ref().unwrap(), identity.table, rusty::None, identity.kind);
}

} // namespace mako
#endif
