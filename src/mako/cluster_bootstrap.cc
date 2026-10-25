#include "cluster_bootstrap.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include "native_sharding_host.h"
#include "benchmarks/benchmark_config.h"
#include "silo_runtime.h"
#include "storage/abstract_db.h"
#include "srpc_log.h"

namespace janus {
namespace {
rusty::Mutex<rusty::Vec<rusty::Option<rusty::Box<mako::NativeShardingHost>>>> hosts{
    rusty::Vec<rusty::Option<rusty::Box<mako::NativeShardingHost>>>{}};

// @unsafe - process environment is read only during bootstrap.
bool enabled() {
    const char* value = std::getenv("MAKO_CLUSTER_CONFIG");
    return value && std::strcmp(value, "1") == 0;
}
} // namespace

// @unsafe - publishes an owner only after its real data load completes. The
// host table is lifecycle ownership only; all sharding decisions live in Rust.
void BootstrapClusterConfig(abstract_db* db) {
    if (!enabled()) return;
    auto& bench = BenchmarkConfig::getInstance();
    if (!db || !bench.getConfig() || bench.getNshards() == 0
        || bench.getNshards() > std::numeric_limits<uint32_t>::max()
        || bench.getShardIndex() >= bench.getNshards())
        throw abstract_db::abstract_abort_exception();
    const auto owner = static_cast<uint32_t>(bench.getShardIndex());
    const auto nshards = static_cast<uint32_t>(bench.getNshards());
    auto local = hosts.lock().unwrap();
    while (local->size() < nshards) local->push(rusty::None);
    if ((*local)[owner].is_some()) {
        if ((*local)[owner].as_ref().unwrap()->db != db)
            throw abstract_db::abstract_abort_exception();
        return;
    }
    auto* runtime = SiloRuntime::Current();
    if (bench.getConfig()->multi_shard_mode) {
        auto* shard = bench.getShardContext(owner);
        if (!shard || !shard->runtime.get()) throw abstract_db::abstract_abort_exception();
        runtime = const_cast<SiloRuntime*>(shard->runtime.get());
    }
    auto host = rusty::make_box<mako::NativeShardingHost>(db, owner, nshards, runtime);
    const auto callbacks = host->callbacks();
    const uint32_t status = mako_sharding_start_node(owner, nshards,
        bench.getIsReplicated() == 0 ? 1 : 0, &callbacks);
    if (status != MAKO_SHARD_OK) {
        srpc::Log_error("Native sharding start rejected owner {} (status {})", owner, status);
        throw abstract_db::abstract_abort_exception();
    }
    const uint32_t listening = host->start_service();
    if (listening != MAKO_SHARD_OK) {
        host->stop_accepting();
        mako_sharding_stop_node(owner);
        srpc::Log_error("Native sharding service failed for owner {} (status {})", owner, listening);
        throw abstract_db::abstract_abort_exception();
    }
    (*local)[owner] = rusty::Some(std::move(host));
}

// @unsafe - ordinary workers have stopped; index storage still exists. The
// poll thread remains live while native completions and peer calls are joined.
void ShutdownClusterConfig(uint32_t owner) {
    auto local = hosts.lock().unwrap();
    if (owner >= local->size() || (*local)[owner].is_none()) return;
    (*local)[owner].as_ref().unwrap()->stop_accepting();
    mako_sharding_stop_node(owner);
    (*local)[owner] = rusty::None;
}

// @unsafe - first fence every local listener, then join every native queue,
// then release RPC ownership. No background borrow survives database teardown.
void ShutdownClusterConfig() {
    auto local = hosts.lock().unwrap();
    for (auto& host : *local)
        if (host.is_some()) host.as_ref().unwrap()->stop_accepting();
    for (auto& host : *local)
        if (host.is_some()) mako_sharding_stop_node(host.as_ref().unwrap()->owner);
    local->clear();
}
} // namespace janus
