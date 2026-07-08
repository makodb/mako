#pragma once

// ShardMigrator — the online 2PC migration COORDINATOR for the real engine:
// the plain-C++ counterpart to the stub ShardManager's migration methods
// (src/cluster/shard_manager.h), run against real mbta-backed shard data. It
// drives ONE range hand-off between two participants (OrderedIndexShardData)
// through the phases of docs/mako-book.md §3:
//
//   background_copy()        Phase 1: bulk copy [lo,hi); the source keeps
//                                     serving reads+writes throughout.
//   staged_put/staged_delete           writes that land on the range after the
//                                     snapshot -> applied to the source AND
//                                     remembered as the delta (put supersedes a
//                                     staged delete and vice-versa).
//   lock()                   Phase 2: freeze the range on the source (a
//                                     control-plane range-predicate gate; the
//                                     engine has no range lock).
//   final_sync_and_verify()  Phase 3: replay the delta to the destination, then
//                                     the checksum-equality gate -> the prepare
//                                     "yes"/"no" vote.
//   commit()                 Phase 4: the source drops the range (routing flips
//                                     via the config version bump elsewhere).
//   abort()                            the source keeps the range (it only ever
//                                     stopped SERVING at lock); dest discards.
//
// A per-attempt `generation` fences stale votes exactly as the stub does. The
// participants are LOCAL (in-process) here; in the cross-process step they
// become RPC proxies implementing the same shard-data ops, and this coordinator
// is unchanged. The config-plane writes (reshard/* keys, version bump) are
// layered on top by the caller via ConfigManager, which already works on mbta.

#include "shard_data.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace janus {

// @unsafe - orchestrates two OrderedIndexShardData (storage-backed) participants.
class ShardMigrator {
public:
    ShardMigrator(ShardData* source, ShardData* dest,
                  std::string lo, std::string hi, uint64_t generation)
        : src_(source), dst_(dest), lo_(std::move(lo)), hi_(std::move(hi)),
          gen_(generation) {}

    // Phase 1 — background bulk copy. Source stays live.
    void background_copy() { dst_->copy_range_from(*src_, lo_, hi_); }

    // A write to the range during copy: apply to the source (authoritative) and
    // remember it as the delta to replay at final sync.
    void staged_put(const std::string& key, const std::string& value) {
        src_->put(key, value);
        del_.erase(key);          // a put supersedes a staged delete
        staged_[key] = value;
    }
    void staged_delete(const std::string& key) {
        src_->remove(key);
        staged_.erase(key);       // a delete supersedes a staged put
        del_.insert(key);
    }

    // Phase 2 — freeze the range on the source.
    void lock() { locked_ = true; }
    bool locked() const { return locked_; }
    // True iff a key is currently frozen (locked AND inside the migrating range);
    // the source rejects writes/reads to such keys so clients retry.
    bool frozen_for(const std::string& key) const {
        return locked_ && key >= lo_ && key < hi_;
    }

    // Phase 3 — replay the delta to the destination, then the checksum-equality
    // gate. Returns true iff the two ranges are byte-identical (the "prepared"
    // vote). A false vote makes the coordinator abort rather than cut over.
    bool final_sync_and_verify() {
        for (const auto& kv : staged_) dst_->put(kv.first, kv.second);
        for (const auto& k : del_)     dst_->remove(k);
        // Distributed 2PC shape: the SOURCE computes its checksum (its "prepared"
        // proof), the coordinator ships that scalar to the DESTINATION, and the
        // destination verifies its own range against it. For a remote dst this is
        // a single RPC -- the dst's checksum never leaves it, only the bool vote
        // comes back. (Local dst: verify_range just does the == in-process.)
        const uint64_t src_ck = src_->checksum(lo_, hi_);
        return dst_->verify_range(lo_, hi_, src_ck);
    }

    // Phase 4 — COMMIT: the source drops the migrated range (its data now lives
    // on the destination). Precondition: locked() && final_sync_and_verify().
    void commit() { src_->drop_range(lo_, hi_); }

    // ABORT: the source keeps the range (it never dropped it — only stopped
    // serving at lock). The destination's partial copy is discarded by dropping
    // it, so a retried migration starts clean.
    void abort() { dst_->drop_range(lo_, hi_); }

    uint64_t generation() const { return gen_; }
    const std::string& lo() const { return lo_; }
    const std::string& hi() const { return hi_; }

private:
    ShardData* src_;
    ShardData* dst_;
    std::string lo_, hi_;
    uint64_t gen_;
    bool locked_ = false;
    std::map<std::string, std::string> staged_;  // delta: puts
    std::set<std::string> del_;                   // delta: deletes
};

}  // namespace janus
