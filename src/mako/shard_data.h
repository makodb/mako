#pragma once

// ShardData — the participant seam for online migration: the abstract data
// plane a shard exposes so the ShardMigrator coordinator can drive a range
// hand-off WITHOUT knowing whether the participant is LOCAL (masstree/mbta —
// OrderedIndexShardData) or REMOTE (an RPC proxy to another shard process).
//
// Backends implement four primitives (put / get / remove / scan_range); the
// migration operations (range_count / checksum / copy_range_from / drop_range)
// are defined ONCE here over scan_range, so every backend gets them for free
// and the source and destination always agree on the checksum definition.
//
// This is the C++/mako-side analogue of the cluster module's KvStore port: one
// interface, multiple transports.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace janus {

// @unsafe - participants wrap the storage engine or an RPC transport.
class ShardData {
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
    uint64_t checksum(const std::string& lo, const std::string& hi) {
        uint64_t sum = 0;
        for (const auto& kv : scan_range(lo, hi)) {
            sum += fnv(kv.first) * 1000003ull + fnv(kv.second);
        }
        return sum;
    }
    // Background bulk copy: pull the source's [lo,hi) into this shard, CHUNKED --
    // scan the source in small windows so a concurrent write invalidates only one
    // window, not the whole range. On an OCC-backed source (mbta) this turns a
    // hot-range whole-scan starvation into guaranteed progress; on a cold range
    // it is just a handful of extra scans. Correctness is unchanged.
    void copy_range_from(ShardData& source, const std::string& lo,
                         const std::string& hi) {
        static const size_t kCopyChunk = 512;
        std::string cur = lo;
        while (cur < hi) {
            std::vector<KvPair> batch = source.scan_range_limited(cur, hi, kCopyChunk);
            if (batch.empty()) break;
            for (const auto& kv : batch) put(kv.first, kv.second);
            cur = batch.back().first;
            cur.push_back('\0');   // resume strictly after the last key copied
        }
    }
    // Post-commit shed: drop the range from this shard.
    void drop_range(const std::string& lo, const std::string& hi) {
        for (const auto& kv : scan_range(lo, hi)) remove(kv.first);
    }

    static uint64_t fnv(const std::string& s) {
        uint64_t h = 1469598103934665603ull;  // < 2^63
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }
};

}  // namespace janus
