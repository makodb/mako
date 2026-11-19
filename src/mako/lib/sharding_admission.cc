#include "lib/sharding_admission.h"
#include "lib/table_registry.h"
#include "sto/Transaction.hh"
#include "storage/abstract_db.h"
#include "storage/mbta_wrapper.hh"
#include <limits>
#include <string_view>
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

// @unsafe - sole constructor for dispatch values crossing legacy engine/RPC code.
struct ShardingPointAccess {
    // @unsafe - slots, not references, survive growth of the protected capture cache.
    static ShardingPoint captured(size_t slot, bool fixed, FullOrderedIndex* handle,
                                  int owner) {
        ShardingPoint point;
        point.payload_.slot = slot;
        point.handle_ = handle;
        point.owner_ = owner;
        point.kind_ = fixed ? ShardingPoint::Kind::Fixed : ShardingPoint::Kind::Raw;
        return point;
    }
    // @unsafe - receiver dispatch borrows the envelope owned by its logical guard.
    static ShardingPoint received(FullOrderedIndex* handle, int owner, bool governed) {
        ShardingPoint point;
        point.handle_ = handle;
        point.owner_ = owner;
        point.kind_ = governed ? ShardingPoint::Kind::Received
                               : ShardingPoint::Kind::ReceivedUnmanaged;
        return point;
    }
    // @unsafe - immutable binding ownership is retained by the context; key bytes
    // belong to the caller until this synchronous operation has been dispatched.
    static ShardingPoint unmanaged(const NativeTableBinding* binding,
                                   MakoShardBytes coordinate, int owner) {
        ShardingPoint point;
        point.payload_.unmanaged = {binding, coordinate};
        point.owner_ = owner;
        point.kind_ = ShardingPoint::Kind::Unmanaged;
        return point;
    }
    // @safe - disabled routing has no native envelope or capture.
    static ShardingPoint disabled(int owner) {
        ShardingPoint point;
        point.owner_ = owner;
        return point;
    }
};

// @unsafe - byte-order conversion for the fixed warehouse ABI, with no admission.
ShardingPreparedRoute::ShardingPreparedRoute(uint64_t table, uint32_t warehouse)
    : table_(table), warehouse_(warehouse),
      coordinate_{uint8_t(warehouse >> 24), uint8_t(warehouse >> 16),
                  uint8_t(warehouse >> 8), uint8_t(warehouse)} {}

