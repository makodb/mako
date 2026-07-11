// Engine side of the shard's migratable data plane (see shard_data_plane.h).
//
// Deliberately masstree-header-FREE: engine thread registration goes through the
// VIRTUAL abstract_db::thread_init (the same registration the gtest migration
// threads use via scoped_db_thread_ctx), so this TU only needs the abstract
// storage interfaces. The first cut included mbta_wrapper.hh for
// mbta_table::thread_init and hit the known pathological-TU compile (masstree
// forest + import cluster under -O2: >1h); the virtual call is the same
// registration without the headers.

#include "shard_data_plane.h"

#include <stdio.h>
#include <time.h>     // time (drain-poll log rate limit)
#include <unistd.h>   // usleep (drain poll)

#include <atomic>
#include <map>
#include <string>
#include <thread>

#include <rusty/mutex.hpp>

#include "storage/abstract_db.h"           // abstract_db::thread_init (virtual)
#include "standalone_index.h"              // registry-free mbta index factory
#include "ordered_index_shard_data.h"      // janus::OrderedIndexShardData
#include "ordered_index_kv_store.h"        // janus::OrderedIndexKvStore
#include "lib/table_registry.h"            // mako::get_table_registry (name<->id)
#include "lib/migration_fence.h"           // per-table staged-writer drain
#include "benchmarks/tpcc_warehouse_directory.h"   // wh: spec -> workload index

namespace mako {
namespace {

// Engine-register the calling thread once (thread_local guard). Loader-mode
// thread_init is the registration the reroute/migration gtest threads use
// (scoped_db_thread_ctx(db, /*loader=*/true)); the engine self-assigns the
// thread id. No paired thread_end: these are process-lifetime service threads.
void engine_init_this_thread(abstract_db* db) {
    thread_local bool done = false;
    if (done) return;
    done = true;
    if (db != nullptr) {
        db->thread_init(/*loader=*/true, /*source=*/0);
    }
}

// The real shard data plane behind a per-op engine-registration gate: rrr
// handler threads reach the storage index through here without any prior setup.
// Wraps (not derives) OrderedIndexShardData so every entry point — including
// the ShardData base's checksum/copy/drop defaults, which call back into these
// primitives — passes the gate.
// @unsafe - delegates to the storage engine; non-owning db/index.
class EngineShardData : public janus::ShardData {
public:
    EngineShardData(abstract_db* db, ::FullOrderedIndex* idx,
                    std::string addr, std::string table)
        : db_(db), inner_(idx), addr_(std::move(addr)), table_(std::move(table)) {}

    // Self-identification as a pull source: this table is served by THIS
    // shard's ShardDataService, so a remote migration destination can pull the
    // range directly from here (no rows through the coordinator).
    std::string service_addr() override { return addr_; }
    std::string service_table() override { return table_; }

    void put(const std::string& k, const std::string& v) override {
        engine_init_this_thread(db_);
        inner_.put(k, v);
    }
    bool get(const std::string& k, std::string& out) override {
        engine_init_this_thread(db_);
        return inner_.get(k, out);
    }
    void remove(const std::string& k) override {
        engine_init_this_thread(db_);
        inner_.remove(k);
    }
    std::vector<KvPair> scan_range(const std::string& lo,
                                   const std::string& hi) override {
        engine_init_this_thread(db_);
        return inner_.scan_range(lo, hi);
    }
    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                           const std::string& hi,
                                           size_t limit) override {
        engine_init_this_thread(db_);
        return inner_.scan_range_limited(lo, hi, limit);
    }
    // Drain on THIS table's staged-writer watermark: cross-shard 2PC gives
    // unrelated txns multi-second tails, so a process-wide wait times out on
    // live beds (forensics: 3-5s holds on other warehouses indexes); tails
    // under migration load reach ~14s -- and the gen-bucket watermark now
    // (correctly) waits real pre-fence stragglers the parity scheme missed --
    // so 60s. The begin/poll split serves the RPC path (the DrainWrites
    // handler), whose per-call budget must stay under rrr's ~1s request
    // timeout.
    bool drain_writes() override {
        return mako::migration_fence_drain_writes_for(table_, 60000);
    }
    int drain_begin() override { return mako::migration_fence_drain_begin(); }
    bool drain_poll(int parity) override {
        for (int waited = 0; waited < 400; waited += 10) {
            if (mako::migration_fence_drained(table_, parity)) return true;
            usleep(10 * 1000);
        }
        if (mako::migration_fence_drained(table_, parity)) return true;
        // Still waiting after this poll's budget: name the holders (rate-
        // limited to ~1/s process-wide). @unsafe { stderr diagnostics }
        static std::atomic<long> last_log{0};
        const long now = time(nullptr);
        long prev = last_log.load(std::memory_order_relaxed);
        if (now != prev &&
            last_log.compare_exchange_strong(prev, now, std::memory_order_relaxed)) {
            fprintf(stderr, "drain_poll waiting: table='%s' skip_bucket=%d\n",
                    table_.c_str(), parity);
            mako::migration_fence_dump_residual(table_);
            mako::migration_fence_dump_staged();
        }
        return false;
    }

