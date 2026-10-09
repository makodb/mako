#include "lib/sharding_leases.h"
#include "lib/table_registry.h"
#include "sto/Transaction.hh"
#include "storage/abstract_db.h"
#include "storage/mbta_wrapper.hh"
#include <limits>
#include <rusty/vec.hpp>

import cluster;
import rusty;

namespace mako {
// @unsafe - the fixed MBTA engine is the only provider; no virtual no-op fallback.
void sharding_mark_proxy(FullOrderedIndex* index) {
    auto* mbta = dynamic_cast<mbta_ordered_index*>(index);
    if (!mbta || oi_mbta_size(mbta->mbta) != 0)
        throw abstract_db::abstract_abort_exception();
    mbta->set_is_remote(true);
}

namespace {
struct CapturedRoute {
    uint64_t table;
    std::string coordinate;
    MakoShardGrant grant;
};
struct LeaseContext {
    MakoShardTxn transaction{};
    ShardingRequest incoming{};
    ShardingRequest outgoing{};
    uint64_t sequence = 0;
    bool open = false;
    bool engine = false;
    bool received = false;
    bool native_kernel = false;
    bool replication_replay = false;
    unsigned pins = 0;
    rusty::Vec<CapturedRoute> routes;
};
thread_local LeaseContext context;

// @unsafe - converts borrowed legacy bytes for the duration of one FFI call.
MakoShardBytes bytes(const char* p, size_t n) {
    return {reinterpret_cast<const uint8_t*>(p), n};
}
// @unsafe - reports native admission failure through the existing engine boundary.
void require(uint32_t status) {
    if (status != MAKO_SHARD_OK) throw abstract_db::abstract_abort_exception();
}
// @unsafe - identity allocation for the fixed-membership, no-restart worker stream.
void begin_local() {
    if (context.open) return;
    if (context.sequence == std::numeric_limits<uint64_t>::max())
        throw abstract_db::abstract_abort_exception();
    if (TThread::get_shard_index() < 0 || TThread::id() < 0
        || uint64_t(TThread::id()) >= (uint64_t{1} << 31))
        throw abstract_db::abstract_abort_exception();
    context.transaction = {
        (uint64_t(uint32_t(TThread::get_shard_index())) << 31)
            | uint64_t(uint32_t(TThread::id())),
        ++context.sequence};
    context.outgoing = {};
    context.outgoing.transaction = context.transaction;
    context.received = false;
    context.routes.clear();
    require(mako_sharding_lease_begin(TThread::get_shard_index(), context.transaction));
    context.open = true;
}
// @unsafe - native finish is called only after physical engine completion.
void finish() {
    if (!context.open || context.engine) return;
    require(mako_sharding_lease_finish(TThread::get_shard_index(), context.transaction));
    context.open = false;
    context.routes.clear();
}
// @unsafe - canonical registry binding; active unbound tables fail closed.
rusty::Arc<NativeTableBinding> binding(int table) {
    auto found = get_table_registry().native_binding(table);
    if (found.is_none()) throw abstract_db::abstract_abort_exception();
    return std::move(found).unwrap();
}
// @unsafe - an immutable alias borrows its warehouse coordinate, raw tables the key.
MakoShardBytes coordinate(const NativeTableBinding& b, const char* key, size_t n) {
    if (b.fixed_coordinate.is_some()) {
        const auto& c = b.fixed_coordinate.as_ref().unwrap();
        return bytes(c.data(), c.size());
    }
    return bytes(key, n);
}
// @unsafe - request caching retains the first full incarnation through reply retries.
MakoShardGrant capture(uint64_t table, MakoShardBytes c) {
    for (const auto& route : context.routes) {
        if (route.table == table && route.coordinate.size() == c.len
            && (c.len == 0 || std::memcmp(route.coordinate.data(), c.data, c.len) == 0))
            return route.grant;
    }
    MakoShardGrant grant{};
    require(mako_sharding_route(table, c, &grant));
    context.routes.push(CapturedRoute{table,
        std::string(reinterpret_cast<const char*>(c.data), c.len), grant});
    return grant;
}
}

// @unsafe - configuration absence is not native admission success. Only disabled
// deployment and the pre-activation bulk-loader bypass the native participant.
bool sharding_leases_enabled() {
    return mako_sharding_enabled()
        && !(TThread::in_loading_phase && !mako_sharding_active(TThread::get_shard_index()));
}
// @unsafe - dedicated transfer worker only; cannot steal an ordinary operation.
bool sharding_enter_native_kernel() {
    if (context.native_kernel || context.replication_replay || context.open
        || context.engine || context.received || context.pins
        || (TThread::txn && TThread::txn->in_progress())) return false;
    context.native_kernel = true;
    return true;
}
// @unsafe - host ends/aborts the raw engine operation before releasing this flag.
void sharding_leave_native_kernel() {
    context.native_kernel = false;
}
// @unsafe - engine begins before any physical access, not on each RPC piece.
void sharding_engine_start() {
    if (context.native_kernel || context.replication_replay) return;
    if (!sharding_leases_enabled()) return;
    if (TThread::mode() == 1 && !context.received) return; // empty helper reset
    if (!context.open) begin_local();
    context.engine = true;
}
// @unsafe - invoked after every engine unlock, cleanup and end callback.
void sharding_engine_complete() {
    if (context.native_kernel || context.replication_replay) return;
    if (!sharding_leases_enabled()) return;
    context.engine = false;
    if (context.pins == 0 && !context.received) finish();
}
// @unsafe - remote terminal messages can still name the just-completed engine.
MakoShardTxn sharding_transaction() { return context.transaction; }
// @unsafe - serializes the origin identity, never a fresh receiver route.
ShardingRequest sharding_outgoing_request() {
    return context.outgoing;
}
// @unsafe - exact local native registration and Serving/epoch check share one lock.
void sharding_require_point(int table, const char* key, size_t length) {
    if (!sharding_leases_enabled()) return;
    if (!context.open) begin_local();
    auto b = binding(table);
    if (b->kind != 0) return; // explicitly catalogued replicated/static table
    auto c = coordinate(*b, key, length);
    auto grant = context.received ? context.incoming.grant : capture(b->table, c);
    require(mako_sharding_lease_acquire(TThread::get_shard_index(), context.transaction,
                                         b->table, c, grant));
}
// @unsafe - protect the entire scan, including absent keys and ownership gaps.
void sharding_require_scan(int table, const std::string& start,
                           const std::string* end, bool reverse) {
    if (!sharding_leases_enabled()) return;
    if (!context.open) begin_local();
    auto b = binding(table);
    if (b->kind != 0) return;
    if (b->fixed_coordinate.is_some()) {
        sharding_require_point(table, start.data(), start.size());
        return;
    }
    std::string lower, upper;
    MakoShardBytes lo = bytes(start.data(), start.size());
    MakoShardBytes hi{};
    bool has_hi = end != nullptr;
    if (end) hi = bytes(end->data(), end->size());
    if (reverse) {
        // Reverse engine bounds are (end,start]. For finite byte strings x\0 is
        // the immediate successor of x; this is an interval bound, not a cursor.
        if (end) { lower = *end; lower.push_back('\0'); }
        upper = start; upper.push_back('\0');
        lo = bytes(lower.data(), lower.size());
        hi = bytes(upper.data(), upper.size());
        has_hi = true;
    }
    auto grant = context.received ? context.incoming.grant : capture(b->table, lo);
    require(mako_sharding_lease_acquire_range(TThread::get_shard_index(),
                                             context.transaction, b->table,
                                             lo, has_hi, hi, grant));
}
// @unsafe - disabled deployments retain legacy routing; native mode fails closed.
int sharding_route_request(int table, const std::string& key, ShardingRequest& request) {
    if (!sharding_leases_enabled()) {
        request = {};
        context.outgoing = request;
        return compute_shard_for_key(table, key);
    }
    try {
        if (!context.open) begin_local();
        auto b = binding(table);
        auto c = coordinate(*b, key.data(), key.size());
        if (c.len > sizeof(request.coordinate)) return -1;
        request = {};
        request.transaction = context.transaction;
        if (b->kind == 0) {
            if (context.received) {
                if (context.incoming.table != b->table
                    || context.incoming.coordinate_length != c.len
                    || (c.len && std::memcmp(context.incoming.coordinate, c.data, c.len)))
                    return -1;
                request.grant = context.incoming.grant;
            } else {
                request.grant = capture(b->table, c);
            }
        } else {
            const int owner = compute_shard_for_key(table, key);
            if (owner < 0) return -1;
            request.grant = {static_cast<uint32_t>(owner), 0};
        }
        request.table = b->table;
        request.fixed_coordinate = b->fixed_coordinate.is_some();
        request.coordinate_length = static_cast<uint16_t>(c.len);
        if (c.len) std::memcpy(request.coordinate, c.data, c.len);
        context.outgoing = request;
        return static_cast<int>(request.grant.owner);
    } catch (const abstract_db::abstract_abort_exception&) {
        return -1;
    }
}

// @unsafe - Rust owns routing and handles; C++ only borrows the engine ABI.
FullOrderedIndex* sharding_point_handle(int table, const char* key, size_t length) {
    if (!sharding_leases_enabled()) return nullptr;
    auto b = binding(table);
    if (b->kind != 0) return nullptr;
    ShardingRequest request{};
    if (sharding_route_request(table, std::string(key, length), request) < 0)
        throw abstract_db::abstract_abort_exception();
    uintptr_t handle = 0;
    if (request.grant.owner == uint32_t(TThread::get_shard_index())) {
        require(mako_sharding_local_table(request.table,
            {request.coordinate, request.coordinate_length}, request.fixed_coordinate,
            request.grant.owner, &handle));
    } else if (b->fixed_coordinate.is_some()) {
        // Warehouse resolution supplies the origin-specific proxy. Its returned
        // route is not substituted for the already captured request grant.
        const auto& c = b->fixed_coordinate.as_ref().unwrap();
        if (c.size() != sizeof(uint32_t))
            throw abstract_db::abstract_abort_exception();
        uint32_t warehouse = 0;
        for (unsigned char byte : c) warehouse = (warehouse << 8) | byte;
        MakoShardGrant ignored{};
        require(mako_sharding_warehouse_resolve(b->table, warehouse,
            TThread::get_shard_index(), &handle, &ignored));
        if (ignored.owner != request.grant.owner || ignored.epoch != request.grant.epoch)
            throw abstract_db::abstract_abort_exception();
    } else {
        require(mako_sharding_raw_proxy(b->table, TThread::get_shard_index(), &handle));
    }
    if (!handle) throw abstract_db::abstract_abort_exception();
    return reinterpret_cast<FullOrderedIndex*>(handle);
}

// @unsafe - serializes the exact snapshot segment, with no new route lookup.
ShardingRequest sharding_request_with_grant(int table, const std::string& key,
                                            MakoShardGrant grant) {
    ShardingRequest request{};
    if (!sharding_leases_enabled()) return request;
    if (!context.open) begin_local();
    auto b = binding(table);
    auto c = coordinate(*b, key.data(), key.size());
    if (c.len > sizeof(request.coordinate))
        throw abstract_db::abstract_abort_exception();
    request.transaction = context.transaction;
    request.grant = grant;
    request.table = b->table;
    request.fixed_coordinate = b->fixed_coordinate.is_some();
    request.coordinate_length = static_cast<uint16_t>(c.len);
    if (c.len) std::memcpy(request.coordinate, c.data, c.len);
    return request;
}
// @unsafe - the scan/RPC adapter cannot replace the logical transaction identity.
void sharding_use_outgoing_request(const ShardingRequest& request) {
    if (sharding_leases_enabled() && context.open
        && (context.transaction.client != request.transaction.client
            || context.transaction.sequence != request.transaction.sequence))
        throw abstract_db::abstract_abort_exception();
    context.outgoing = request;
}
// @unsafe - interval registration is an atomic Rust participant operation.
void sharding_require_interval(int table, const std::string& lo,
                               const std::string* hi, MakoShardGrant grant) {
    if (!sharding_leases_enabled()) return;
    if (!context.open) begin_local();
    auto b = binding(table);
    if (b->kind != 0) return;
    if (b->fixed_coordinate.is_some()) {
        require(mako_sharding_lease_acquire(TThread::get_shard_index(),
            context.transaction, b->table, coordinate(*b, lo.data(), lo.size()), grant));
    } else {
        require(mako_sharding_lease_acquire_range(TThread::get_shard_index(),
            context.transaction, b->table, bytes(lo.data(), lo.size()), hi != nullptr,
            hi ? bytes(hi->data(), hi->size()) : MakoShardBytes{}, grant));
    }
}
// @unsafe - dispatch owns the engine; refuse to replace a still-live transaction.
bool sharding_bind_request(const ShardingRequest& request) {
    if (!sharding_leases_enabled()) return true;
    if (context.open && (context.transaction.client != request.transaction.client
        || context.transaction.sequence != request.transaction.sequence)) return false;
    if (!context.open) {
        if (mako_sharding_lease_begin(TThread::get_shard_index(),
                                       request.transaction) != MAKO_SHARD_OK) return false;
        context.transaction = request.transaction;
        context.open = true;
        context.routes.clear();
    }
    context.received = true;
    context.incoming = request;
    context.engine = TThread::txn && TThread::txn->in_progress();
    return true;
}
// @unsafe - a batch carries one independently selected grant for each row.
void sharding_set_request(const ShardingRequest& request) {
    if (!sharding_leases_enabled()) return;
    if (context.transaction.client != request.transaction.client
        || context.transaction.sequence != request.transaction.sequence)
        throw abstract_db::abstract_abort_exception();
    context.incoming = request;
}
// @unsafe - canonical identity selects this owner's actual registered index.
int sharding_request_table(const ShardingRequest& request, int legacy_table) {
    if (!sharding_leases_enabled()) return legacy_table;
    if (request.coordinate_length > sizeof(request.coordinate))
        throw abstract_db::abstract_abort_exception();
    auto table = get_table_registry().native_table(request.table,
        TThread::get_shard_index(), request.fixed_coordinate,
        std::string_view(reinterpret_cast<const char*>(request.coordinate),
                         request.coordinate_length));
    if (table.is_none()) throw abstract_db::abstract_abort_exception();
    return table.unwrap();
}
// @unsafe - terminal dispatcher calls this after the actual engine is complete.
void sharding_finish_request() {
    if (!sharding_leases_enabled()) return;
    finish();
    if (!context.open) context.received = false;
}
// @unsafe - the replication configuration is an engine boundary input; Rust
// permanently excludes migration activation in a process that admits replay.
ShardingReplicationReplay::ShardingReplicationReplay() {
    if (context.native_kernel || context.replication_replay || context.open
        || context.engine || context.received || context.pins || TThread::mode() != 0
        || (TThread::txn && TThread::txn->in_progress()))
        throw abstract_db::abstract_abort_exception();
    if (mako_sharding_enabled())
        require(mako_sharding_replication_replay(
            BenchmarkConfig::getInstance().getIsReplicated() != 0 ? 1 : 0));
    context.replication_replay = true;
}
// @unsafe - an exceptional replay must finish physical cleanup in its own lane.
ShardingReplicationReplay::~ShardingReplicationReplay() {
    if (TThread::txn) TThread::txn->silent_abort();
    context.replication_replay = false;
}
// @unsafe - pin the logical operation, not an individual OCC attempt.
ShardingOperation::ShardingOperation() : outer_(false) {
    if (!sharding_leases_enabled()) return;
    outer_ = true;
    ++context.pins;
}
// @unsafe - fail closed if cleanup unwound: retain scopes while engine is live.
ShardingOperation::~ShardingOperation() {
    if (!outer_) return;
    --context.pins;
    if (context.pins == 0 && !context.received) finish();
}
}
