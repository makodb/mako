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
    // Background bulk copy: pull the source's [lo,hi) into this shard, CHUNKED --
    // scan the source in small windows so a concurrent write invalidates only one
    // window, not the whole range. On an OCC-backed source (mbta) this turns a
    // hot-range whole-scan starvation into guaranteed progress; on a cold range
    // it is just a handful of extra scans. Correctness is unchanged.
    //
    // VIRTUAL so the copy is DESTINATION-DRIVEN: this default runs where the
    // destination lives (local dest pulls straight from the source), and the
    // RPC destination participant overrides it to send ONE PullRange control
    // RPC -- the destination shard then pulls directly from the source's
    // data-plane service. Migration data never transits the coordinator.
    virtual void copy_range_from(ShardData* source, const std::string& lo,
                                 const std::string& hi) {
        static const size_t kCopyChunk = 512;
        std::string cur = lo;
        while (cur < hi) {
            std::vector<KvPair> batch = source->scan_range_limited(cur, hi, kCopyChunk);
            if (batch.empty()) break;
            for (const auto& kv : batch) put(kv.first, kv.second);
            cur = batch.back().first;
            cur.push_back('\0');   // resume strictly after the last key copied
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
