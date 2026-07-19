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
#include "lib/migration_fence.h"   // fence bypass + the staged-writer drain

#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

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
    //
    // Internally the window is scanned in SMALL ADAPTIVE SUB-SCANS and
    // stitched. A single limit-sized (4096-row) one-op scan of a HOT index
    // can lose the OCC race forever: any concurrent write to a visited row
    // aborts the attempt, and when the writer stream's inter-write gap is
    // shorter than the scan itself, no amount of retrying (even backed off)
    // wins -- observed live and core-proven: the migration data plane's
    // stock-chunk scan pinned its single poll thread through hundreds of
    // backed-off retries while the peer shard's remote new_order writes
    // kept invalidating it, muting the whole data-plane service. A 64-row
    // sub-scan finishes inside the write gap easily; each success doubles
    // the sub-size back toward the chunk. The contract is unchanged:
    // fewer than `limit` pairs are returned ONLY when the range ended (a
    // sub-scan shorter than its own sub-limit is the range-end signal).
    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                           const std::string& hi,
                                           size_t limit) override {
        std::vector<KvPair> out;
        out.reserve(limit);
        std::string cur = lo;
        size_t sub = 64;
        int conflicted_rounds = 0;
        while (out.size() < limit) {
            const size_t left = limit - out.size();
            const size_t want = sub < left ? sub : left;
            LimitedCollector cb(want);
            // Bounded-attempt scan through the thread-local seam: the engine
            // gives up after a few OCC-aborted attempts and raises the flag
            // (discarding partial rows) instead of retrying a hopeless
            // window forever -- grow-only adaptivity re-wedged live at 1024
            // (core-proven), so the control loop must SHRINK on conflict.
            mako::g_oi_scan_attempt_cap = 4;
            mako::g_oi_scan_conflicted = false;
            index_->scan(cur, &hi, cb, nullptr);
            const bool conflicted = mako::g_oi_scan_conflicted;
            mako::g_oi_scan_attempt_cap = 0;
            mako::g_oi_scan_conflicted = false;
            if (conflicted) {
                // Window longer than the writers' inter-write gap: shrink
                // hard (floor 16 -- a ~30us scan always slips into a gap)
                // and retry the SAME position -- but PACED. Each aborted
                // attempt is a thrown Transaction::Abort, and C++ unwinding
                // serializes on a process-global unwinder lock: an unpaced
                // conflicted loop threw thousands of exceptions per second,
                // convoying every OTHER thread's normal abort/retry path in
                // this process (commits stalled holding row locks, which
                // aborted the next scan attempt -- a self-sustaining wedge,
                // core-proven live: workers stuck in commit_txn while this
                // loop spun). Escalating to 10ms caps the storm at ~400
                // throws/s and lets stalled writers drain, after which a
                // small window wins.
                sub = sub > 128 ? sub / 8 : 16;
                ++conflicted_rounds;
                // Rate-limited abort forensics (see g_oi_scan_abort_progress
                // in abstract_ordered_index.h for how to read `got`).
                // @unsafe { gettimeofday + stderr diagnostics }
                {
                    static std::atomic<long> s_last{0};
                    struct timeval tv;
                    gettimeofday(&tv, nullptr);
                    long prev = s_last.load(std::memory_order_relaxed);
                    if (tv.tv_sec != prev &&
                        s_last.compare_exchange_strong(prev, tv.tv_sec,
                                                       std::memory_order_relaxed)) {
                        char curhex[17];
                        size_t n = cur.size() < 8 ? cur.size() : 8;
                        for (size_t i = 0; i < n; i++)
                            snprintf(curhex + 2 * i, 3, "%02x",
                                     (unsigned char)cur[i]);
                        curhex[2 * n] = '\0';
                        fprintf(stderr,
                                "scan-conflict round=%d sub=%zu got=%zu "
                                "curlen=%zu cur=%s\n",
                                conflicted_rounds, sub,
                                mako::g_oi_scan_abort_progress, cur.size(),
                                curhex);
                    }
                }
                const int shift = conflicted_rounds < 5 ? conflicted_rounds : 5;
                unsigned pace_us = 500u << shift;
                if (pace_us > 10000u) pace_us = 10000u;
                // @unsafe { usleep is libc }
                usleep(pace_us);
                continue;
            }
            conflicted_rounds = 0;
            const size_t got = cb.pairs.size();
            for (auto& kv : cb.pairs) out.push_back(std::move(kv));
            if (got < want) break;          // range exhausted
            cur = out.back().first;
            cur.push_back('\0');            // resume strictly after the last key
            if (sub < 512) sub *= 2;        // winning the race: widen the window
        }
        return out;
    }

    // Write drain on the LOCAL engine: wait until no staged-write txn that
    // began before the fence remains (the staged-writer counter in
    // lib/migration_fence.cc -- registered BEFORE the fence check, closed at
    // Transaction::stop for txn staging and at RAII scope exit for the
    // internally-retrying one-op kernels, so post-fence the count only drains).
    // NOTE deliberately NOT Silo's active_epoch: idle 2PC participants hold
    // in_progress-but-empty txns that pin epochs forever; empty txns cannot
    // hold pre-fence staged writes, so the counter is both sufficient and
    // immune to that pinning. Timeout 3s -> false, the coordinator aborts.
    bool drain_writes() override {
        return mako::migration_fence_drain_writes(3000);
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
    // Collects (key, value) pairs from a scan; never stops early. restart()
    // (each retry attempt of the whole-scan-retrying one-op scan) discards
    // the aborted attempt's rows -- they are the reads that failed OCC
    // validation, i.e. torn values, and keeping them corrupted migration
    // copies and checksums (observed live).
    class Collector : public oi_scan_callback {
    public:
        void restart() override { pairs.clear(); }
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
        void restart() override { pairs.clear(); }
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
