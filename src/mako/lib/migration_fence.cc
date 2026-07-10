// Migration write-fence bridge (see migration_fence.h). The only TU that both
// the storage staging points reach (via plain function calls) and the cluster
// module's MigrationGuard lives in.
//
// v1 cost note: every fenced check takes the guard's uncontended mutex + a
// tiny vector scan (a migration touches a handful of ranges). At TPC-C's
// write rates that is ~0.1% ceiling; the live bed's throughput gate measures
// it. If it ever shows up, an atomic fast-path count can land HERE without
// touching mbta_wrapper.hh again.

#include "lib/migration_fence.h"

import cluster;   // janus::get_migration_guard()

namespace mako {
namespace {
// Depth, not flag: the data plane's ops can nest (drop_range -> remove).
thread_local int g_fence_bypass_depth = 0;
}

bool migration_write_fenced(const std::string& table, const char* key, size_t len) {
    if (g_fence_bypass_depth > 0) return false;
    return janus::get_migration_guard().is_frozen(table, std::string(key, len));
}

bool migration_read_moved(const std::string& table, const char* key, size_t len) {
    if (g_fence_bypass_depth > 0) return false;
    return janus::get_migration_guard().is_moved(table, std::string(key, len));
}

MigrationFenceBypass::MigrationFenceBypass() { ++g_fence_bypass_depth; }
MigrationFenceBypass::~MigrationFenceBypass() { --g_fence_bypass_depth; }

}  // namespace mako
