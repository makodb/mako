module;

// ShardData — the migration PARTICIPANT port: the abstract data plane a shard
// exposes so the ShardMaster coordinator (src/cluster/shard_master.h) can drive
// a range hand-off WITHOUT knowing whether the participant is LOCAL (a
// masstree/mbta index — OrderedIndexShardData, src/mako) or REMOTE (an RPC
// proxy to another shard process — RemoteShardData, src/mako/shard_data_service.h)
// or an in-memory test double (InMemoryShardData, cluster:in_memory_shard_data).
//
// This is the peer of the cluster module's KvStore port: one narrow interface,
// multiple transports, so the control plane (ShardMaster) compiles and
// unit-tests with NO dependency on the storage engine. Backends implement the
// primitives (put / get / remove / scan_range / scan_range_limited); the
// migration operations (range_count / checksum / copy_range_from / drop_range)
// are defined ONCE here over the primitives, so every backend gets them for free
// and the source and destination always agree on the checksum definition.

#include <algorithm>   // lower_bound (the mirror-copy's membership probe)
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

export module cluster:shard_data;

namespace janus {

// @unsafe - participants wrap the storage engine or an RPC transport.
export class ShardData {
public:
    using KvPair = std::pair<std::string, std::string>;
    virtual ~ShardData() = default;

    // ---- primitives every backend implements ----
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual bool get(const std::string& key, std::string& out) = 0;
    virtual void remove(const std::string& key) = 0;
    // Enumerate every live key->value pair in [lo, hi).
    virtual std::vector<KvPair> scan_range(const std::string& lo,
                                           const std::string& hi) = 0;
    // Scan up to `limit` live pairs from [lo, hi). Backends with an OCC/retrying
    // scan (mbta) implement this to stop early at the engine level, so a
    // concurrent-write conflict only re-scans a small window -- copy_range_from
    // chunks over it and never starves on a hot range.
    virtual std::vector<KvPair> scan_range_limited(const std::string& lo,
                                                   const std::string& hi,
                                                   size_t limit) = 0;

    // ---- migration ops, defined over the primitives (backend-agnostic) ----
    size_t range_count(const std::string& lo, const std::string& hi) {
        return scan_range(lo, hi).size();
    }
    // Order-independent u64 fold over live pairs in [lo, hi). A deleted key
    // drops out of the scan on both sides, so deletes keep source and
    // destination checksums equal without any tombstone.
    virtual uint64_t checksum(const std::string& lo, const std::string& hi) {
        uint64_t sum = 0;
        for (const auto& kv : scan_range(lo, hi)) {
            sum += fnv(kv.first) * 1000003ull + fnv(kv.second);
        }
        return sum;
    }
    // Verify this shard's [lo,hi) against a checksum computed ELSEWHERE (the
    // source's, in a migration's 2PC prepare): returns true iff this shard's
    // range is byte-identical to the source's. On a remote backend this is one
    // RPC -- the source's checksum goes in, a bool comes out, and this shard's
    // own checksum never leaves it.
    virtual bool verify_range(const std::string& lo, const std::string& hi,
                              uint64_t expected_checksum) {
        return checksum(lo, hi) == expected_checksum;
    }
    // Background bulk copy as a MIRROR: make this shard's [lo,hi) EQUAL the
    // source's -- put every live source pair (idempotent overwrite) AND delete
    // the keys this shard holds in the range that the source does not. A
    // put-only copy cannot converge a table that takes DELETES: TPC-C delivery
    // consumes new_order rows between the phase-1 copy and the fence, the
    // stale destination rows survive every re-copy, and the checksum gate
    // refuses forever (observed live). Mirroring is safe in every phase
    // because the destination never serves [lo,hi) before the commit. CHUNKED:
    // scan the source in small windows so a concurrent write invalidates only
    // one window, not the whole range (on an OCC-backed source this turns a
    // hot-range whole-scan starvation into guaranteed progress); both cursors
    // resume strictly after the last key seen.
    //
    // VIRTUAL so the copy is DESTINATION-DRIVEN: this default runs where the
    // destination lives (local dest pulls straight from the source), and the
    // RPC destination participant overrides it to send ONE PullRange control
    // RPC -- the destination shard then pulls directly from the source's
    // data-plane service. Migration data never transits the coordinator.
    // Returns the number of pairs written.
    virtual size_t copy_range_from(ShardData* source, const std::string& lo,
                                   const std::string& hi) {
        // Chunk size bounds the SERVER-SIDE LATENCY of one remote scan, not
        // just the response bytes: under live write conflict the engine scans
        // the chunk in small adaptive sub-windows with bounded-attempt retry,
        // and a 4096-row chunk of a write-hot index summed past the rrr ~1s
        // request budget (observed live: a 10s stock chunk = 8 client
        // timeouts, retries queueing behind the still-running scan on the
        // single poll thread). 512 keeps the worst case comfortably inside
        // the budget; a 100k-row table is ~200 round-trips, fine off the
        // critical path.
        static const size_t kCopyChunk = 512;
        size_t copied = 0;
        std::string src_cur = lo;
        std::string dst_cur = lo;
        while (true) {
            std::vector<KvPair> batch =
                source->scan_range_limited(src_cur, hi, kCopyChunk);
            const bool src_done = batch.size() < kCopyChunk;
            if (src_done && source->faulted()) {
                // A dead source reads as an empty tail; finishing the mirror
                // would DELETE every remaining destination key in the range.
                // Stop here -- the coordinator sees faulted() and aborts.
                return copied;
            }
            for (const auto& kv : batch) put(kv.first, kv.second);
            copied += batch.size();
            // The batch is authoritative for [dst_cur, scan_to): a key of
            // ours in that window that is not in the (sorted) batch no
            // longer exists on the source -- remove it.
            std::string scan_to;
            if (src_done) {
                scan_to = hi;
            } else {
                scan_to = batch.back().first;
                scan_to.push_back('\0');
            }
            while (dst_cur < scan_to) {
                std::vector<KvPair> mine =
                    scan_range_limited(dst_cur, scan_to, kCopyChunk);
                for (const auto& kv : mine) {
                    auto it = std::lower_bound(
                        batch.begin(), batch.end(), kv.first,
                        [](const KvPair& p, const std::string& k) {
                            return p.first < k;
                        });
                    if (it == batch.end() || it->first != kv.first) {
                        remove(kv.first);
                    }
                }
                if (mine.size() < kCopyChunk) break;
                dst_cur = mine.back().first;
                dst_cur.push_back('\0');
            }
            dst_cur = scan_to;
            if (src_done) return copied;
            src_cur = scan_to;
        }
    }