namespace {
struct CapturedRoute {
    uint64_t table;
    std::string coordinate;
    MakoShardRoute route;
    // Non-null means selection and local ownership admission both completed.
    FullOrderedIndex* handle = nullptr;
    // Borrows an immutable Arc allocation retained by bindings, not a map element.
    const NativeTableBinding* binding = nullptr;
    // Transaction-local scope membership, independent of wire materialization.
    uint64_t used_revision = 0;
};
struct AdmissionContext {
    // Borrows the active Rust TLS read view; owns one nested STO epoch guard.
    const MakoShardRead* read = nullptr;
    MakoShardTxn transaction{};
    ShardingRequest incoming{};
    uint64_t sequence = 0;
    uint64_t revision = 0;
    uint64_t view_id = 0;
    size_t prepared_slot = SIZE_MAX;
    int prepared_table = -1;
    bool open = false;
    bool engine = false;
    bool received = false;
    bool native_kernel = false;
    bool replication_replay = false;
    unsigned pins = 0;
    rusty::Vec<CapturedRoute> routes;
    // Only bounded fixed coordinates persist; arbitrary raw keys end with the txn.
    rusty::Vec<CapturedRoute> fixed_routes;
    rusty::HashMap<int, rusty::Arc<NativeTableBinding>> bindings;
    // Mapping only: the protected view implies the participant. Raw row keys
    // never enter these caches; fixed keys borrow the retained binding's string.
    // Native ABI handles are pointer-sized integers; the engine owns the objects.
    rusty::HashMap<uint64_t, uintptr_t> raw_handles;
    rusty::HashMap<uint64_t, rusty::HashMap<std::string_view, uintptr_t>> fixed_handles;
    FullOrderedIndex* received_handle = nullptr;
    const NativeTableBinding* received_binding = nullptr;
    bool received_admitted = false;
};
thread_local AdmissionContext context;

// @unsafe - converts borrowed legacy bytes for the duration of one FFI call.
MakoShardBytes bytes(const char* p, size_t n) {
    return {reinterpret_cast<const uint8_t*>(p), n};
}
// @unsafe - reports native admission failure through the existing engine boundary.
void require(uint32_t status) {
    if (status != MAKO_SHARD_OK) throw abstract_db::abstract_abort_exception();
}
// @unsafe - announce the STO epoch before reading any authorization metadata.
// A failed read begin releases only this new logical guard, never an engine pin.
uint32_t begin_context() {
    if (context.revision == std::numeric_limits<uint64_t>::max()) return MAKO_SHARD_RETRY;
    const auto epoch = Transaction::epoch_enter();
    if (epoch == 0) return MAKO_SHARD_RETRY;
    const MakoShardRead* read = nullptr;
    const auto status = mako_sharding_read_begin(TThread::get_shard_index(), epoch, &read);
    if (status != MAKO_SHARD_OK) {
        Transaction::epoch_leave();
        return status;
    }
    uint64_t view_id = 0;
    const auto view_status = mako_sharding_read_view_id(read, &view_id);
    if (view_status != MAKO_SHARD_OK || view_id == 0) {
        mako_sharding_read_end(read);
        Transaction::epoch_leave();
        return view_status == MAKO_SHARD_OK ? MAKO_SHARD_RETRY : view_status;
    }
    if (context.view_id != view_id) {
        // @unsafe - drop all borrowed coordinates/bindings before their Arc owners.
        context.raw_handles.clear();
        context.fixed_handles.clear();
        context.routes.clear();
        context.fixed_routes.clear();
        context.received_handle = nullptr;
        context.received_binding = nullptr;
        context.received_admitted = false;
        context.bindings.clear();
        context.view_id = view_id;
    }
    context.read = read;
    ++context.revision;
    context.prepared_slot = SIZE_MAX;
    context.prepared_table = -1;
    return MAKO_SHARD_OK;
}
// @unsafe - consume only the logical guard; a separate engine guard may remain.
void release_read() {
    mako_sharding_read_end(context.read);
    context.read = nullptr;
    Transaction::epoch_leave();
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
    context.received = false;
    context.routes.clear();
    context.received_handle = nullptr;
    context.received_binding = nullptr;
    context.received_admitted = false;
    require(begin_context());
    context.open = true;
    context.engine = TThread::txn && TThread::txn->has_epoch();
}
// @unsafe - all physical effects, remote terminal work and logical retries ended.
void finish() {
    if (!context.open || context.engine || context.pins || context.received) return;
    release_read();
    context.open = false;
    context.routes.clear();
    context.prepared_slot = SIZE_MAX;
    context.prepared_table = -1;
    context.received_handle = nullptr;
    context.received_binding = nullptr;
    context.received_admitted = false;
}
// @unsafe - borrow the immutable Arc allocation, never a movable map entry.
const NativeTableBinding* binding(int table) {
    auto cached = context.bindings.get(table);
    if (cached.is_some()) return &*cached.unwrap();
    auto found = get_table_registry().native_binding(table);
    if (found.is_none()) throw abstract_db::abstract_abort_exception();
    auto owner = std::move(found).unwrap();
    const auto* result = &*owner;
    context.bindings.insert(table, std::move(owner));
    return result;
}
// @unsafe - selection guarantees the same canonical binding. Clone the known
// source owner only on a cold alias; no map-element reference crosses insert().
const NativeTableBinding* retain_alias(int table, int source_table) {
    auto cached = context.bindings.get(table);
    if (cached.is_some()) return &*cached.unwrap();
    auto source = context.bindings.get(source_table);
    if (source.is_none()) throw abstract_db::abstract_abort_exception();
    auto owner = source.unwrap().clone();
    const auto* result = &*owner;
    context.bindings.insert(table, std::move(owner));
    return result;
}
// @unsafe - an immutable alias borrows its warehouse coordinate, raw tables the key.
MakoShardBytes coordinate(const NativeTableBinding& b, const char* key, size_t n) {
    if (b.fixed_coordinate.is_some()) {
        const auto& c = b.fixed_coordinate.as_ref().unwrap();
        return bytes(c.data(), c.size());
    }
    return bytes(key, n);
}
// @unsafe - borrows a TLS container, never an element across an ABI call.
rusty::Vec<CapturedRoute>& captures(bool fixed) {
    return fixed ? context.fixed_routes : context.routes;
}
// @unsafe - slot indices survive Vec growth; references to its elements do not.
size_t find_capture(uint64_t table, MakoShardBytes c, bool fixed) {
    const auto& routes = captures(fixed);
    for (size_t i = 0; i < routes.len(); ++i) {
        const auto& route = routes[i];
        if (route.table == table && route.coordinate.size() == c.len
            && (c.len == 0 || std::memcmp(route.coordinate.data(), c.data, c.len) == 0))
            return i;
    }
    return routes.len();
}
// @unsafe - fixed-coordinate routing survives only in the identical protected view.
size_t capture_slot(uint64_t table, MakoShardBytes c, bool fixed) {
    const auto slot = find_capture(table, c, fixed);
    if (slot != captures(fixed).len()) return slot;
    MakoShardRoute route{};
    require(mako_sharding_read_route(context.read, table, c, &route));
    captures(fixed).push(CapturedRoute{table,
        std::string(reinterpret_cast<const char*>(c.data), c.len), route});
    return slot;
}
// @unsafe - scan callers need only the retained route, not point selection.
MakoShardRoute capture(uint64_t table, MakoShardBytes c, bool fixed) {
    const auto slot = capture_slot(table, c, fixed);
    return captures(fixed)[slot].route;
}
// @unsafe - binds a transported capture to the actual native point operation.
MakoShardRoute received_route(uint64_t table, MakoShardBytes c) {
    if (c.len > sizeof(context.incoming.coordinate))
        throw abstract_db::abstract_abort_exception();
    if (context.incoming.table != table
        || context.incoming.coordinate_length != c.len
        || (c.len && std::memcmp(context.incoming.coordinate, c.data, c.len)))
        throw abstract_db::abstract_abort_exception();
    return {context.incoming.grant, context.incoming.snapshot_version};
}
// @safe - full request identity, including snapshot provenance and bounded bytes.
bool same_request(const ShardingRequest& a, const ShardingRequest& b) {
    return a.transaction.client == b.transaction.client
        && a.transaction.sequence == b.transaction.sequence
        && a.table == b.table && a.fixed_coordinate == b.fixed_coordinate
        && a.grant.owner == b.grant.owner
        && a.grant.ownership_version == b.grant.ownership_version
        && a.snapshot_version == b.snapshot_version
        && a.coordinate_length == b.coordinate_length
        && a.coordinate_length <= sizeof(a.coordinate)
        && (a.coordinate_length == 0
            || std::memcmp(a.coordinate, b.coordinate, a.coordinate_length) == 0);
}
// @unsafe - serialize only at a data-RPC boundary, using the protected capture.
void write_point_request(ShardingRequest& request, uint64_t table, MakoShardBytes c,
                         bool fixed, MakoShardRoute route) {
    if (c.len > sizeof(request.coordinate))
        throw abstract_db::abstract_abort_exception();
    request = {};
    request.transaction = context.transaction;
    request.table = table;
    request.fixed_coordinate = fixed;
    request.grant = route.grant;
    request.snapshot_version = route.snapshot_version;
    request.coordinate_length = static_cast<uint16_t>(c.len);
    if (c.len) std::memcpy(request.coordinate, c.data, c.len);
}
// @unsafe - selection consumes captured routing fields, not a temporary RPC envelope.
FullOrderedIndex* select_handle(uint64_t table, MakoShardBytes c, bool fixed,
                               MakoShardGrant grant) {
    uintptr_t handle = 0;
    if (fixed) {
        if (c.len != sizeof(uint32_t))
            throw abstract_db::abstract_abort_exception();
        uint32_t warehouse = 0;
        for (size_t i = 0; i < c.len; ++i)
            warehouse = (warehouse << 8) | c.data[i];
        require(mako_sharding_warehouse_select(table, warehouse,
            TThread::get_shard_index(), grant, &handle));
    } else if (grant.owner == uint32_t(TThread::get_shard_index())) {
        require(mako_sharding_local_table(table, c, false, grant.owner, &handle));
    } else {
        require(mako_sharding_raw_proxy(table, TThread::get_shard_index(), &handle));
    }
    if (!handle) throw abstract_db::abstract_abort_exception();
    return reinterpret_cast<FullOrderedIndex*>(handle);
}
// @unsafe - publish a reusable handle only after local ownership admission.
// The protected view retains that decision; no transaction-specific request is
// constructed and no authorization predicate is repeated on a warm selection.
void select_capture_slot(size_t slot, bool fixed, int source_table = -1) {
    const auto& entry = captures(fixed)[slot];
    if (entry.handle) return;
    const auto table = entry.table;
    const auto route = entry.route;
    const auto c = bytes(entry.coordinate.data(), entry.coordinate.size());
    auto* handle = select_handle(table, c, fixed, route.grant);
    if (route.grant.owner == uint32_t(TThread::get_shard_index())) {
        const auto& current = captures(fixed)[slot];
        require(mako_sharding_read_point(context.read, table,
            bytes(current.coordinate.data(), current.coordinate.size()), route));
    }
    const auto* b = source_table >= 0
        ? retain_alias(handle->get_table_id(), source_table)
        : binding(handle->get_table_id());
    // No capture reference crosses publication; callbacks may grow a container.
    captures(fixed)[slot].binding = b;
    captures(fixed)[slot].handle = handle;
}
// @unsafe - record current transaction membership without creating wire bytes.
ShardingPoint selected_slot(size_t slot, bool fixed) {
    auto& entry = captures(fixed)[slot];
    entry.used_revision = context.revision;
    return ShardingPointAccess::captured(slot, fixed, entry.handle,
                                        static_cast<int>(entry.route.grant.owner));
}
// @unsafe - captured slots survive callbacks and container growth under the guard.
ShardingPoint selected_capture(int source_table, uint64_t table, MakoShardBytes c,
                               bool fixed) {
    const auto slot = capture_slot(table, c, fixed);
    select_capture_slot(slot, fixed, source_table);
    return selected_slot(slot, fixed);
}
} // namespace