    // Fence ops go to the process-global MigrationGuard under THIS table's name
    // (matching the remote service's named entries, so a later unfreeze -- e.g.
    // the destination clearing a stale fence when it re-gains a range -- finds
    // the exact (table, lo, hi) triple).
    void freeze_range(const std::string& lo, const std::string& hi) override {
        janus::get_migration_guard().freeze(table_, lo, hi);
    }
    void unfreeze_range(const std::string& lo, const std::string& hi) override {
        janus::get_migration_guard().unfreeze(table_, lo, hi);
    }
    // Shedding the range IS losing ownership: upgrade the fence to MOVED so
    // stale-routed reads (not just writes) get the retryable rejection until
    // their config reloads. Rides drop_range -- no extra RPC or master step.
    // The tombstone goes up BEFORE the first delete: a reader between a
    // delete and a later mark would pass the (frozen = reads-allowed) fence,
    // see a clean miss on half-emptied data, and panic workload invariants
    // (observed live: payment ALWAYS_ERROR on the migrated warehouse during
    // the drop window). Marked first, every read from the instant ownership
    // is lost gets the retryable abort; pre-mark reads still see full data.
    void drop_range(const std::string& lo, const std::string& hi) override {
        engine_init_this_thread(db_);
        janus::get_migration_guard().mark_moved(table_, lo, hi);
        janus::ShardData::drop_range(lo, hi);   // scan+remove via the gated primitives
    }

private:
    abstract_db* db_;                        // non-owning; for thread registration
    janus::OrderedIndexShardData inner_;
    std::string addr_;                       // this shard's data-plane host:port
    std::string table_;                      // this entry's table name
};

// A per-WAREHOUSE workload index as a migration participant. The physical
// index IS the warehouse (separate tree per warehouse partition), so every
// range operation widens [lo, hi) to the WHOLE keyspace: the master drives
// the migration with the logical table's warehouse_route_key range (what the
// commit publishes for routing), while the physical row keys carry the
// shard-local warehouse id -- the two key spaces never intersect, and the
// index-granular truth is "migrate everything in this tree". Fences land on
// the PHYSICAL table name over the full range, which is exactly the staging
// fence's lookup key (t->get_table_name()) for txn writes to this index.
// @unsafe - delegates to the storage engine; non-owning db/index.
class WarehouseShardData : public EngineShardData {
public:
    using EngineShardData::EngineShardData;

    // The whole-keyspace widening bounds. The UPPER bound is a sentinel above
    // every real key, NOT "": the range machinery treats hi as a plain
    // exclusive lexicographic bound (chunk loops guard cur < hi; scans pass
    // it to the engine), so an empty hi means "before everything" and turns
    // every op into a no-op (observed live: 15ms migrations moving nothing,
    // then checksum aborts). TPC-C encoded keys start with a small
    // big-endian field, so 16 x 0xff dominates all of them.
    static std::string widen_hi() { return std::string(16, '\xff'); }

    // Full-index reads must be CHUNKED, never one one-op scan: the engine
    // registers a read-set item per visited row and Transaction's item set
    // hard-caps at tset_max_capacity (32768) -- a whole customer warehouse
    // (~90k rows) runs the chunk-pointer array off the end and crashes on a
    // garbage TransItem (both shards segfaulted live at exactly the customer
    // migration's checksum scan). Each chunk is its own one-op txn, well
    // under the cap; the limited scan stops registering at the limit.
    std::vector<KvPair> scan_range(const std::string&, const std::string&) override {
        static const size_t kScanChunk = 4096;
        std::vector<KvPair> out;
        std::string cur;   // "" = -inf
        while (true) {
            std::vector<KvPair> batch =
                EngineShardData::scan_range_limited(cur, widen_hi(), kScanChunk);
            const bool last = batch.size() < kScanChunk;
            for (auto& kv : batch) out.push_back(std::move(kv));
            if (last) break;
            cur = out.back().first;
            cur.push_back('\0');   // resume strictly after the last key
        }
        return out;
    }
    std::vector<KvPair> scan_range_limited(const std::string& lo, const std::string&,
                                           size_t limit) override {
        // Chunked copy resumes from `lo` (the successor of the last copied
        // key); only the UPPER bound widens, or every chunk would rescan
        // from the start and the copy would never advance.
        return EngineShardData::scan_range_limited(lo, widen_hi(), limit);
    }
    void copy_range_from(janus::ShardData* source, const std::string&,
                         const std::string&) override {
        EngineShardData::copy_range_from(source, std::string(), widen_hi());
    }
    void drop_range(const std::string&, const std::string&) override {
        EngineShardData::drop_range(std::string(), widen_hi());
    }
    void freeze_range(const std::string&, const std::string&) override {
        EngineShardData::freeze_range(std::string(), widen_hi());
    }
    void unfreeze_range(const std::string&, const std::string&) override {
        EngineShardData::unfreeze_range(std::string(), widen_hi());
    }
};

// The production catalog: named tables created lazily as standalone fixed-id
// engine indexes (ids from 9100), registered in the table registry so the
// non-txn write handler can resolve an op's table name for the per-table
// migration freeze. Warehouse specs ("wh:<gwid>:<logical>") instead resolve
// the REAL workload index through the warehouse directory -- the source's
// startup index, or the destination's adopted index materialized on demand.
// One instance per process; entries are process-lifetime.
// @unsafe - guarded map of leaked engine tables.
class EngineShardCatalog : public janus::ShardDataCatalog {
public:
    EngineShardCatalog(abstract_db* db, std::string own_addr, int my_shard)
        : db_(db), own_addr_(std::move(own_addr)), my_shard_(my_shard) {}

