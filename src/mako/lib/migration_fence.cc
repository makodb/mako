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
// migration. Instead each registration lands in the bucket of the current
// fence generation (gen & kGenMask); the drain bumps the generation and
// waits for every bucket EXCEPT its own bump's to empty. Post-bump
// registrants (current bucket) don't block it -- their fence check runs
// after registration and the fence is already up, so they cannot stage into
// the fenced range anyway; writers to other ranges sail on unimpeded.
//
// Why 16 buckets and not 2: with two parities, SEQUENTIAL drains alternate
// buckets -- warehouse's drain bumps gen 0->1 (waits parity 0), district's
// bumps 1->2 (waits parity 1) -- so a txn that registered at gen 0 and was
// still open at district's drain sat in parity 0, INVISIBLE to the parity-1
// wait, yet it was a pre-fence stager of district's table. Observed live as
// single-row one-update-lag divergences on every table after the first
// (dst d_ytd exactly one payment behind src, post-drain post-copy). Sixteen
// buckets mean a straggler is only aliased into the not-waited bucket after
// 16 intervening drain bumps while its txn stays open -- minutes at this
// system's drain cadence, versus ms-scale txns.
std::atomic<uint64_t> g_fence_gen{0};
constexpr unsigned kGenBuckets = 16;
constexpr unsigned kGenMask = kGenBuckets - 1;

// Per-TABLE staged-writer counters (see migration_fence.h): each live table
// name claims its own slot (open addressing on the 64-bit FNV, claims are
// permanent -- the physical-table population is static per process and well
// under kSlots). Identity must be EXACT, not modular: with hash-mod slots,
// customer_1 shared a slot with order_line_8 and stock_remote_2 (observed
// live), and a hot never-migrating table's perpetual churn kept the fenced
// table's drain from ever emptying under load. A genuine 64-bit collision
// (or slot exhaustion) degrades to sharing = conservative, never unsound.
constexpr int kSlots = 256;
struct TableCounter {
    std::atomic<uint64_t> name_hash{0};   // 0 = unclaimed
    std::atomic<long> by_gen[kGenBuckets] = {};
};
TableCounter g_staged[kSlots];

// FNV-1a over the table name.
inline uint64_t table_hash(const std::string& table) {
    uint64_t h = 1469598103934665603ull;
    for (char c : table) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h != 0 ? h : 1;   // 0 marks a free slot
}

inline int table_slot(const std::string& table) {
    const uint64_t h = table_hash(table);
    int i = static_cast<int>(h & (kSlots - 1));
    for (int probes = 0; probes < kSlots; probes++, i = (i + 1) & (kSlots - 1)) {
        uint64_t cur = g_staged[i].name_hash.load(std::memory_order_acquire);
        if (cur == h) return i;
        if (cur == 0) {
            uint64_t expected = 0;
            if (g_staged[i].name_hash.compare_exchange_strong(
                    expected, h, std::memory_order_acq_rel)) return i;
            if (expected == h) return i;   // lost the race to ourselves
        }
    }
    return 0;   // table saturated: share slot 0 (conservative)
}

