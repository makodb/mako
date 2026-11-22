#include <cstring>
#include "mako/benchmarks/tpcc_sharding.h"
#include "mako/storage/abstract_db.h"
#include "mako/lib/sharding_admission.h"

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

// @unsafe - borrows the captured transaction's handle, or resolves without a lease
// for setup/no-open-context access. Actual storage access still requires admission.
FullOrderedIndex* tpcc_resolve_warehouse(ShardingPreparedRoute& route, uint32_t participant) {
    return sharding_warehouse_handle(route, participant);
}

} // namespace mako
