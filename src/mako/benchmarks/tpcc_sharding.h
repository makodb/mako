#ifndef _MAKO_TPCC_SHARDING_H_
#define _MAKO_TPCC_SHARDING_H_

#include <stdint.h>
#include "cluster/native_sharding.h"

class FullOrderedIndex;

namespace mako {

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

// Resolve on every workload access. The opaque engine handle is borrowed until
// process shutdown; native lease admission still checks the selected incarnation
// before accessing its bytes. The caller supplies its actual participant, not
// the warehouse's original static owner.
// @unsafe - native synchronized handle directory and engine-owned pointer boundary.
FullOrderedIndex* tpcc_resolve_warehouse(uint64_t table, uint32_t global_warehouse,
                                       uint32_t participant);

} // namespace mako
#endif
