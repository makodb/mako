#include "native_sharding_host.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include "benchmarks/benchmark_config.h"
#include "lib/sharding_leases.h"
#include "lib/table_registry.h"
#include "silo_runtime.h"
#include "storage/mbta_wrapper.hh"

namespace mako {
namespace {
thread_local NativeShardingHost* entered_host = nullptr;

// @unsafe - borrowed native byte spans, never retained past a callback.
std::string_view view(MakoShardBytes value) {
    return {value.len ? reinterpret_cast<const char*>(value.data) : "", value.len};
}
// @unsafe - generated engine wire strings own these spans during the callback.
MakoShardBytes bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// @unsafe - brackets only raw engine calls made under Rust's transfer guard.
// No ordinary lease can be opened while the participant mutex is held here.
struct KernelTransaction {
    bool entered;
    KernelTransaction() : entered(sharding_enter_native_kernel()) {}
    ~KernelTransaction() {
        if (!entered) return;
        if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
        sharding_leave_native_kernel();
    }
};

// @unsafe - owner-bound thread registration; loader=true avoids constructing an
// ordinary ShardClient, but is NOT used as an authorization bypass after init.
uint32_t thread_enter(void* context, uint32_t owner) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host || owner != host.owner || TThread::txn) return MAKO_SHARD_INVALID;
    try {
        BenchmarkConfig::setThreadLocalShardIndex(static_cast<int>(owner));
        host.runtime->BindToCurrentThread();
        host.db->thread_init(true);
        TThread::in_loading_phase = false;
        TThread::set_shard_index(static_cast<int>(owner));
        entered_host = &host;
        return MAKO_SHARD_OK;
    } catch (...) {
        BenchmarkConfig::clearThreadLocalShardIndex();
        SiloRuntime::BindCurrentThread(nullptr);
        MasstreeContext::BindCurrentThread(nullptr);
        return MAKO_SHARD_IO;
    }
}

// @unsafe - called once after the last native job on this engine thread. Legacy
// Masstree threadinfo is RCU-owned; thread_end is its supported release hook.
void thread_leave(void* context) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host != &host) return;
    {
        KernelTransaction kernel;
        if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
        auto transaction = rusty::Box<Transaction>::from_raw(TThread::txn);
        TThread::txn = nullptr;
    }
    host.db->thread_end();
    entered_host = nullptr;
    BenchmarkConfig::clearThreadLocalShardIndex();
    SiloRuntime::BindCurrentThread(nullptr);
    MasstreeContext::BindCurrentThread(nullptr);
}

// @unsafe - Rust serializes opening and supplies an authoritative canonical
// binding. Actual local table slots come from the DB, never numeric arithmetic.
uint32_t open(void* context, uint64_t table, MakoShardBytes name, uint32_t owner,
                uint32_t proxy, uint32_t kind, uintptr_t* handle) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (owner != host.owner || !handle || proxy > 1 || kind > 2) return MAKO_SHARD_INVALID;
    try {
        const std::string native_name(view(name));
        auto* index = host.db->open_index(native_name, static_cast<int>(owner));
        if (!index) return MAKO_SHARD_IO;
        auto& registry = get_table_registry();
        auto previous = registry.native_binding(index->get_table_id());
        if (previous.is_none()) {
            if (proxy) sharding_mark_proxy(index);
            registry.register_table(index->get_table_id(), native_name,
                                      static_cast<int>(owner), proxy == 0, index);
            if (!registry.bind_native(native_name, table, rusty::None, kind))
                return MAKO_SHARD_INVALID;
        } else if (previous.as_ref().unwrap()->table != table
                   || previous.as_ref().unwrap()->kind != kind
                   || previous.as_ref().unwrap()->fixed_coordinate.is_some()) {
            return MAKO_SHARD_INVALID;
        }
        if (index->get_is_remote() != (proxy != 0)) return MAKO_SHARD_INVALID;
        *handle = reinterpret_cast<uintptr_t>(index);
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @safe - canonical BE4 warehouse wire coordinate; not a physical index ID.
uint32_t warehouse_prefix(std::string_view coordinate) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value = (value << 8) | (i < coordinate.size()
            ? static_cast<unsigned char>(coordinate[i]) : 0);
    return value;
}
// @unsafe - narrow coordinate serialization at the engine boundary.
std::string warehouse_coordinate(uint32_t warehouse) {
    const char data[4] = {static_cast<char>(warehouse >> 24),
        static_cast<char>(warehouse >> 16), static_cast<char>(warehouse >> 8),
        static_cast<char>(warehouse)};
    return std::string(data, 4);
}

