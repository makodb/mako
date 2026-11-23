#include "storage/rust_sto_sharding_host.hh"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "silo_runtime.h"
#include "storage/mbta_wrapper.hh"
#include "storage/rust_sto_tpcc_wrapper.hh"

// Host-private exports of crates/sto-tpcc-ffi/src/shard_host.rs. They stay
// out of the installed C header: this file establishes their preconditions
// (a live table that outlives its registration, a thread handle registered on
// the thread that created it and unregistered before it is destroyed). None
// of them unwinds. The three verbs are declared with the callback table's own
// types and are assigned to its slots without a cast, so a prototype that
// differs from the slot does not compile. That these prototypes agree with
// the Rust definitions is not checked by a tool; the fixture
// (examples/rust_sharding_storage_fixture.cc) calls every one of them.
extern "C" {
uint32_t mako_sto_shard_table_register(uint64_t catalog_table, uint32_t owner,
                                       const sto_tpcc_table* table,
                                       uint32_t table_id) noexcept;
uint32_t mako_sto_shard_table_unregister(uint64_t catalog_table, uint32_t owner) noexcept;
uint32_t mako_sto_shard_thread_register(void* context, uint32_t owner,
                                        sto_tpcc_thread* thread) noexcept;
uint32_t mako_sto_shard_thread_unregister(void* context) noexcept;
uint32_t mako_sto_shard_scan_next(void*, uint64_t table, MakoShardBytes lo, uint32_t has_hi,
                                  MakoShardBytes hi, uint32_t has_after,
                                  MakoShardBytes after_coordinate, MakoShardBytes after_key,
                                  MakoShardRowSink, void* sink_context) noexcept;
uint32_t mako_sto_shard_put(void*, uint64_t table, MakoShardBytes coordinate,
                            MakoShardBytes key, MakoShardBytes value) noexcept;
uint32_t mako_sto_shard_remove(void*, uint64_t table, MakoShardBytes coordinate,
                               MakoShardBytes key) noexcept;
}

// The Rust module declares its own span type and status numbers
// (MakoStoShardBytes, SHARD_*); these are the C++ half of that agreement.
static_assert(sizeof(MakoShardBytes) == 16 && alignof(MakoShardBytes) == 8);
static_assert(offsetof(MakoShardBytes, data) == 0 && offsetof(MakoShardBytes, len) == 8);
static_assert(std::is_standard_layout_v<MakoShardBytes>
              && std::is_trivially_copyable_v<MakoShardBytes>);
static_assert(MAKO_SHARD_OK == 0 && MAKO_SHARD_NOT_FOUND == 1 && MAKO_SHARD_RETRY == 2
              && MAKO_SHARD_INVALID == 3 && MAKO_SHARD_BUSY == 4 && MAKO_SHARD_EXHAUSTED == 5
              && MAKO_SHARD_IO == 6);

