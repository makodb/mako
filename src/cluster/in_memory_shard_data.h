module;
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <btree_port/btreemap.hpp>  // native-API ordered map (the fake's data)
#include <rusty/slice.hpp>          // rusty::for_in (ordered BTreeMap iteration)
export module cluster:in_memory_shard_data;
import :shard_data;

namespace janus {

/**
 * InMemoryShardData — a btree_port::BTreeMap-backed ShardData for unit tests.
 * The test double of the migration participant port (peer of InMemoryKvStore):
 * it lets the ShardMaster migration tests drive the 2PC copy/lock/verify/commit
 * with no storage engine, no masstree, no RPC. A remove() erases the key (the
 * ShardData contract folds live pairs; a deleted key simply drops out of the
 * scan, so source and destination checksums stay equal without a tombstone).
 *
 * BTreeMap iterates in sorted key order, so scan_range_limited can stop at the
 * first key >= hi and after `limit` pairs — exactly the chunked-copy contract.
 */
// @safe - BTreeMap behind the participant port; no I/O.
export class InMemoryShardData : public ShardData {
public:
    void put(const std::string& key, const std::string& value) override {
        data_.insert(key, value);  // BTreeMap::insert overwrites
    }

    bool get(const std::string& key, std::string& out) override {
        auto found = data_.get(key);
        if (found.is_none()) return false;
        out = std::string(found.unwrap().get());
        return true;
    }

    void remove(const std::string& key) override { data_.remove(key); }

    std::vector<KvPair> scan_range(const std::string& lo,
                                   const std::string& hi) override {
        std::vector<KvPair> out;
        for (auto&& kv : rusty::for_in(data_)) {
            if (kv.first < lo) continue;
            if (!(kv.first < hi)) break;   // sorted: past the range
            out.emplace_back(kv.first, kv.second);
        }
        return out;
    }

    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                           const std::string& hi,
                                           size_t limit) override {
        std::vector<KvPair> out;
        for (auto&& kv : rusty::for_in(data_)) {
            if (kv.first < lo) continue;
            if (!(kv.first < hi)) break;   // sorted: past the range
            out.emplace_back(kv.first, kv.second);
            if (out.size() >= limit) break;
        }
        return out;
    }

    // ---- Test helpers (not part of the ShardData port) ----
    size_t size() const { return data_.size(); }
    void clear() { data_.clear(); }

private:
    btree_port::BTreeMap<std::string, std::string> data_;
};

}  // namespace janus
