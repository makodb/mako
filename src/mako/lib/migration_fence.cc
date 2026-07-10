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
// Staged-writer accounting (the drain's ground truth), as a GENERATION
// WATERMARK: a plain "wait for zero staged writers" is a quiescence
// condition, and a busy shard (TPC-C at thousands of txns/s) never has zero
// writers in flight -- observed live as a 3s drain timeout aborting every
// migration. Instead each registration lands in the parity bucket of the
// current fence generation; the drain bumps the generation and waits only
// for the OLD parity to empty. Post-bump registrants (new parity) don't
// block it -- their fence check runs after registration and the fence is
// already up, so they cannot stage into the fenced range anyway; writers to
// other ranges sail on unimpeded. Two parities suffice: migrations are
// serialized (one master, admin mutex), and a straggler from generation g
// still holding its slot when generation g+2 drains merely makes that drain
// conservative.
std::atomic<uint64_t> g_fence_gen{0};
std::atomic<long> g_staged_by_parity[2] = {{0}, {0}};
thread_local bool g_this_txn_counted = false;
thread_local unsigned g_this_txn_parity = 0;
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

// Spans one logical non-txn write (all internal one-op retries): the counter
// covers the op from before its fence check until the final attempt's commit
// or abort, so the drain cannot pass while any such write is still able to
// land. Unlike the per-txn registration, Transaction::stop does NOT close
// this one -- the destructor does. Registers in the CURRENT fence
// generation's parity (see the watermark note above).
MigrationStagedWriter::MigrationStagedWriter()
    : counted_(g_fence_bypass_depth == 0) {
    if (counted_) {
        parity_ = static_cast<unsigned>(
            g_fence_gen.load(std::memory_order_acquire) & 1);
        g_staged_by_parity[parity_].fetch_add(1, std::memory_order_acq_rel);
    }
}
MigrationStagedWriter::~MigrationStagedWriter() {
    if (counted_) g_staged_by_parity[parity_].fetch_sub(1, std::memory_order_acq_rel);
}

bool migration_stage_fenced(const std::string& table, const char* key, size_t len) {
    if (g_fence_bypass_depth > 0) return false;   // data-plane writes: uncounted, unfenced
    const bool counted_here = !g_this_txn_counted;
    if (counted_here) {
        g_this_txn_counted = true;
        g_this_txn_parity =
            static_cast<unsigned>(g_fence_gen.load(std::memory_order_acquire) & 1);
        g_staged_by_parity[g_this_txn_parity].fetch_add(1, std::memory_order_acq_rel);
    }
    if (janus::get_migration_guard().is_frozen(table, std::string(key, len))) {
        if (counted_here) {
            // This call created the registration and the write never stages:
            // roll it back (an earlier-staged txn keeps its count until stop).
            g_this_txn_counted = false;
            g_staged_by_parity[g_this_txn_parity].fetch_sub(1, std::memory_order_acq_rel);
        }
        return true;
    }
    return false;
}

void migration_fence_txn_done() {
    if (g_this_txn_counted) {
        g_this_txn_counted = false;
        g_staged_by_parity[g_this_txn_parity].fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool migration_fence_drain_writes(int timeout_ms) {
    // Watermark: everything registered from here on lands in the new parity;
    // only writers already holding the old parity are waited out.
    const unsigned old_parity = static_cast<unsigned>(
        g_fence_gen.fetch_add(1, std::memory_order_acq_rel) & 1);
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (g_staged_by_parity[old_parity].load(std::memory_order_acquire) == 0)
            return true;
        usleep(10 * 1000);
    }
    return g_staged_by_parity[old_parity].load(std::memory_order_acquire) == 0;
}

int migration_owner_shard(int table_id, const char* key, size_t len) {
    return mako::governed_owner_shard(table_id, std::string(key, len));
}

}  // namespace mako
