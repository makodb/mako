#ifndef MAKO_SHARDING_LEASES_H
#define MAKO_SHARDING_LEASES_H

#include "cluster/native_sharding.h"
#include <cstddef>
#include <cstdint>
#include <string>

class FullOrderedIndex;

namespace mako {
// @unsafe - only a newly allocated, unused MBTA handle may become a proxy.
void sharding_mark_proxy(FullOrderedIndex* index);
// Transport-owned POD; never substitute a receiver's current route for this grant.
struct ShardingRequest {
    MakoShardTxn transaction;
    MakoShardGrant grant;
    uint64_t table;
    uint16_t coordinate_length;
    bool fixed_coordinate;
    uint8_t coordinate[64];
};

// @unsafe - native configuration/legacy thread-local engine boundary.
bool sharding_leases_enabled();
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
ShardingRequest sharding_outgoing_request();
void sharding_require_scan(int table, const std::string& start,
                           const std::string* end, bool reverse = false);
// @unsafe - captures a route once at the sender; returns -1 on rejected routing.
int sharding_route_request(int table, const std::string& key, ShardingRequest& request);
// @unsafe - selects a borrowed canonical physical handle or this origin's proxy.
// Returns nullptr only for explicitly static/immutable or disabled routing.
FullOrderedIndex* sharding_point_handle(int table, const char* key, size_t length);
// @unsafe - scan snapshots supply the original segment grant without rerouting.
ShardingRequest sharding_request_with_grant(int table, const std::string& key,
                                            MakoShardGrant grant);
void sharding_use_outgoing_request(const ShardingRequest& request);
void sharding_require_interval(int table, const std::string& lo,
                               const std::string* hi, MakoShardGrant grant);
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
