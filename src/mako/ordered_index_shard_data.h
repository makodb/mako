#pragma once

// OrderedIndexShardData — the real (masstree/mbta-backed) implementation of a
// shard's DATA PLANE for online migration: the production counterpart to the
// in-memory BTreeMap stub Shard in src/cluster/shard.h. It wraps a
// FullOrderedIndex and provides exactly the range primitives the 2PC migration
// protocol needs — scan_range / copy_range_from / drop_range / range_count /
// checksum plus point put/get/remove — over the non-txn OrderedIndex surface
// (raw bytes; scan covers [lo,hi)). The storage recon (docs/mako-book.md §3)
// established that these are all the engine ops migration needs: freeze is a
// control-plane range gate, and deletes ride the coordinator's delta (the
// engine hard-deletes, keeping no tombstone).
//
// Deliberately PURE data plane: participant metadata (owned ranges, migration
// freeze/role/generation) is NOT here — it belongs to the shard wrapper /
// control plane, exactly as the stub splits Shard-metadata from Shard-data.
//
// Mirrors src/mako/ordered_index_kv_store.h (which binds the cluster KvStore
// port onto a FullOrderedIndex); this binds the shard-data seam. Lives on the
// mako side so the cluster module stays free of storage-engine headers.

#include "storage/abstract_ordered_index.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace janus {

// @unsafe - wraps a FullOrderedIndex (storage engine); non-owning.
class OrderedIndexShardData {
public:
    using KvPair = std::pair<std::string, std::string>;

    // The index outlives this adapter (owned by the shard/process).
    explicit OrderedIndexShardData(::FullOrderedIndex* index) : index_(index) {}

    // ---- point ops (raw bytes; non-txn OrderedIndex surface) ----
    void put(const std::string& key, const std::string& value) {
        index_->put(lcdf::Str(key.data(), key.size()), value);
    }
    bool get(const std::string& key, std::string& out) const {
        return index_->get(lcdf::Str(key.data(), key.size()), out,
                           std::string::npos);
    }
    void remove(const std::string& key) {
        index_->remove(lcdf::Str(key.data(), key.size()));
    }

    // ---- range ops (the migration primitives) ----
    // Enumerate every live key->value pair in [lo, hi).
    std::vector<KvPair> scan_range(const std::string& lo,
                                   const std::string& hi) const {
        Collector cb;
        index_->scan(lo, &hi, cb, nullptr);
        return std::move(cb.pairs);
    }
    size_t range_count(const std::string& lo, const std::string& hi) const {
        return scan_range(lo, hi).size();
    }
    // Order-independent u64 fold over live pairs in [lo, hi). A removed key
    // simply drops out of the scan on both sides, so deletes keep the source
    // and destination checksums equal without any tombstone.
    uint64_t checksum(const std::string& lo, const std::string& hi) const {
        uint64_t sum = 0;
        for (const auto& kv : scan_range(lo, hi)) {
            sum += fnv(kv.first) * 1000003ull + fnv(kv.second);
        }
        return sum;
    }
    // Background bulk copy: pull the source shard's [lo,hi) into this shard.
    void copy_range_from(const OrderedIndexShardData& source,
                         const std::string& lo, const std::string& hi) {
        for (const auto& kv : source.scan_range(lo, hi)) {
            put(kv.first, kv.second);
        }
    }
    // Post-commit shed: drop the migrated range from this shard.
    void drop_range(const std::string& lo, const std::string& hi) {
        for (const auto& kv : scan_range(lo, hi)) {
            remove(kv.first);
        }
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

    static uint64_t fnv(const std::string& s) {
        uint64_t h = 1469598103934665603ull;  // < 2^63
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }

    ::FullOrderedIndex* index_;
};

}  // namespace janus
