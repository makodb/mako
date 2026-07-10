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
    // Freeze goes to the process-global MigrationGuard (no engine work).
    void freeze_range(const std::string& lo, const std::string& hi) override {
        inner_.freeze_range(lo, hi);
    }
    void unfreeze_range(const std::string& lo, const std::string& hi) override {
        inner_.unfreeze_range(lo, hi);
    }

private:
    abstract_db* db_;                        // non-owning; for thread registration
    janus::OrderedIndexShardData inner_;
    std::string addr_;                       // this shard's data-plane host:port
    std::string table_;                      // this entry's table name
};

// The production catalog: named tables created lazily as standalone fixed-id
// engine indexes (ids from 9100), registered in the table registry so the
// non-txn write handler can resolve an op's table name for the per-table
// migration freeze. One instance per process; entries are process-lifetime.
// @unsafe - guarded map of leaked engine tables.
class EngineShardCatalog : public janus::ShardDataCatalog {
public:
    EngineShardCatalog(abstract_db* db, std::string own_addr)
        : db_(db), own_addr_(std::move(own_addr)) {}

    janus::ShardData* get_or_create(const std::string& table) override {
        if (table.empty()) return nullptr;
        auto lk = mu_.lock().unwrap();
        auto it = tables_.find(table);
        if (it != tables_.end()) return it->second;
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
                                                   const std::string& own_addr) {
    if (db == nullptr) return nullptr;
    // Leaked: process-lifetime, like the bootstrap's other singletons.
    return new EngineShardCatalog(db, own_addr);
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
