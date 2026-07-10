#pragma once

// OrderedIndexShardData — the LOCAL (masstree/mbta-backed) ShardData: the real
// production shard data plane, the counterpart to the in-memory stub Shard in
// src/cluster/shard.h. It implements the four ShardData primitives (put / get /
// remove / scan_range) over a FullOrderedIndex, on the non-txn OrderedIndex
// surface (raw bytes; scan covers [lo,hi)); the migration operations
// (checksum / copy_range_from / drop_range / range_count) come from the
// ShardData base, defined once over scan_range.
//
// Mirrors src/mako/ordered_index_kv_store.h (which binds the cluster KvStore
// port onto a FullOrderedIndex); this binds the shard-data seam. Lives on the
// mako side so the cluster module stays free of storage-engine headers. Pure
// data plane — freeze / owned ranges / migration role live in the control
// plane (ShardMigrator / the config manager), per the storage recon.

import cluster;   // ShardData participant port (janus::ShardData; was #include "shard_data.h")
#include "storage/abstract_ordered_index.h"
#include "lib/migration_fence.h"   // MigrationFenceBypass: plane writes are exempt

#include <string>
#include <utility>
#include <vector>

namespace janus {

// @unsafe - wraps a FullOrderedIndex (storage engine); non-owning.
class OrderedIndexShardData : public ShardData {
public:
    // The index outlives this adapter (owned by the shard/process).
    explicit OrderedIndexShardData(::FullOrderedIndex* index) : index_(index) {}

    // Data-plane writes are coordinator-controlled and exempt from the
    // migration write fence (the copy itself pulls rows into a destination
    // that may still carry a stale fence, e.g. ping-pong).
    void put(const std::string& key, const std::string& value) override {
        mako::MigrationFenceBypass bypass;
        index_->put(lcdf::Str(key.data(), key.size()), value);
    }
    bool get(const std::string& key, std::string& out) override {
        return index_->get(lcdf::Str(key.data(), key.size()), out,
                           std::string::npos);
    }
    void remove(const std::string& key) override {
        mako::MigrationFenceBypass bypass;
        index_->remove(lcdf::Str(key.data(), key.size()));
    }
    std::vector<KvPair> scan_range(const std::string& lo,
                                   const std::string& hi) override {
        Collector cb;
        index_->scan(lo, &hi, cb, nullptr);
        return std::move(cb.pairs);
    }

    // Scan up to `limit` live pairs from [lo, hi). Used to CHUNK a range copy so
    // a concurrent-write OCC conflict only re-scans one small window, not the
    // whole range (avoids the hot-range scan-retry starvation; still a
    // memory-safe OCC read). Returns fewer than `limit` iff the range ended.
    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                           const std::string& hi,
                                           size_t limit) override {
        LimitedCollector cb(limit);
        index_->scan(lo, &hi, cb, nullptr);
        return std::move(cb.pairs);
    }

    // Migration write fence on the LOCAL shard: this participant lives in the
    // serving process, so the freeze goes to the process-global MigrationGuard --
    // the registry the shard's non-txn write handler (RunNontxnOp) enforces with
    // SERVER_BUSY. Table-agnostic "" (one service per index), matching
    // ShardDataServiceImpl::DoFreezeRange on the remote side.
    void freeze_range(const std::string& lo, const std::string& hi) override {
        get_migration_guard().freeze(std::string(), lo, hi);
    }
    void unfreeze_range(const std::string& lo, const std::string& hi) override {
        get_migration_guard().unfreeze(std::string(), lo, hi);
    }

private:
    // Collects (key, value) pairs from a scan; never stops early.
    class Collector : public oi_scan_callback {
    public:
        bool invoke(const char* keyp, size_t keylen,
                    const std::string& value) override {
            pairs.emplace_back(std::string(keyp, keylen), value);
            return true;
        }
        std::vector<KvPair> pairs;
    };

    // Like Collector but stops after `limit` pairs (for chunked scans).
    class LimitedCollector : public oi_scan_callback {
    public:
        explicit LimitedCollector(size_t limit) : limit_(limit) {}
        bool invoke(const char* keyp, size_t keylen,
                    const std::string& value) override {
            pairs.emplace_back(std::string(keyp, keylen), value);
            return pairs.size() < limit_;   // stop once we have `limit`
        }
        std::vector<KvPair> pairs;
    private:
        size_t limit_;
    };

    ::FullOrderedIndex* index_;
};

}  // namespace janus
