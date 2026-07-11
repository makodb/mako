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

#include <stdio.h>    // drain-timeout diagnostics
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

// Per-TABLE staged-writer counters (see migration_fence.h): registrations
// land in the slot of their physical table's hash; slot collisions SHARE a
// counter, which only ever makes a drain more conservative. kSlots is a
// power of two well above the live table population (12 tables x warehouses
// per shard + system tables).
constexpr int kSlots = 64;
struct TableCounter {
    std::atomic<long> by_parity[2] = {{0}, {0}};
};
TableCounter g_staged[kSlots];

// FNV-1a over the table name.
inline int table_slot(const std::string& table) {
    uint64_t h = 1469598103934665603ull;
    for (char c : table) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return static_cast<int>(h & (kSlots - 1));
}

// Per-txn registration set: one entry per distinct table this txn staged
// into (TPC-C touches a handful; 16 covers every workload here). Overflow
// falls back to a global bucket every drain waits on -- conservative, never
// under-counts.
std::atomic<long> g_overflow_by_parity[2] = {{0}, {0}};
struct TxnTables {
    int n = 0;
    struct { int slot; unsigned parity; } e[16];
    bool overflow = false;
    unsigned overflow_parity = 0;
};
thread_local TxnTables g_txn_tables;
thread_local bool g_this_txn_counted = false;   // any registration this txn

// Drain-timeout forensics: one slot per registered thread recording what it
// registered on and when (monotonic seconds); cleared at close. Dumped by the
// drain on timeout so a stuck/leaked registration names itself. Fixed-size,
// index handed out once per thread -- no locks on the hot path beyond the
// existing atomics.
struct StagedSlot {
    std::atomic<long> since_s{-1};   // -1 = not registered
    char table[32] = {0};
    unsigned parity = 0;
};
StagedSlot g_slots[256];
std::atomic<int> g_next_slot{0};
thread_local int g_my_slot = -1;

long mono_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec);
}

int my_slot() {
    if (g_my_slot < 0) {
        g_my_slot = g_next_slot.fetch_add(1, std::memory_order_relaxed) & 255;
    }
    return g_my_slot;
}

void slot_open(const std::string& table, unsigned parity) {
    StagedSlot& s = g_slots[my_slot()];
    size_t n = table.size() < 31 ? table.size() : 31;
    for (size_t i = 0; i < n; i++) s.table[i] = table[i];
    s.table[n] = 0;
    s.parity = parity;
    s.since_s.store(mono_seconds(), std::memory_order_release);
}

void slot_close() {
    if (g_my_slot >= 0) g_slots[g_my_slot].since_s.store(-1, std::memory_order_release);
}
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
        // One-op kernels know their table only at the fence check; the RAII
        // spans internal retries but a single op touches ONE index, so it
        // counts in the overflow bucket (every drain waits on it) -- the ops
        // are ms-scale, unlike txns.
        g_overflow_by_parity[parity_].fetch_add(1, std::memory_order_acq_rel);
        slot_open("<one-op>", parity_);
    }
}
MigrationStagedWriter::~MigrationStagedWriter() {
    if (counted_) {
        g_overflow_by_parity[parity_].fetch_sub(1, std::memory_order_acq_rel);
        slot_close();
    }
}

bool migration_stage_fenced(const std::string& table, const char* key, size_t len) {
    if (g_fence_bypass_depth > 0) return false;   // data-plane writes: uncounted, unfenced
    // Register this txn on THIS table's counter (once per (txn, table)): the
    // drain for a fenced table waits only its own writers -- cross-shard 2PC
    // gives unrelated txns multi-second tails that must not stall it.
    const int slot = table_slot(table);
    TxnTables& tt = g_txn_tables;
    bool present = false;
    for (int i = 0; i < tt.n; i++) {
        if (tt.e[i].slot == slot) { present = true; break; }
    }
    bool appended_here = false;    // this call pushed tt.e[tt.n-1]
    bool overflowed_here = false;  // this call set the overflow bucket
    if (!present) {
        const unsigned parity =
            static_cast<unsigned>(g_fence_gen.load(std::memory_order_acquire) & 1);
        if (tt.n < 16) {
            tt.e[tt.n].slot = slot;
            tt.e[tt.n].parity = parity;
            tt.n++;
            g_staged[slot].by_parity[parity].fetch_add(1, std::memory_order_acq_rel);
            appended_here = true;
        } else if (!tt.overflow) {
            tt.overflow = true;
            tt.overflow_parity = parity;
            g_overflow_by_parity[parity].fetch_add(1, std::memory_order_acq_rel);
            overflowed_here = true;
        }
        g_this_txn_counted = true;
        slot_open(table, parity);
    }
    if (janus::get_migration_guard().is_frozen(table, std::string(key, len))) {
        // Roll back exactly what THIS call registered -- the write never
        // stages (an earlier-staged registration keeps its count until stop).
        if (appended_here) {
            tt.n--;
            g_staged[slot].by_parity[tt.e[tt.n].parity].fetch_sub(
                1, std::memory_order_acq_rel);
        } else if (overflowed_here) {
            tt.overflow = false;
            g_overflow_by_parity[tt.overflow_parity].fetch_sub(
                1, std::memory_order_acq_rel);
        }
        if (appended_here || overflowed_here) {
            g_this_txn_counted = (tt.n > 0 || tt.overflow);
        }
        return true;
    }
    return false;
}

