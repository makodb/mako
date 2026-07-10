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

#include <unistd.h>   // usleep (drain poll)

#include <atomic>

import cluster;   // janus::get_migration_guard()

namespace mako {
namespace {
// Depth, not flag: the data plane's ops can nest (drop_range -> remove).
thread_local int g_fence_bypass_depth = 0;
// Staged-writer accounting (the drain's ground truth): txns that have staged
// at least one write and not yet stopped. Registered BEFORE the fence check,
// so once a fence is up the count can only drain.
std::atomic<long> g_staged_txns{0};
thread_local bool g_this_txn_counted = false;
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

bool migration_stage_fenced(const std::string& table, const char* key, size_t len) {
    if (g_fence_bypass_depth > 0) return false;   // data-plane writes: uncounted, unfenced
    const bool counted_here = !g_this_txn_counted;
    if (counted_here) {
        g_this_txn_counted = true;
        g_staged_txns.fetch_add(1, std::memory_order_acq_rel);
    }
    if (janus::get_migration_guard().is_frozen(table, std::string(key, len))) {
        if (counted_here) {
            // This call created the registration and the write never stages:
            // roll it back (an earlier-staged txn keeps its count until stop).
            g_this_txn_counted = false;
            g_staged_txns.fetch_sub(1, std::memory_order_acq_rel);
        }
        return true;
    }
    return false;
}

void migration_fence_txn_done() {
    if (g_this_txn_counted) {
        g_this_txn_counted = false;
        g_staged_txns.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool migration_fence_drain_writes(int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (g_staged_txns.load(std::memory_order_acquire) == 0) return true;
        usleep(10 * 1000);
    }
    return g_staged_txns.load(std::memory_order_acquire) == 0;
}

}  // namespace mako