// @unsafe - materialization uses Rust's independent warehouse handle registry,
// not participant admission. The reverse map excludes origin-side proxies.
mbta_table* physical(NativeShardingHost& host, uint64_t table, bool fixed,
                      std::string_view coordinate) {
    uintptr_t materialized = 0;
    if (fixed) {
        if (coordinate.size() != 4) return nullptr;
        const uint32_t warehouse = warehouse_prefix(coordinate);
        if (warehouse == 0 || warehouse > mako_sharding_warehouse_count()) return nullptr;
        if (mako_sharding_warehouse_local(table, warehouse, host.owner, &materialized)
            != MAKO_SHARD_OK) return nullptr;
    }
    auto handle = get_table_registry().native_handle(table, host.owner, fixed, coordinate);
    if (handle.is_none()) return nullptr;
    auto* registered = handle.unwrap();
    if (fixed && materialized != reinterpret_cast<uintptr_t>(registered)) return nullptr;
    auto* index = dynamic_cast<mbta_ordered_index*>(registered);
    if (!index || index->get_is_remote()) return nullptr;
    return index->mbta;
}

// @unsafe - one ordered engine lookahead. Successful commit precedes publication
// of copied bytes. OCC conflicts/errors never masquerade as end-of-range.
uint32_t scan_one(mbta_table* index, const std::string& start,
                    const std::string* end, bool exclusive,
                    std::string& key, std::string& value) {
    if (!index) return MAKO_SHARD_IO;
    bool found = false;
    KernelTransaction kernel;
    if (!kernel.entered) return MAKO_SHARD_INVALID;
    try {
        Sto::start_transaction();
        index->transQuery(mbta_table::Str(start), end ? mbta_table::Str(*end) : mbta_table::Str(),
            [&](mbta_table::Str row_key, std::string& row_value) {
                if (row_value.size() < mako::EXTRA_BITS_FOR_VALUE) throw Transaction::Abort();
                key.assign(row_key.data(), row_key.length());
                value.assign(row_value.data(), row_value.size() - mako::EXTRA_BITS_FOR_VALUE);
                found = true;
                return false;
            }, nullptr, mbta_table::mythreadinfo, !exclusive);
        if (TThread::transget_without_throw) return MAKO_SHARD_IO;
        if (!Sto::try_commit_no_paxos()) return MAKO_SHARD_IO;
        return found ? MAKO_SHARD_OK : MAKO_SHARD_NOT_FOUND;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @unsafe - bounded exact ordered scan over actual local indexes. Enumerating
// intersected coordinates also materializes empty aliases, so absent aliases
// cannot hide destination leftovers or falsely certify complete coverage.
uint32_t scan_next(void* context, uint64_t table, MakoShardBytes lo,
                     uint32_t has_hi, MakoShardBytes hi, uint32_t has_after,
                     MakoShardBytes after_coordinate, MakoShardBytes after_key,
                     MakoShardRowSink sink, void* sink_context) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host != &host || !sink || has_hi > 1 || has_after > 1)
        return MAKO_SHARD_INVALID;
    try {
        uint32_t coordinate_kind = 0;
        const uint32_t catalog_status = mako_sharding_table_coordinate(table, &coordinate_kind);
        if (catalog_status != MAKO_SHARD_OK) return catalog_status;
        const std::string lower(view(lo)), upper(view(hi));
        if (has_hi && lower >= upper) return MAKO_SHARD_INVALID;
        const std::string cursor_coordinate(view(after_coordinate)), cursor_key(view(after_key));
        std::string key, value;
        if (coordinate_kind == 0) {
            if (has_after && cursor_coordinate != cursor_key) return MAKO_SHARD_INVALID;
            const bool exclusive = has_after && cursor_key >= lower;
            const std::string& start = exclusive ? cursor_key : lower;
            if (has_hi && start >= upper) return MAKO_SHARD_NOT_FOUND;
            auto* index = physical(host, table, false, {});
            const uint32_t status = scan_one(index, start, has_hi ? &upper : nullptr,
                                               exclusive, key, value);
            if (status == MAKO_SHARD_OK) sink(sink_context, bytes(key), bytes(key), bytes(value));
            return status;
        }
        if (coordinate_kind != 1) return MAKO_SHARD_INVALID;
        const auto& floor = has_after && cursor_coordinate > lower ? cursor_coordinate : lower;
        const uint32_t total = mako_sharding_warehouse_count();
        if (total == 0) return MAKO_SHARD_INVALID;
        uint64_t warehouse = std::max(uint32_t{1}, warehouse_prefix(floor));
        for (; warehouse <= total; ++warehouse) {
            const std::string coordinate = warehouse_coordinate(static_cast<uint32_t>(warehouse));
            if (coordinate < lower || (has_after && coordinate < cursor_coordinate)) continue;
            if (has_hi && coordinate >= upper) break;
            auto* index = physical(host, table, true, coordinate);
            const bool exclusive = has_after && coordinate == cursor_coordinate;
            const uint32_t status = scan_one(index, exclusive ? cursor_key : std::string(),
                                               nullptr, exclusive, key, value);
            if (status == MAKO_SHARD_OK) {
                sink(sink_context, bytes(coordinate), bytes(key), bytes(value));
                return MAKO_SHARD_OK;
            }
            if (status != MAKO_SHARD_NOT_FOUND) return status;
        }
        return MAKO_SHARD_NOT_FOUND;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @unsafe - atomic one-key successful engine effect, framed by the supplied
// physical identity. The selected range's Rust authorization borrow stays live.
uint32_t mutate(void* context, uint64_t table, MakoShardBytes coordinate,
                  MakoShardBytes key, const MakoShardBytes* value) {
    auto& host = *static_cast<NativeShardingHost*>(context);
    if (entered_host != &host) return MAKO_SHARD_INVALID;
    try {
        uint32_t coordinate_kind = 0;
        const uint32_t status = mako_sharding_table_coordinate(table, &coordinate_kind);
        if (status != MAKO_SHARD_OK) return status;
        if (coordinate_kind > 1 || (coordinate_kind == 0 && view(coordinate) != view(key)))
            return MAKO_SHARD_INVALID;
        auto* index = physical(host, table, coordinate_kind == 1, view(coordinate));
        if (!index) return MAKO_SHARD_IO;
        const auto key_view = view(key);
        const lcdf::Str row_key(key_view.data(), key_view.size());
        const std::string encoded = value ? mako::Encode(view(*value)) : std::string();
        KernelTransaction kernel;
        if (!kernel.entered) return MAKO_SHARD_INVALID;
        Sto::start_transaction();
        if (value) index->transPut(row_key, StringWrapper(encoded));
        else index->transDelete(row_key);
        if (TThread::transget_without_throw || !Sto::try_commit_no_paxos()) return MAKO_SHARD_IO;
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}
// @unsafe - raw one-key put callback; no ordinary routing/admission recursion.
uint32_t put(void* context, uint64_t table, MakoShardBytes coordinate,
               MakoShardBytes key, MakoShardBytes value) {
    return mutate(context, table, coordinate, key, &value);
}
// @unsafe - raw one-key delete callback; missing keys are successful no-ops.
uint32_t remove(void* context, uint64_t table, MakoShardBytes coordinate, MakoShardBytes key) {
    return mutate(context, table, coordinate, key, nullptr);
}
} // namespace

// @safe - immutable callback table copied by the native Rust runtime.
MakoShardHost NativeShardingHost::callbacks() {
    return {this, thread_enter, thread_leave, open, scan_next, put, remove,
             native_sharding_peer_call};
}
} // namespace mako