// @unsafe - invoked only while the resolved operation's guard and key borrow live.
void ShardingPoint::write_request(ShardingRequest& destination) const {
    switch (kind_) {
    case Kind::Fixed:
    case Kind::Raw: {
        const auto& entry = captures(kind_ == Kind::Fixed)[payload_.slot];
        write_point_request(destination, entry.table,
            bytes(entry.coordinate.data(), entry.coordinate.size()),
            kind_ == Kind::Fixed, entry.route);
        break;
    }
    case Kind::Received:
    case Kind::ReceivedUnmanaged:
        destination = context.incoming;
        break;
    case Kind::Unmanaged:
        write_point_request(destination, payload_.unmanaged.binding->table,
            payload_.unmanaged.coordinate,
            payload_.unmanaged.binding->fixed_coordinate.is_some(),
            {{static_cast<uint32_t>(owner_), 0}, MAKO_SHARD_NO_SNAPSHOT});
        break;
    case Kind::Disabled:
        destination = {};
        destination.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
        break;
    }
}

// @unsafe - configuration absence is not native admission success. Only disabled
// deployment and the pre-activation bulk-loader bypass native authorization.
bool sharding_admission_enabled() {
    return mako_sharding_enabled()
        && !(TThread::in_loading_phase && !mako_sharding_active(TThread::get_shard_index()));
}
// @safe - the logical receiver owns this flag through terminal engine cleanup.
bool sharding_has_request() { return context.received; }
// @unsafe - the guard owns immutable binding allocations; direct borrows avoid
// another map lookup for a prepared route or the current receiver's table.
const NativeTableBinding* sharding_table_binding(int table) {
    if (!context.open) begin_local();
    if (context.received_handle && context.received_handle->get_table_id() == table)
        return context.received_binding;
    if (table == context.prepared_table
        && context.prepared_slot < context.fixed_routes.len())
        return context.fixed_routes[context.prepared_slot].binding;
    return binding(table);
}
// @unsafe - native registry/engine ownership bridge, not an admission boundary.
// A positive result borrows an engine handle until shutdown; the current guard
// protects its view. No grant, snapshot or transaction is authorized by a hit.
FullOrderedIndex* sharding_physical_handle(const ShardingRequest& request) {
    if (!context.open) begin_local();
    if (request.coordinate_length > sizeof(request.coordinate))
        throw abstract_db::abstract_abort_exception();
    const auto c = request.fixed_coordinate
        ? std::string_view(reinterpret_cast<const char*>(request.coordinate),
                           request.coordinate_length)
        : std::string_view();
    if (request.fixed_coordinate) {
        auto group = context.fixed_handles.get(request.table);
        if (group.is_some()) {
            auto cached = group.unwrap().get(c);
            if (cached.is_some()) return reinterpret_cast<FullOrderedIndex*>(cached.unwrap());
        }
    } else {
        auto cached = context.raw_handles.get(request.table);
        if (cached.is_some()) return reinterpret_cast<FullOrderedIndex*>(cached.unwrap());
    }
    auto found = get_table_registry().native_handle(request.table,
        TThread::get_shard_index(), request.fixed_coordinate, c);
    if (found.is_none()) throw abstract_db::abstract_abort_exception();
    auto* handle = found.unwrap();
    const auto opaque = reinterpret_cast<uintptr_t>(handle);
    const auto* b = binding(handle->get_table_id());
    if (request.fixed_coordinate) {
        // @unsafe - never retain incoming bytes or a movable map/Vec element.
        // Registry publication guarantees this physical binding's exact address.
        const auto& owned = b->fixed_coordinate.as_ref().unwrap();
        const std::string_view retained(owned.data(), owned.size());
        auto group = context.fixed_handles.get(request.table);
        if (group.is_some()) {
            group.unwrap().insert(retained, opaque);
        } else {
            rusty::HashMap<std::string_view, uintptr_t> handles;
            handles.insert(retained, opaque);
            context.fixed_handles.insert(request.table, std::move(handles));
        }
    } else {
        context.raw_handles.insert(request.table, opaque);
    }
    return handle;
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
    if (!sharding_admission_enabled()) return;
    if (!context.open) begin_local();
    context.engine = true;
}
// @unsafe - invoked after every engine unlock, cleanup and end callback.
void sharding_engine_complete() {
    if (context.native_kernel || context.replication_replay) return;
    if (!context.open) return;
    context.engine = false;
    if (context.pins == 0 && !context.received) finish();
}
// @unsafe - remote terminal messages can still name the just-completed engine.
MakoShardTxn sharding_transaction() { return context.transaction; }
// @unsafe - terminal RPCs retain the logical identity even after engine completion.
void sharding_write_control_request(ShardingRequest& destination) {
    destination = {};
    destination.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
    if (sharding_admission_enabled()) destination.transaction = context.transaction;
}
// @unsafe - receiver/gateway callers retain their independent admission boundary.
void sharding_require_point(int table, const char* key, size_t length) {
    const auto point = sharding_resolve_point(table, key, length);
    // This boundary cannot forward: the receiver must execute on the owner.
    if (point.governed() && point.owner() != TThread::get_shard_index())
        throw abstract_db::abstract_abort_exception();
}
// @unsafe - protect the entire scan, including absent keys and ownership gaps.
void sharding_require_scan(int table, const std::string& start,
                           const std::string* end, bool reverse) {
    if (!sharding_admission_enabled()) return;
    if (!context.open) begin_local();
    const auto* b = sharding_table_binding(table);
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
    // A lower-endpoint point capture is not a homogeneous interval certificate.
    // Full scans use sharding_require_interval with verified snapshot segments.
    const auto grant = context.received ? context.incoming.grant : capture(b->table, lo, false).grant;
    require(mako_sharding_read_range(context.read, b->table, lo, has_hi, hi,
        {grant, MAKO_SHARD_NO_SNAPSHOT}));
}
// @unsafe - resolve from the active context and admit local effects in one path;
// callers cannot supply an old point for revalidation or authorization.
ShardingPoint sharding_resolve_point(int table, const char* key, size_t length) {
    if (!sharding_admission_enabled())
        return ShardingPointAccess::disabled(compute_shard_for_key(table, {}));
    if (!context.open) begin_local();
    if (!context.received && table == context.prepared_table
        && context.prepared_slot < context.fixed_routes.len())
        return selected_slot(context.prepared_slot, true);
    const auto* b = sharding_table_binding(table);
    const auto c = coordinate(*b, key, length);
    if (context.received) {
        // Exact current envelope, including an untrusted NO_SNAPSHOT marker.
        if (b->kind == 0) received_route(b->table, c);
        if (context.incoming.fixed_coordinate != b->fixed_coordinate.is_some()
            || context.incoming.grant.owner != uint32_t(TThread::get_shard_index()))
            throw abstract_db::abstract_abort_exception();
        if (!context.received_handle || context.received_handle->get_table_id() != table)
            if (sharding_request_table(context.incoming, table) != table)
                throw abstract_db::abstract_abort_exception();
        if (b->kind == 0 && !context.received_admitted) {
            require(mako_sharding_read_point(context.read, context.incoming.table, c,
                {context.incoming.grant, context.incoming.snapshot_version}));
            context.received_admitted = true;
        }
        return ShardingPointAccess::received(context.received_handle,
            static_cast<int>(context.incoming.grant.owner), b->kind == 0);
    }
    if (b->kind == 0) {
        return selected_capture(table, b->table, c, b->fixed_coordinate.is_some());
    }
    const auto owner = compute_shard_for_key(table, {});
    if (owner < 0) throw abstract_db::abstract_abort_exception();
    return ShardingPointAccess::unmanaged(b, c, owner);
}
// @unsafe - reuse fixed routing/selection only in the same full admission view.
FullOrderedIndex* sharding_warehouse_handle(ShardingPreparedRoute& route,
                                           uint32_t participant) {
    if (!context.open) {
        uintptr_t handle = 0;
        MakoShardGrant grant{};
        require(mako_sharding_warehouse_resolve(route.table_, route.warehouse_,
                                               participant, &handle, &grant));
        if (!handle) throw abstract_db::abstract_abort_exception();
        return reinterpret_cast<FullOrderedIndex*>(handle);
    }
    if (participant != uint32_t(TThread::get_shard_index()) || context.received)
        throw abstract_db::abstract_abort_exception();
    if (route.view_id_ != context.view_id
        || route.capture_slot_ >= context.fixed_routes.len()) {
        route.capture_slot_ = capture_slot(route.table_,
            {route.coordinate_, sizeof(route.coordinate_)}, true);
        route.view_id_ = context.view_id;
    }
    select_capture_slot(route.capture_slot_, true);
    auto* handle = context.fixed_routes[route.capture_slot_].handle;
    context.prepared_slot = route.capture_slot_;
    context.prepared_table = handle->get_table_id();
    return handle;
}
// @unsafe - legitimate scan routing still captures once, but is not point admission.
int sharding_scan_request(int table, const std::string& key, ShardingRequest& request) {
    try {
        if (!sharding_admission_enabled()) {
            request = {};
            request.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
            return compute_shard_for_key(table, {});
        }
        if (!context.open) begin_local();
        const auto* b = sharding_table_binding(table);
        const auto c = coordinate(*b, key.data(), key.size());
        if (context.received) {
            if (b->kind == 0) received_route(b->table, c);
            request = context.incoming;
        } else if (b->kind == 0) {
            const bool fixed = b->fixed_coordinate.is_some();
            write_point_request(request, b->table, c, fixed, capture(b->table, c, fixed));
        } else {
            const auto owner = compute_shard_for_key(table, {});
            if (owner < 0) return -1;
            write_point_request(request, b->table, c, b->fixed_coordinate.is_some(),
                {{static_cast<uint32_t>(owner), 0}, MAKO_SHARD_NO_SNAPSHOT});
        }
        return static_cast<int>(request.grant.owner);
    } catch (const abstract_db::abstract_abort_exception&) {
        return -1;
    }
}
// @unsafe - a staged governed write must already own its exact operation capture.
int sharding_batch_request(int table, const std::string& key, ShardingRequest& request) {
    try {
        if (!sharding_admission_enabled()) return sharding_scan_request(table, key, request);
        if (!context.open || context.received) throw abstract_db::abstract_abort_exception();
        const auto* b = sharding_table_binding(table);
        if (b->kind != 0) return sharding_scan_request(table, key, request);
        const bool fixed = b->fixed_coordinate.is_some();
        const auto slot = find_capture(b->table, coordinate(*b, key.data(), key.size()), fixed);
        const auto& routes = captures(fixed);
        if (slot == routes.len() || !routes[slot].handle)
            throw abstract_db::abstract_abort_exception();
        const auto& entry = routes[slot];
        // A persistent routing cache is not this transaction's captured scope.
        if (entry.used_revision != context.revision || entry.handle->get_table_id() != table)
            throw abstract_db::abstract_abort_exception();
        write_point_request(request, entry.table,
            bytes(entry.coordinate.data(), entry.coordinate.size()), fixed, entry.route);
        return static_cast<int>(entry.route.grant.owner);
    } catch (const abstract_db::abstract_abort_exception&) {
        return -1;
    }
}