namespace mako {
namespace {

// The host whose thread_enter succeeded on this OS thread.
thread_local NativeShardingHost* entered_host = nullptr;

struct Registered {
    uint64_t table;
    uint32_t owner;
    // The value the sharding crate holds as this table's handle.
    abstract_ordered_index* index;
};
struct Registry {
    rusty::Vec<Registered> tables;
    bool closed = false;
};
// Never held across a call into the sharding crate: `open` runs under two of
// the crate's own mutexes and takes this one.
rusty::Mutex<Registry> registry{Registry{}};

// @unsafe - owner-bound registration of one Rust-engine thread. Called once,
// first thing, on the crate's storage thread of this owner.
uint32_t thread_enter(void* context, uint32_t owner) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host || owner != host.owner) return MAKO_SHARD_INVALID;
    if (!dynamic_cast<rust_sto_tpcc_wrapper*>(host.db)) return MAKO_SHARD_INVALID;
    bool attached = false;
    bool registered = false;
    try {
        // open_index, thread_init and every getShardIndex() on this thread
        // answer `owner` from here on.
        BenchmarkConfig::setThreadLocalShardIndex(static_cast<int>(owner));
        // The engine's Masstree attach refuses a thread whose current runtime
        // is not the one the database was created with.
        host.runtime->BindToCurrentThread();
        // One STO thread id and one engine handle; loader=true creates no
        // ShardClient and is not kept as a mode after initialisation.
        host.db->thread_init(true);
        attached = true;
        TThread::in_loading_phase = false;
        TThread::set_shard_index(static_cast<int>(owner));
        auto* thread = rust_sto_tpcc_wrapper::sharding_host_idle_thread();
        if (!thread) throw std::runtime_error("no idle engine handle after thread_init");
        if (mako_sto_shard_thread_register(context, owner, thread) != MAKO_SHARD_OK)
            throw std::runtime_error("the Rust verbs refused the thread handle");
        registered = true;
        entered_host = &host;
        // @unsafe { stdio is not borrow-checked }
        fprintf(stderr, "rust_sharding_host: entered owner=%u slot=%d\n", owner, TThread::id());
        return MAKO_SHARD_OK;
    } catch (const std::exception& error) {
        // @unsafe { stdio is not borrow-checked }
        fprintf(stderr, "rust_sharding_host: thread_enter failed for owner=%u: %s\n", owner,
                error.what());
    } catch (...) {
        // @unsafe { stdio is not borrow-checked }
        fprintf(stderr, "rust_sharding_host: thread_enter failed for owner=%u\n", owner);
    }
    if (registered) (void)mako_sto_shard_thread_unregister(context);
    if (attached) {
        try {
            host.db->thread_end();
        } catch (...) {
        }
    }
    entered_host = nullptr;
    BenchmarkConfig::clearThreadLocalShardIndex();
    SiloRuntime::BindCurrentThread(nullptr);
    MasstreeContext::BindCurrentThread(nullptr);
    return MAKO_SHARD_IO;
}

// @unsafe - once, after the last native job on this engine thread. The Rust
// verbs lose the handle before thread_end destroys it.
void thread_leave(void* context) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host != &host) return;
    const uint32_t unregistered = mako_sto_shard_thread_unregister(context);
    if (unregistered != MAKO_SHARD_OK) {
        // @unsafe { stdio is not borrow-checked }
        fprintf(stderr, "rust_sharding_host: thread unregister failed for owner=%u: status %u\n",
                host.owner, unregistered);
    }
    try {
        host.db->thread_end();
    } catch (...) {
        // An exception must not leave a callback the crate calls.
        // @unsafe { stdio is not borrow-checked }
        fprintf(stderr, "rust_sharding_host: thread_end failed for owner=%u\n", host.owner);
    }
    // @unsafe { stdio is not borrow-checked }
    fprintf(stderr, "rust_sharding_host: left owner=%u\n", host.owner);
    entered_host = nullptr;
    BenchmarkConfig::clearThreadLocalShardIndex();
    SiloRuntime::BindCurrentThread(nullptr);
    MasstreeContext::BindCurrentThread(nullptr);
}

// @unsafe - never creates a table: answers only handles registered at
// start-up. A table cannot be created on a thread that holds an engine
// handle, and the crate calls this on such a thread. The name is ignored.
uint32_t open(void* context, uint64_t table, MakoShardBytes, uint32_t owner, uint32_t proxy,
              uint32_t kind, uintptr_t* handle) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (owner != host.owner || !handle || proxy > 1 || kind > 2) return MAKO_SHARD_INVALID;
    if (proxy) return MAKO_SHARD_INVALID; // no proxies in this slice
    auto state = registry.lock().unwrap();
    for (const auto& entry : state->tables) {
        if (entry.table == table && entry.owner == owner) {
            *handle = reinterpret_cast<uintptr_t>(entry.index);
            return MAKO_SHARD_OK;
        }
    }
    return MAKO_SHARD_INVALID;
}

} // namespace

