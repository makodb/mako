module;
#include <string>
module cluster;
import :shard_router;

namespace mako {
// @safe - legacy engine table-slot addressing only; native governed routing
// never dispatches through this function. Invalid physical IDs fail closed.
int compute_shard_for_key(int table_id, const std::string&) {
    return table_id > 0 ? (table_id - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD : -1;
}
} // namespace mako