// @unsafe - serializes the exact snapshot segment, with no new route lookup.
ShardingRequest sharding_request_with_route(int table, const std::string& key,
                                            MakoShardRoute route) {
    ShardingRequest request{};
    request.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
    if (!sharding_admission_enabled()) return request;
    if (!context.open) begin_local();
    const auto* b = sharding_table_binding(table);
    auto c = coordinate(*b, key.data(), key.size());
    write_point_request(request, b->table, c, b->fixed_coordinate.is_some(), route);
    return request;
}
// @unsafe - certify the complete interval in this logical operation's read view.
void sharding_require_interval(int table, MakoShardBytes lo, bool has_hi,
                               MakoShardBytes hi, MakoShardRoute route) {
    if (!sharding_admission_enabled()) return;
    if (!context.open) begin_local();
    const auto* b = sharding_table_binding(table);
    if (b->kind != 0) return;
    if (b->fixed_coordinate.is_some()) {
        require(mako_sharding_read_point(context.read, b->table,
            coordinate(*b, reinterpret_cast<const char*>(lo.data), lo.len), route));
    } else {
        require(mako_sharding_read_range(context.read,
            b->table, lo, has_hi, hi, route));
    }
}
// @unsafe - synchronous callbacks borrow the retained snapshot under its STO pin.
uint32_t sharding_scan_segments(uint64_t table, MakoShardBytes lo, uint32_t has_hi,
                                MakoShardBytes hi, uint32_t reverse,
                                MakoScanSegmentCallback callback, void* user) {
    if (!context.open) begin_local();
    return mako_sharding_read_scan_segments(context.read, table, lo, has_hi, hi,
                                            reverse, callback, user);
}
// @unsafe - dispatch owns the engine; refuse to replace a still-live transaction.
bool sharding_bind_request(const ShardingRequest& request) {
    if (!sharding_admission_enabled()) return true;
    if (request.coordinate_length > sizeof(request.coordinate)
        || context.native_kernel || context.replication_replay) return false;
    if (context.open && (!context.received
        || context.transaction.client != request.transaction.client
        || context.transaction.sequence != request.transaction.sequence)) return false;
    const bool opened = !context.open;
    if (opened) {
        if (begin_context() != MAKO_SHARD_OK) return false;
        context.transaction = request.transaction;
        context.open = true;
        context.routes.clear();
        context.received_handle = nullptr;
        context.received_binding = nullptr;
        context.received_admitted = false;
    }
    context.received = true;
    if (TThread::txn && TThread::txn->in_progress()) {
        try {
            // Idle helper resets deliberately do not own an engine epoch yet.
            TThread::txn->ensure_epoch();
        } catch (...) {
            if (opened) {
                release_read();
                context.open = false;
                context.received = false;
            }
            return false;
        }
        context.engine = true;
    }
    if (!same_request(request, context.incoming)) {
        if (!opened) {
            if (context.revision == std::numeric_limits<uint64_t>::max()) return false;
            ++context.revision;
        }
        context.received_handle = nullptr;
        context.received_binding = nullptr;
        context.received_admitted = false;
    }
    context.incoming = request;
    return true;
}
// @unsafe - a batch carries one independently selected grant for each row.
void sharding_set_request(const ShardingRequest& request) {
    if (!sharding_admission_enabled()) return;
    if (!context.open || !context.received
        || request.coordinate_length > sizeof(request.coordinate)
        || context.transaction.client != request.transaction.client
        || context.transaction.sequence != request.transaction.sequence)
        throw abstract_db::abstract_abort_exception();
    if (!same_request(request, context.incoming)) {
        if (context.revision == std::numeric_limits<uint64_t>::max())
            throw abstract_db::abstract_abort_exception();
        ++context.revision;
        context.received_handle = nullptr;
        context.received_binding = nullptr;
        context.received_admitted = false;
    }
    context.incoming = request;
}
// @unsafe - canonical identity selects this owner's actual registered index.
int sharding_request_table(const ShardingRequest& request, int legacy_table) {
    if (!sharding_admission_enabled()) return legacy_table;
    if (!context.open || !context.received || !same_request(request, context.incoming))
        throw abstract_db::abstract_abort_exception();
    if (!context.received_handle) {
        auto* handle = sharding_physical_handle(request);
        const auto* b = binding(handle->get_table_id());
        context.received_binding = b;
        context.received_handle = handle;
    }
    return context.received_handle->get_table_id();
}
// @unsafe - terminal dispatcher calls this after the actual engine is complete.
void sharding_finish_request() {
    if (context.engine) return;
    context.received = false;
    finish();
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
    if (context.native_kernel || context.replication_replay
        || !sharding_admission_enabled()) return;
    if (context.pins == std::numeric_limits<unsigned>::max())
        throw abstract_db::abstract_abort_exception();
    if (!context.open) begin_local();
    outer_ = true;
    ++context.pins;
}
// @unsafe - fail closed if cleanup unwound: retain the epoch while engine is live.
ShardingOperation::~ShardingOperation() {
    if (!outer_) return;
    --context.pins;
    if (context.pins == 0 && !context.received) finish();
}
}
