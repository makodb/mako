#include "storage/rust_sto_sharding_host.hh"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <exception>
#include <map>
#include <optional>
#include <tuple>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>
#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "lib/table_registry.h"
#include "sto/Transaction.hh"
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
uint32_t mako_sto_shard_warehouse_register(uint64_t, uint32_t owner, uint32_t warehouse,
                                          uint32_t total, const sto_tpcc_table*, uint32_t id) noexcept;
uint32_t mako_sto_shard_warehouse_unregister(uint64_t, uint32_t owner, uint32_t warehouse) noexcept;
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
    abstract_ordered_index* index;
    rusty::Box<rust_sto_tpcc_ordered_index> proxy;
    uint32_t kind;
};
struct Registry {
    std::map<std::tuple<uint32_t, uint64_t, uint32_t>, Registered> tables;
    std::set<uint32_t> closed;
};
// Serializes registration, startup and release. open() only takes registry,
// because the sharding crate may hold its raw-handle mutex when it calls open.
std::mutex lifecycle;
rusty::Mutex<Registry> registry{Registry{}};

// @unsafe - owner-bound registration of one Rust-engine thread. Called once,
// first thing, on the crate's storage thread of this owner.
uint32_t thread_enter(void* context, uint32_t owner) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host || owner != host.owner) return MAKO_SHARD_INVALID;
    if (!dynamic_cast<rust_sto_tpcc_wrapper*>(host.db)) return MAKO_SHARD_INVALID;
    bool attached = false;
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
    // Only a failure comes here, and it happened before the Rust verbs took
    // the thread handle: there is nothing to unregister.
    if (attached) {
        try {
            host.db->thread_end();
        } catch (...) {
        }
    }
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

