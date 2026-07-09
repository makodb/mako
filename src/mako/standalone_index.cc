// Standalone (registry-free) mbta index factory — the ONLY masstree-heavy TU
// behind the cluster runtime's storage. System tables (__mako_config__,
// __mako_kv__) must NOT go through abstract_db::open_index: table ids are
// assigned per-process in open order, so bootstrap-opened tables shift every
// workload table id after them — and asymmetrically across shards (shard 0
// opens the config store, others do not), which breaks cross-shard table-id
// arithmetic (observed live: ~100% remote aborts on both shards). A standalone
// mbta_table with a FIXED id consumes no registry slot on any node.
//
// TU discipline: mbta_wrapper.hh only — no import cluster, no rrr headers
// (either pairing compiles pathologically with the masstree forest).

#include "standalone_index.h"

#include <stdlib.h>

#include "benchmarks/bench.h"        // canonical include order for mbta TUs
#include "storage/mbta_wrapper.hh"   // mbta_index_build

import std;

namespace mako {

::FullOrderedIndex* make_standalone_index(const std::string& name, long table_id) {
    // Leaked: process-lifetime, like the bootstrap's other singletons.
    return mbta_index_build(name, table_id, /*is_remote=*/false);
}

}  // namespace mako
