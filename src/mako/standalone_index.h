#pragma once

// Registry-free mbta index factory (see standalone_index.cc for why system
// tables must not go through abstract_db::open_index). This header is
// deliberately free of module imports and rrr/masstree includes so BOTH sides
// can use it: the masstree-heavy factory TU and the rrr-side bootstrap.

#include <string>

class FullOrderedIndex;
class abstract_db;

namespace mako {

// A process-lifetime (leaked) mbta index with a FIXED table id, outside the
// per-process open_index registry — consumes no table-id slot on any node.
::FullOrderedIndex* make_standalone_index(const std::string& name, long table_id);

// Claim the PREALLOCATED engine index at `table_id` for an on-demand
// warehouse-migration handle: set its name (what the staging fence reads at
// write time) and register it with the helper servers' id map, then return
// it. is_remote was already set by id-window ownership at preallocation, so
// the same call yields the destination's adopted LOCAL index and a departed
// warehouse's remote PROXY, depending on whose window `table_id` sits in.
// Lives here because the concrete index type is masstree-TU-only.
::FullOrderedIndex* claim_preallocated_index(abstract_db* db, int table_id,
                                             const std::string& name);

}  // namespace mako