    janus::ShardData* get_or_create(const std::string& table) override {
        if (table.empty()) return nullptr;
        auto lk = mu_.lock().unwrap();
        auto it = tables_.find(table);
        if (it != tables_.end()) return it->second;

        int gwid = 0;
        std::string logical;
        if (parse_warehouse_spec(table, &gwid, &logical)) {
            // The directory's opener runs open_index -> needs an engine-
            // registered thread (rrr handler threads are not).
            engine_init_this_thread(db_);
            ::FullOrderedIndex* idx =
                get_warehouse_directory().local_for_migration(logical, gwid,
                                                              my_shard_);
            if (idx == nullptr) return nullptr;   // no TPC-C directory here
            // Fence entries must carry the PHYSICAL table name (what the
            // staging fence resolves from the index at write time).
            const std::string physical =
                get_table_registry().get_table_name(idx->get_table_id())
                    .unwrap_or(table);
            auto* sd = new WarehouseShardData(db_, idx, own_addr_, physical);  // leaked
            tables_.emplace(table, sd);
            return sd;
        }

        const long id = next_id_.fetch_add(1);
        ::FullOrderedIndex* idx = make_standalone_index(table, id);
        if (idx == nullptr) return nullptr;
        // Name<->id registration: lets RunNontxnOp resolve an op's table NAME to
        // query the migration guard, and the router govern the table by name.
        get_table_registry().register_table(static_cast<int>(id), table);
        auto* sd = new EngineShardData(db_, idx, own_addr_, table);  // leaked
        tables_.emplace(table, sd);
        return sd;
    }

private:
    abstract_db* db_;                                    // non-owning
    std::string own_addr_;                               // this shard's data-plane host:port
    int my_shard_;                                       // this shard's index (adoption opens)
    rusty::Mutex<int> mu_{0};                            // guards tables_/creation
    std::map<std::string, janus::ShardData*> tables_;
    std::atomic<long> next_id_{9100};                    // outside workload windows
};

// The config store behind the same gate: the ConfigWatcher poll thread and the
// ConfigKvService rrr handler thread hit the __mako_config__ index directly.
// @unsafe - delegates to the storage engine; non-owning db/index.
class EngineKvStore : public janus::KvStore {
public:
    EngineKvStore(abstract_db* db, ::FullOrderedIndex* idx)
        : db_(db), inner_(idx) {}

    rusty::Option<std::string> get(const std::string& key) override {
        engine_init_this_thread(db_);
        return inner_.get(key);
    }
    void put(const std::string& key, const std::string& value) override {
        engine_init_this_thread(db_);
        inner_.put(key, value);
    }
    void remove(const std::string& key) override {
        engine_init_this_thread(db_);
        inner_.remove(key);
    }

private:
    abstract_db* db_;                    // non-owning; for thread registration
    janus::OrderedIndexKvStore inner_;
};

}  // namespace

janus::ShardDataCatalog* make_engine_shard_catalog(abstract_db* db,
                                                   const std::string& own_addr,
                                                   int my_shard) {
    if (db == nullptr) return nullptr;
    // Leaked: process-lifetime, like the bootstrap's other singletons.
    return new EngineShardCatalog(db, own_addr, my_shard);
}

void engine_register_this_thread(abstract_db* db) {
    engine_init_this_thread(db);
}

janus::KvStore* make_engine_kv_store(abstract_db* db, ::FullOrderedIndex* idx) {
    if (db == nullptr || idx == nullptr) return nullptr;
    // Leaked: process-lifetime, like the bootstrap's other singletons.
    return new EngineKvStore(db, idx);
}

void seed_shard_data(janus::ShardData* sd, int count) {
    if (sd == nullptr || count <= 0) return;
    // Fresh thread: registers itself through the decorator's gate on first put.
    std::thread t([sd, count] {
        for (int i = 0; i < count; i++) {
            char kb[16];
            snprintf(kb, sizeof kb, "d%02d", i);
            sd->put(kb, std::string("v") + std::to_string(i));
        }
    });
    t.join();
}

}  // namespace mako
