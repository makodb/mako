#ifndef _MAKO_TPCC_SHARDING_H_
#define _MAKO_TPCC_SHARDING_H_

#include <stdint.h>
#include "cluster/native_sharding.h"

class FullOrderedIndex;

namespace mako {

class ShardingPreparedRoute;

// Canonical catalog identity, never a node's physical table-id window. Disabled
// deployments and the immutable replicated item table do not use the warehouse
// directory. Unknown enabled tables are configuration errors, not static routes.
struct TpccTableIdentity {
    uint64_t table;
    uint32_t kind; // native catalog: governed=0, replicated=1, static=2
};
// @unsafe - native bootstrap/catalog FFI; may throw the legacy abort exception.
bool tpcc_native_warehouses_enabled();
TpccTableIdentity tpcc_table_identity(const char* logical_name);

// Open transactions reuse their first complete capture and selected handle.
// Outside a transaction this is non-admitting resolution (including loaders).
// Handles are engine-owned until shutdown; byte access always requires native
// admission. The participant is the caller, not the warehouse's original owner.
// @unsafe - native synchronized handle directory and engine-owned pointer boundary.
FullOrderedIndex* tpcc_resolve_warehouse(ShardingPreparedRoute& route, uint32_t participant);

} // namespace mako
#endif
