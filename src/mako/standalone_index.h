#pragma once

// Registry-free mbta index factory (see standalone_index.cc for why system
// tables must not go through abstract_db::open_index). This header is
// deliberately free of module imports and rrr/masstree includes so BOTH sides
// can use it: the masstree-heavy factory TU and the rrr-side bootstrap.

#include <string>

class FullOrderedIndex;

namespace mako {

// A process-lifetime (leaked) mbta index with a FIXED table id, outside the
// per-process open_index registry — consumes no table-id slot on any node.
::FullOrderedIndex* make_standalone_index(const std::string& name, long table_id);

}  // namespace mako