// Per-txn registration set: one entry per distinct table this txn staged
// into (TPC-C touches a handful; 16 covers every workload here). Overflow
// falls back to a global bucket every drain waits on -- conservative, never
// under-counts.
std::atomic<long> g_overflow_by_gen[kGenBuckets] = {};
struct TxnTables {
    int n = 0;
    struct { int slot; unsigned bucket; } e[16];
    bool overflow = false;
    unsigned overflow_bucket = 0;
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
    unsigned bucket = 0;
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

void slot_open(const std::string& table, unsigned bucket) {
    StagedSlot& s = g_slots[my_slot()];
    size_t n = table.size() < 31 ? table.size() : 31;
    for (size_t i = 0; i < n; i++) s.table[i] = table[i];
    s.table[n] = 0;
    s.bucket = bucket;
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
// generation's bucket (see the watermark note above).
MigrationStagedWriter::MigrationStagedWriter()
    : counted_(g_fence_bypass_depth == 0) {
    if (counted_) {
        bucket_ = static_cast<unsigned>(
            g_fence_gen.load(std::memory_order_acquire) & kGenMask);
        // One-op kernels know their table only at the fence check; the RAII
        // spans internal retries but a single op touches ONE index, so it
        // counts in the overflow bucket (every drain waits on it) -- the ops
        // are ms-scale, unlike txns.
        g_overflow_by_gen[bucket_].fetch_add(1, std::memory_order_acq_rel);
        slot_open("<one-op>", bucket_);
    }
}
MigrationStagedWriter::~MigrationStagedWriter() {
    if (counted_) {
        g_overflow_by_gen[bucket_].fetch_sub(1, std::memory_order_acq_rel);
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
        const unsigned bucket = static_cast<unsigned>(
            g_fence_gen.load(std::memory_order_acquire) & kGenMask);
        if (tt.n < 16) {
            tt.e[tt.n].slot = slot;
            tt.e[tt.n].bucket = bucket;
            tt.n++;
            g_staged[slot].by_gen[bucket].fetch_add(1, std::memory_order_acq_rel);
            appended_here = true;
        } else if (!tt.overflow) {
            tt.overflow = true;
            tt.overflow_bucket = bucket;
            g_overflow_by_gen[bucket].fetch_add(1, std::memory_order_acq_rel);
            overflowed_here = true;
        }
        g_this_txn_counted = true;
        slot_open(table, bucket);
    }
    if (janus::get_migration_guard().is_frozen(table, std::string(key, len))) {
        // Roll back exactly what THIS call registered -- the write never
        // stages (an earlier-staged registration keeps its count until stop).
        if (appended_here) {
            tt.n--;
            g_staged[slot].by_gen[tt.e[tt.n].bucket].fetch_sub(
                1, std::memory_order_acq_rel);
        } else if (overflowed_here) {
            tt.overflow = false;
            g_overflow_by_gen[tt.overflow_bucket].fetch_sub(
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
        g_staged[tt.e[i].slot].by_gen[tt.e[i].bucket].fetch_sub(
            1, std::memory_order_acq_rel);
    }
    tt.n = 0;
    if (tt.overflow) {
        tt.overflow = false;
        g_overflow_by_gen[tt.overflow_bucket].fetch_sub(1, std::memory_order_acq_rel);
    }
    g_this_txn_counted = false;
    slot_close();
}

namespace {
// Residual pre-bump writers the drain still waits on: EVERY bucket except
// the drain's own bump's (skip_bucket) -- a registration in any older bucket
// predates this drain's fence, whichever earlier generation it came from.
// Scoped to the fenced table's slot (or every slot for the table-agnostic ""
// fence) plus the overflow.
long drain_residual(const std::string& table, unsigned skip_bucket) {
    long r = 0;
    for (unsigned b = 0; b < kGenBuckets; b++) {
        if (b == skip_bucket) continue;
        r += g_overflow_by_gen[b].load(std::memory_order_acquire);
        if (table.empty()) {
            for (int i = 0; i < kSlots; i++)
                r += g_staged[i].by_gen[b].load(std::memory_order_acquire);
        } else {
            r += g_staged[table_slot(table)].by_gen[b].load(
                std::memory_order_acquire);
        }
    }
    return r;
}
}  // namespace

// Returns the bump's OWN bucket -- the drain-poll cookie (wire name still
// "parity"): the one bucket the polls do NOT wait on.
int migration_fence_drain_begin() {
    const uint64_t new_gen =
        g_fence_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    return static_cast<int>(new_gen & kGenMask);
}

bool migration_fence_drained(const std::string& table, int skip_bucket) {
    return drain_residual(
               table, static_cast<unsigned>(skip_bucket) & kGenMask) == 0;
}

void migration_fence_dump_residual(const std::string& table) {
    char line[512];
    int off = snprintf(line, sizeof(line), "  residual '%s' slot=%d gen=%llu:",
                       table.c_str(), table.empty() ? -1 : table_slot(table),
                       static_cast<unsigned long long>(
                           g_fence_gen.load(std::memory_order_acquire)));
    for (unsigned b = 0; b < kGenBuckets; b++) {
        long v = g_overflow_by_gen[b].load(std::memory_order_acquire);
        long t = 0;
        if (table.empty()) {
            for (int i = 0; i < kSlots; i++)
                t += g_staged[i].by_gen[b].load(std::memory_order_acquire);
        } else {
            t = g_staged[table_slot(table)].by_gen[b].load(
                std::memory_order_acquire);
        }
        if ((v | t) != 0 && off < static_cast<int>(sizeof(line)) - 32) {
            off += snprintf(line + off, sizeof(line) - off,
                            " b%u=%ld/ov%ld", b, t, v);
        }
    }
    // @unsafe { stderr diagnostics }
    fprintf(stderr, "%s\n", line);
}

void migration_fence_dump_staged() {
    const long now = mono_seconds();
    for (int i = 0; i < 256; i++) {
        const long since = g_slots[i].since_s.load(std::memory_order_acquire);
        if (since >= 0) {
            // @unsafe { stderr diagnostics }
            fprintf(stderr, "  staged slot %d: table='%s' bucket=%u held=%lds\n",
                    i, g_slots[i].table, g_slots[i].bucket, now - since);
        }
    }
}

bool migration_fence_drain_writes_for(const std::string& table, int timeout_ms) {
    // Watermark: everything registered from here on lands in the bump's own
    // bucket; same-table writers holding ANY older bucket are waited out.
    const unsigned skip_bucket =
        static_cast<unsigned>(migration_fence_drain_begin());
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (drain_residual(table, skip_bucket) == 0) return true;
        usleep(10 * 1000);
    }
    const long residual = drain_residual(table, skip_bucket);
    if (residual != 0) {
        // @unsafe { stderr diagnostics }
        fprintf(stderr,
                "migration drain TIMEOUT: table='%s' skip_bucket=%u residual=%ld gen=%llu\n",
                table.c_str(), skip_bucket, residual,
                static_cast<unsigned long long>(
                    g_fence_gen.load(std::memory_order_acquire)));
        const long now = mono_seconds();
        for (int i = 0; i < 256; i++) {
            const long since = g_slots[i].since_s.load(std::memory_order_acquire);
            if (since >= 0) {
                fprintf(stderr,
                        "  staged slot %d: table='%s' bucket=%u held=%lds\n",
                        i, g_slots[i].table, g_slots[i].bucket, now - since);
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