// @unsafe - copies the C++ host's table and replaces the six engine slots.
// The verbs use term 0 and drop the log record, which is right only on a
// non-replicated node.
MakoShardHost rust_sharding_callbacks(NativeShardingHost& base) {
    if (!dynamic_cast<rust_sto_tpcc_wrapper*>(base.db))
        throw std::logic_error("rust_sharding_callbacks needs a Rust-engine database");
    if (BenchmarkConfig::getInstance().getIsReplicated())
        throw std::logic_error("Rust sharding storage is non-replicated only");
    registry.lock().unwrap()->closed = true;
    MakoShardHost table = base.callbacks(); // context = &base
    table.thread_enter = thread_enter;
    table.thread_leave = thread_leave;
    table.open = open;
    table.scan_next = mako_sto_shard_scan_next;
    table.put = mako_sto_shard_put;
    table.remove = mako_sto_shard_remove;
    return table; // peer_call, epoch_cut, epoch_drained unchanged
}

// @unsafe - start-up registration with the Rust verbs and the sharding crate.
uint32_t rust_sharding_register_raw(uint64_t table, uint32_t owner,
                                    abstract_ordered_index* index) {
    try {
        auto* rust = dynamic_cast<rust_sto_tpcc_ordered_index*>(index);
        if (!rust) return MAKO_SHARD_INVALID;
        const sto_tpcc_table* native = rust->sharding_host_table();
        if (!native) return MAKO_SHARD_INVALID; // a remote facade owns no rows
        uint32_t coordinates = UINT32_MAX;
        if (mako_sharding_table_coordinate(table, &coordinates) != MAKO_SHARD_OK
            || coordinates != 0)
            return MAKO_SHARD_INVALID; // the verbs treat a coordinate as the key
        // The protocol's log record has room for ids below 0x8000 only, and
        // the index must have been opened in the owner's id window.
        const int32_t id = index->get_table_id();
        if (id <= 0 || id >= 0x8000
            || static_cast<uint32_t>((id - 1) / NUM_TABLES_PER_SHARD) != owner)
            return MAKO_SHARD_INVALID;
        {
            auto state = registry.lock().unwrap();
            if (state->closed) return MAKO_SHARD_INVALID;
            for (const auto& entry : state->tables) {
                const bool same_key = entry.table == table && entry.owner == owner;
                if (same_key && entry.index == index) return MAKO_SHARD_OK;
                if (same_key || entry.index == index) return MAKO_SHARD_INVALID;
            }
        }
        // The registry mutex is released here: mako_sharding_raw_register
        // takes the crate's handle mutex, and `open` takes the two in the
        // other order. Registration is start-up only and single-threaded.
        const uint32_t verbs = mako_sto_shard_table_register(table, owner, native,
                                                             static_cast<uint32_t>(id));
        if (verbs != MAKO_SHARD_OK) return verbs;
        // The handle is the abstract_ordered_index* itself, as the C++ host
        // gives it; the adapter index has four bases, so the cast result
        // above is not the same address.
        const uint32_t crate = mako_sharding_raw_register(table, owner, 0,
                                                          reinterpret_cast<uintptr_t>(index));
        if (crate != MAKO_SHARD_OK) {
            (void)mako_sto_shard_table_unregister(table, owner);
            return crate;
        }
        try {
            registry.lock().unwrap()->tables.push(Registered{table, owner, index});
        } catch (...) {
            // Without the entry rust_sharding_release would leave the verbs a
            // pointer to a table that is about to be destroyed. The crate has
            // no call that takes a handle back; it keeps one it cannot use.
            (void)mako_sto_shard_table_unregister(table, owner);
            throw;
        }
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @unsafe - the Rust verbs forget every table; the indexes may then go.
void rust_sharding_release() noexcept {
    auto state = registry.lock().unwrap();
    for (const auto& entry : state->tables) {
        const uint32_t status = mako_sto_shard_table_unregister(entry.table, entry.owner);
        if (status != MAKO_SHARD_OK) {
            // @unsafe { stdio is not borrow-checked }
            fprintf(stderr,
                    "rust_sharding_host: table unregister failed for owner=%u table=%llu: "
                    "status %u\n",
                    entry.owner, static_cast<unsigned long long>(entry.table), status);
        }
    }
    state->tables.clear();
    state->closed = false;
}

} // namespace mako