void migration_fence_txn_done() {
    if (!g_this_txn_counted) return;
    TxnTables& tt = g_txn_tables;
    for (int i = 0; i < tt.n; i++) {
        g_staged[tt.e[i].slot].by_parity[tt.e[i].parity].fetch_sub(
            1, std::memory_order_acq_rel);
    }
    tt.n = 0;
    if (tt.overflow) {
        tt.overflow = false;
        g_overflow_by_parity[tt.overflow_parity].fetch_sub(1, std::memory_order_acq_rel);
    }
    g_this_txn_counted = false;
    slot_close();
}

namespace {
// Residual old-parity writers the drain still waits on: the fenced table's
// slot (or every slot for the table-agnostic "" fence) plus the overflow.
long drain_residual(const std::string& table, unsigned old_parity) {
    long r = g_overflow_by_parity[old_parity].load(std::memory_order_acquire);
    if (table.empty()) {
        for (int i = 0; i < kSlots; i++)
            r += g_staged[i].by_parity[old_parity].load(std::memory_order_acquire);
    } else {
        r += g_staged[table_slot(table)].by_parity[old_parity].load(
            std::memory_order_acquire);
    }
    return r;
}
}  // namespace

int migration_fence_drain_begin() {
    return static_cast<int>(g_fence_gen.fetch_add(1, std::memory_order_acq_rel) & 1);
}

bool migration_fence_drained(const std::string& table, int parity) {
    return drain_residual(table, static_cast<unsigned>(parity & 1)) == 0;
}

void migration_fence_dump_staged() {
    const long now = mono_seconds();
    for (int i = 0; i < 256; i++) {
        const long since = g_slots[i].since_s.load(std::memory_order_acquire);
        if (since >= 0) {
            // @unsafe { stderr diagnostics }
            fprintf(stderr, "  staged slot %d: table='%s' parity=%u held=%lds\n",
                    i, g_slots[i].table, g_slots[i].parity, now - since);
        }
    }
}

bool migration_fence_drain_writes_for(const std::string& table, int timeout_ms) {
    // Watermark: everything registered from here on lands in the new parity;
    // only same-table writers already holding the old parity are waited out.
    const unsigned old_parity =
        static_cast<unsigned>(migration_fence_drain_begin());
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (drain_residual(table, old_parity) == 0) return true;
        usleep(10 * 1000);
    }
    const long residual = drain_residual(table, old_parity);
    if (residual != 0) {
        // @unsafe { stderr diagnostics }
        fprintf(stderr,
                "migration drain TIMEOUT: table='%s' parity=%u residual=%ld gen=%llu\n",
                table.c_str(), old_parity, residual,
                static_cast<unsigned long long>(
                    g_fence_gen.load(std::memory_order_acquire)));
        const long now = mono_seconds();
        for (int i = 0; i < 256; i++) {
            const long since = g_slots[i].since_s.load(std::memory_order_acquire);
            if (since >= 0) {
                fprintf(stderr,
                        "  staged slot %d: table='%s' parity=%u held=%lds\n",
                        i, g_slots[i].table, g_slots[i].parity, now - since);
            }
        }
    }
    return residual == 0;
}

bool migration_fence_drain_writes(int timeout_ms) {
    return migration_fence_drain_writes_for(std::string(), timeout_ms);
}

int migration_owner_shard(int table_id, const char* key, size_t len) {
    return mako::governed_owner_shard(table_id, std::string(key, len));
}

}  // namespace mako
