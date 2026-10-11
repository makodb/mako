#include <cstring>
#include "mako/benchmarks/tpcc_sharding.h"
#include "mako/storage/abstract_db.h"

namespace mako {

// @unsafe - explicit configuration query; absence does not invent a native owner.
bool tpcc_native_warehouses_enabled() {
    return mako_sharding_enabled() != 0;
}

// @unsafe - borrowed logical-name bytes cross the immutable native catalog ABI.
TpccTableIdentity tpcc_table_identity(const char* logical_name) {
    if (!tpcc_native_warehouses_enabled()) return {0, 2};
    TpccTableIdentity identity{};
    const MakoShardBytes name{reinterpret_cast<const uint8_t*>(logical_name),
                              std::strlen(logical_name)};
    if (mako_sharding_table_kind(name, &identity.table, &identity.kind) != MAKO_SHARD_OK)
        throw abstract_db::abstract_abort_exception();
    return identity;
}

// @unsafe - Rust selects a process-lifetime engine-owned handle under its mutex.
// Neither the snapshot lookup nor this pointer conversion authorizes byte access:
// the engine takes an exact native owner+epoch lease before using the handle.
FullOrderedIndex* tpcc_resolve_warehouse(uint64_t table, uint32_t global_warehouse,
                                       uint32_t participant) {
    uintptr_t handle = 0;
    MakoShardGrant grant{};
    if (mako_sharding_warehouse_resolve(table, global_warehouse, participant,
                                        &handle, &grant) != MAKO_SHARD_OK || !handle)
        throw abstract_db::abstract_abort_exception();
    return reinterpret_cast<FullOrderedIndex*>(handle);
}

} // namespace mako