// @unsafe - both handles were built before attachment. The physical index
// carries the local rows, and its stable proxy carries only the routing ID.
uint32_t select_handle(uint64_t table, uint32_t owner, uint32_t warehouse, uint32_t proxy,
                       uintptr_t* handle, uint32_t kind) {
    if (!handle || proxy > 1) return MAKO_SHARD_INVALID;
    try {
        auto state = registry.lock().unwrap();
        const auto at = state->tables.find({owner, table, warehouse});
        if (at == state->tables.end() || kind != at->second.kind) return MAKO_SHARD_INVALID;
        FullOrderedIndex* selected = proxy ? at->second.proxy.get() : at->second.index;
        *handle = reinterpret_cast<uintptr_t>(selected);
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

uint32_t open(void* context, uint64_t table, MakoShardBytes, uint32_t owner,
              uint32_t proxy, uint32_t kind, uintptr_t* handle) {
    if (owner != static_cast<NativeShardingHost*>(context)->owner) return MAKO_SHARD_INVALID;
    return select_handle(table, owner, 0, proxy, handle, kind);
}

uint32_t open_warehouse(void*, uint64_t table, uint32_t warehouse, uint32_t owner,
                        uint32_t proxy, uintptr_t* handle) {
    // A cold-start failure has no retained, stopped node to fence a cached handle.
    if (!warehouse || !mako_sharding_active(owner)) return MAKO_SHARD_INVALID;
    return select_handle(table, owner, warehouse, proxy, handle, 0);
}

uint32_t unregister_table(uint32_t owner, uint64_t table, uint32_t warehouse) noexcept {
    return warehouse ? mako_sto_shard_warehouse_unregister(table, owner, warehouse)
                     : mako_sto_shard_table_unregister(table, owner);
}

void release_table(uint32_t owner, uint64_t table, uint32_t warehouse, Registered& entry) noexcept {
    if (mako_sharding_active(owner)) std::terminate();
    if (warehouse == 0) {
        if (mako_sharding_raw_unregister(table, owner, 0, reinterpret_cast<uintptr_t>(entry.index))
                != MAKO_SHARD_OK)
            std::terminate();
        // The native directory opens this proxy only on the first remote route.
        const auto proxy = mako_sharding_raw_unregister(
            table, owner, 1, reinterpret_cast<uintptr_t>(entry.proxy.get()));
        if (proxy != MAKO_SHARD_OK && proxy != MAKO_SHARD_NOT_FOUND) std::terminate();
    }
    if (unregister_table(owner, table, warehouse) != MAKO_SHARD_OK)
        std::terminate(); // a borrowed Rust pointer must not outlive its table
    get_table_registry().forget_raw_handle(entry.index->get_table_id(), entry.index);
}

void release_tables(std::optional<uint32_t> owner, rust_sto_tpcc_wrapper* db = nullptr) noexcept {
    std::lock_guard guard(lifecycle);
    decltype(Registry::tables) removed;
    {
        auto state = registry.lock().unwrap();
        for (auto at = state->tables.begin(); at != state->tables.end();) {
            auto current = at++;
            const auto registered_owner = std::get<0>(current->first);
            if ((!owner || *owner == registered_owner) &&
                (!db || db->get_index_by_table_id(current->second.index->get_table_id()) == current->second.index)) {
                removed.insert(state->tables.extract(current));
                auto next = state->tables.lower_bound({registered_owner, 0, 0});
                if (next == state->tables.end() || std::get<0>(next->first) != registered_owner)
                    state->closed.erase(registered_owner);
            }
        }
    }
    // A live sibling can hold the directory mutex while it calls open(), so
    // release the C++ registry mutex before entering the native directory.
    for (auto& [key, entry] : removed) {
        const auto& [registered_owner, table, warehouse] = key;
        release_table(registered_owner, table, warehouse, entry);
    }
}

} // namespace

MakoShardHost rust_sharding_callbacks(NativeShardingHost& base) {
    auto* db = dynamic_cast<rust_sto_tpcc_wrapper*>(base.db);
    if (!db) throw std::logic_error("Rust sharding requires a Rust-engine database");
    if (BenchmarkConfig::getInstance().getIsReplicated())
        throw std::logic_error("Rust sharding requires a non-replicated engine");
    const bool warehouses = mako_sharding_warehouse_count() > 0;
    std::lock_guard guard(lifecycle);
    auto state = registry.lock().unwrap();
    auto first = state->tables.lower_bound({base.owner, 0, 0});
    if (first == state->tables.end() || std::get<0>(first->first) != base.owner)
        throw std::logic_error("register local indexes before starting Rust sharding");
    for (auto at = first; at != state->tables.end() && std::get<0>(at->first) == base.owner; ++at) {
        auto* index = at->second.index;
        if (db->get_index_by_table_id(index->get_table_id()) != index)
            throw std::logic_error("index belongs to a different Rust database");
    }
    if (warehouses &&
        mako_sharding_warehouse_opener(base.owner, nullptr, open_warehouse) != MAKO_SHARD_OK)
        throw std::logic_error("warehouse directory has a different engine opener");
    Transaction::start_epoch_advancer();
    state->closed.insert(base.owner);
    base.rust_storage = true;
    MakoShardHost host = base.callbacks();
    host.thread_enter = thread_enter;
    host.thread_leave = thread_leave;
    host.open = open;
    host.scan_next = mako_sto_shard_scan_next;
    host.put = mako_sto_shard_put;
    host.remove = mako_sto_shard_remove;
    return host;
}

static uint32_t register_index(uint64_t table, uint32_t owner, uint32_t warehouse,
                               abstract_ordered_index* index) {
    try {
        auto* rust = dynamic_cast<rust_sto_tpcc_ordered_index*>(index);
        if (!rust || !rust->sharding_host_table()) return MAKO_SHARD_INVALID;
        const int32_t id = index->get_table_id();
        if (id <= 0 || id >= 0x8000
            || static_cast<uint32_t>((id - 1) / NUM_TABLES_PER_SHARD) != owner)
            return MAKO_SHARD_INVALID;
        auto& bindings = get_table_registry();
        auto name = bindings.get_table_name(id);
        if (name.is_none()) return MAKO_SHARD_INVALID;
        const auto& text = name.as_ref().unwrap();
        uint64_t canonical = 0;
        uint32_t kind = 0, coordinates = 0;
        if (mako_sharding_table_coordinate(table, &coordinates) != MAKO_SHARD_OK ||
            coordinates != (warehouse ? 1u : 0u))
            return MAKO_SHARD_INVALID;
        const uint32_t total = mako_sharding_warehouse_count();
        rusty::Option<std::string> coordinate = rusty::None;
        if (warehouse) {
            if (warehouse > total || rust_sto_tpcc_detail::table_has_static_directory(text))
                return MAKO_SHARD_INVALID;
            const char be[4] = {static_cast<char>(warehouse >> 24), static_cast<char>(warehouse >> 16),
                                static_cast<char>(warehouse >> 8), static_cast<char>(warehouse)};
            coordinate = rusty::Some(std::string(be, sizeof(be)));
        } else if (mako_sharding_table_kind({reinterpret_cast<const uint8_t*>(text.data()), text.size()},
                                            &canonical, &kind) != MAKO_SHARD_OK || canonical != table) {
            return MAKO_SHARD_INVALID;
        }
        std::lock_guard guard(lifecycle);
        const auto key = std::make_tuple(owner, table, warehouse);
        {
            auto state = registry.lock().unwrap();
            if (state->closed.contains(owner)) return MAKO_SHARD_INVALID;
            auto at = state->tables.find(key);
            if (at != state->tables.end())
                return at->second.index == index ? MAKO_SHARD_OK : MAKO_SHARD_INVALID;
            state->tables.emplace(key, Registered{index, rust->make_sharding_proxy(), kind});
        }
        // open() for this owner is unreachable until callbacks() closes
        // registration. Publish the crate's irreversible handle last.
        uint32_t result = MAKO_SHARD_IO;
        bool bound = false;
        bool verbs = false;
        try {
            bound = bindings.bind_native_handle(id, table, owner, kind, index, std::move(coordinate));
            if (!bound) result = MAKO_SHARD_INVALID;
            else if (warehouse) result = mako_sto_shard_warehouse_register(
                table, owner, warehouse, total, rust->sharding_host_table(), id);
            else result = mako_sto_shard_table_register(table, owner, rust->sharding_host_table(), id);
            verbs = result == MAKO_SHARD_OK;
            if (verbs && !warehouse) result = mako_sharding_raw_register(
                table, owner, 0, reinterpret_cast<uintptr_t>(index));
        } catch (...) {
            result = MAKO_SHARD_IO;
        }
        if (result != MAKO_SHARD_OK) {
            if (verbs && unregister_table(owner, table, warehouse) != MAKO_SHARD_OK)
                std::terminate();
            if (bound) bindings.forget_raw_handle(id, index);
            registry.lock().unwrap()->tables.erase(key);
        }
        return result;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

uint32_t rust_sharding_register_raw(uint64_t table, uint32_t owner,
                                    abstract_ordered_index* index) {
    return register_index(table, owner, 0, index);
}

uint32_t rust_sharding_register_warehouse(uint64_t table, uint32_t owner,
                                         uint32_t warehouse, abstract_ordered_index* index) {
    if (!warehouse) return MAKO_SHARD_INVALID;
    return register_index(table, owner, warehouse, index);
}

void rust_sharding_release() noexcept {
    release_tables(std::nullopt);
}

} // namespace mako

rusty::Box<rust_sto_tpcc_ordered_index> rust_sto_tpcc_ordered_index::make_sharding_proxy() const {
    return rusty::make_box<rust_sto_tpcc_ordered_index>(owner_, nullptr, table_id_, name_, true);
}

void rust_sto_tpcc_wrapper::release_sharding_indexes() noexcept {
    mako::release_tables(std::nullopt, this);
}

bool rust_sto_tpcc_wrapper::sharding_uses_index(abstract_ordered_index* index) const {
    auto state = mako::registry.lock().unwrap();
    for (const auto& [key, entry] : state->tables)
        if (entry.index == index) return true;
    return false;
}

void rust_sto_tpcc_wrapper::configure_native_sharding(mako::NativeShardingHost& host,
                                                       MakoShardHost& callbacks) {
    callbacks = mako::rust_sharding_callbacks(host);
}

bool rust_sto_tpcc_wrapper::preopen_native_warehouses(uint64_t table, uint32_t owner,
                                                    uint32_t total) {
    try {
        for (uint32_t warehouse = 1; warehouse <= total; ++warehouse) {
            const auto name = "tpcc_native_" + std::to_string(table) + "_warehouse_"
                + std::to_string(warehouse) + "_owner_" + std::to_string(owner) + "_bytes";
            auto* physical = open_index(name, static_cast<int>(owner));
            if (mako::rust_sharding_register_warehouse(table, owner, warehouse, physical) != MAKO_SHARD_OK)
                throw std::runtime_error("could not register warehouse index " + name);
        }
    } catch (...) {
        native_sharding_stopped(owner);
        throw;
    }
    return true;
}

void rust_sto_tpcc_wrapper::native_sharding_stopped(uint32_t owner) noexcept {
    mako::release_tables(owner, this);
}
