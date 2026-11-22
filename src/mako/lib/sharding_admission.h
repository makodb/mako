#ifndef MAKO_SHARDING_ADMISSION_H
#define MAKO_SHARDING_ADMISSION_H

#include "cluster/native_sharding.h"
#include "cluster/full_scan.h"
#include <cstddef>
#include <cstdint>
#include <string>

class FullOrderedIndex;

namespace mako {
struct NativeTableBinding;
// @unsafe - only a newly allocated, unused MBTA handle may become a proxy.
void sharding_mark_proxy(FullOrderedIndex* index);
// Transport-owned POD; preserve the sender's paired snapshot and ownership grant.
struct ShardingRequest {
    MakoShardTxn transaction;
    MakoShardGrant grant;
    uint64_t snapshot_version;
    uint64_t table;
    uint16_t coordinate_length;
    bool fixed_coordinate;
    uint8_t coordinate[64];
};

// Immediate-dispatch result, not an admission token or an RPC envelope. Captured
// slots survive container growth but belong to the current logical guard. The
// unmanaged case borrows the caller's key until dispatch; received points borrow
// the current incoming envelope. No point may outlive those scopes.
class ShardingPoint {
public:
    // @unsafe - materializes captured provenance directly into an RPC buffer.
    void write_request(ShardingRequest& destination) const;
    // @unsafe - borrows an engine-owned handle until shutdown.
    FullOrderedIndex* handle() const { return handle_; }
    // @safe - immutable captured owner.
    int owner() const { return owner_; }
    // @safe - immutable catalog classification.
    bool governed() const {
        return kind_ == Kind::Fixed || kind_ == Kind::Raw || kind_ == Kind::Received;
    }
private:
    friend struct ShardingPointAccess;
    enum class Kind : uint8_t { Disabled, Fixed, Raw, Received, ReceivedUnmanaged, Unmanaged };
    // @unsafe - tagged native bridge payload; never owns a movable cache element.
    union Payload {
        size_t slot;
        struct {
            const NativeTableBinding* binding;
            MakoShardBytes coordinate;
        } unmanaged;
        // @safe - default payload belongs to the disabled kind.
        Payload() : slot(0) {}
    } payload_;
    // @safe - only the resolver constructs the private dispatch result.
    ShardingPoint() = default;
    FullOrderedIndex* handle_ = nullptr;
    int owner_ = -1;
    Kind kind_ = Kind::Disabled;
};

// Producer-owned routing input. Its private fixed-cache locator survives
// transactions only while the protected native admission-view identity matches.
// It retains no engine pin or transaction authority between operations.
class ShardingPreparedRoute {
public:
    // @unsafe - precomputes ABI bytes only; no engine pointer, metadata read or pin.
    ShardingPreparedRoute(uint64_t table, uint32_t warehouse);
private:
    friend FullOrderedIndex* sharding_warehouse_handle(ShardingPreparedRoute&, uint32_t);
    uint64_t table_;
    uint32_t warehouse_;
    uint8_t coordinate_[4];
    uint64_t view_id_ = 0;
    size_t capture_slot_ = SIZE_MAX;
};

// @unsafe - resolve and authorize the current local access, or select its remote
// request. Callers immediately dispatch under their existing operation/engine guard.
ShardingPoint sharding_resolve_point(int table, const char* key, size_t length);
// @unsafe - active logical work shares its capture with the TPC-C accessor.
// Outside an open context this only selects a handle and never pins an epoch.
FullOrderedIndex* sharding_warehouse_handle(ShardingPreparedRoute& route,
                                           uint32_t participant);

// @unsafe - native configuration/legacy thread-local engine boundary.
bool sharding_admission_enabled();
// @safe - an idle mode-1 helper has no received logical request.
bool sharding_has_request();
// @unsafe - mapping only, never authorization. The immutable Arc allocation is
// retained by the current protected view; callers borrow it under a logical guard.
const NativeTableBinding* sharding_table_binding(int table);
// @unsafe - mapping only, never authorization. Resolves this participant's actual
// physical index under the logical guard; engine handles live until shutdown.
FullOrderedIndex* sharding_physical_handle(const ShardingRequest& request);
// @unsafe - engine lifetime callbacks; completion follows all unlocks and cleanup.
void sharding_engine_start();
void sharding_engine_complete();
// @unsafe - only the guarded native storage host may bracket raw MBTA calls.
// This suppresses engine lifecycle reentry, NOT ordinary point/range admission.
bool sharding_enter_native_kernel();
void sharding_leave_native_kernel();
// @unsafe - preserves the originating identity while constructing RPC buffers.
MakoShardTxn sharding_transaction();
// @unsafe - resolves the registered canonical address, then calls native admission.
void sharding_require_point(int table, const char* key, size_t length);
// @unsafe - control messages carry transaction identity, never the last row's scope.
void sharding_write_control_request(ShardingRequest& destination);
void sharding_require_scan(int table, const std::string& start,
                           const std::string* end, bool reverse = false);
// @unsafe - legacy/static scan dispatch; raw range admission still uses NO_SNAPSHOT.
int sharding_scan_request(int table, const std::string& key, ShardingRequest& request);
// @unsafe - commit rows must reuse an existing governed capture, never route anew.
int sharding_batch_request(int table, const std::string& key, ShardingRequest& request);
// @unsafe - scan snapshots supply the original complete segment route.
ShardingRequest sharding_request_with_route(int table, const std::string& key,
                                            MakoShardRoute route);
// @unsafe - bounds borrow the captured segment only for synchronous admission.
void sharding_require_interval(int table, MakoShardBytes lo, bool has_hi,
                               MakoShardBytes hi, MakoShardRoute route);
// @unsafe - enumerate the immutable routing view retained by this logical operation.
uint32_t sharding_scan_segments(uint64_t table, MakoShardBytes lo, uint32_t has_hi,
                                MakoShardBytes hi, uint32_t reverse,
                                MakoScanSegmentCallback callback, void* user);
// @unsafe - binds a received identity; no fresh grant is computed at the receiver.
bool sharding_bind_request(const ShardingRequest& request);
void sharding_set_request(const ShardingRequest& request);
// @unsafe - resolves the receiver's actual physical index, never the sender's ID.
int sharding_request_table(const ShardingRequest& request, int legacy_table);
void sharding_finish_request();

// @unsafe - replication-log application only. Authorizes an isolated replay
// engine lane, never ordinary admission or a migration-enabled native process.
class ShardingReplicationReplay {
public:
    ShardingReplicationReplay();
    ~ShardingReplicationReplay();
    ShardingReplicationReplay(const ShardingReplicationReplay&) = delete;
    ShardingReplicationReplay& operator=(const ShardingReplicationReplay&) = delete;
};

// @unsafe - pins one logical operation across internal OCC/RPC retries. The
// destructor releases only after the engine completion hook has run.
class ShardingOperation {
public:
    ShardingOperation();
    ~ShardingOperation();
    ShardingOperation(const ShardingOperation&) = delete;
    ShardingOperation& operator=(const ShardingOperation&) = delete;
private:
    bool outer_;
};
}
#endif