    // ---- data-plane self-identification (for destination-driven copy) ----
    // Where can ANOTHER shard reach this participant's rows over the
    // ShardDataService, and under which table name? A participant that is
    // network-reachable (an RPC proxy, or a local engine table served by this
    // process) reports its host:port + table; in-memory test doubles report ""
    // (unreachable), which makes the destination fall back to the coordinator-
    // side default copy above.
    virtual std::string service_addr() { return std::string(); }
    virtual std::string service_table() { return std::string(); }
    // Post-commit shed: drop the range from this shard.
    virtual void drop_range(const std::string& lo, const std::string& hi) {
        for (const auto& kv : scan_range(lo, hi)) remove(kv.first);
    }

    // ---- write drain (the fence's other half) ----
    // Block until every writer that staged into this participant's table
    // before the fence has finished, so the post-drain catch-up copy sees a
    // provably quiescent range: the staging fence rejects writes that begin
    // AFTER the fence install, and this waits out the ones that began BEFORE
    // it (the check-then-act closure -- see docs/mako-book.md s3). The engine
    // participant waits a per-table staged-writer watermark; the RPC
    // participant polls the remote shard; in-memory doubles are trivially
    // quiescent. Returns false only on timeout (the coordinator aborts).
    virtual bool drain_writes() { return true; }

    // Begin/poll split of the same drain, for RPC transports whose requests
    // cannot block on the wait (rrr's ~1s client timeout): drain_begin bumps
    // the watermark generation ONCE and returns the parity to poll (or -1 if
    // this participant has no split drain -- callers fall back to
    // drain_writes); drain_poll answers "has that parity emptied" within a
    // short server-side budget.
    virtual int drain_begin() { return -1; }
    virtual bool drain_poll(int /*parity*/) { return drain_writes(); }

    // ---- participant health ----
    // Has ANY operation on this participant failed (e.g. an RPC error on a
    // remote participant whose process died)? The coordinator consults this
    // before voting prepared: a faulted participant's reads degenerate to
    // empty-scan / checksum-0, and an empty destination ALSO checksums 0, so
    // without this gate a dead source would VACUOUSLY match and the migration
    // would commit a cutover to an empty destination — losing the range to
    // routing while the rows sit on the (dead) source. Local/in-memory
    // participants never fault.
    virtual bool faulted() { return false; }

    // Un-latch a prior fault before a NEW migration attempt: the latch is
    // per-participant and participants are cached per (shard, table), so a
    // transient (e.g. one congested copy chunk exhausting its RPC retries)
    // would otherwise poison every later attempt against a healthy shard.
    virtual void clear_faulted() {}

    // ---- migration freeze (write fence) on THIS participant ----
    // The coordinator (ShardMaster) freezes the SOURCE's [lo,hi) at lock_range so
    // no write slips between the final copy and the cutover, and unfreezes only on
    // abort. On commit the source is left frozen on purpose: it no longer owns the
    // range, and the standing freeze keeps rejecting stale-routed writers
    // (SERVER_BUSY -> retry) until their config reloads and lands them on the new
    // owner. Backends where a freeze is meaningful override these: the local mbta
    // participant fences its process-global MigrationGuard (which the shard's
    // non-txn write handler enforces); the RPC participant forwards to the remote
    // shard's service. In-memory test doubles keep the no-op default.
    virtual void freeze_range(const std::string& lo, const std::string& hi) {
        (void)lo; (void)hi;
    }
    virtual void unfreeze_range(const std::string& lo, const std::string& hi) {
        (void)lo; (void)hi;
    }

    static uint64_t fnv(const std::string& s) {
        uint64_t h = 1469598103934665603ull;  // < 2^63
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }
};

}  // namespace janus
