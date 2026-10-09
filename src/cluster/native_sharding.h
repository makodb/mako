#ifndef MAKO_CLUSTER_NATIVE_SHARDING_H
#define MAKO_CLUSTER_NATIVE_SHARDING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed bytes; the caller retains storage for the duration of the call. */
typedef struct MakoShardBytes {
    const uint8_t* data;
    size_t len;
} MakoShardBytes;

typedef struct MakoShardTxn {
    uint64_t client;
    uint64_t sequence;
} MakoShardTxn;

typedef struct MakoShardGrant {
    uint32_t owner;
    uint64_t epoch;
} MakoShardGrant;

enum MakoShardStatus {
    MAKO_SHARD_OK = 0,
    MAKO_SHARD_NOT_FOUND = 1,
    MAKO_SHARD_RETRY = 2,
    MAKO_SHARD_INVALID = 3,
    MAKO_SHARD_BUSY = 4,
    MAKO_SHARD_EXHAUSTED = 5,
    MAKO_SHARD_IO = 6
};

/* These functions target the process's initialized native participant/cache.
 * Disabled cluster configuration is detected by the C++ caller, not treated as
 * successful ownership by these functions. Acquiring a lease and checking the
 * local role/incarnation take the same Rust mutex. A successful lease remains
 * held across storage access, transaction retries, validation and cleanup until
 * the terminal finish call. All engine reads, including scan scopes, pin leases.
 */
uint32_t mako_sharding_route(uint64_t table, MakoShardBytes coordinate,
                             MakoShardGrant* grant);
uint32_t mako_sharding_enabled(void);
uint32_t mako_sharding_active(uint32_t participant);
/* Separate from local activation: true only after every fixed peer loaded the
 * same catalog. Ordinary leases fail closed until this bootstrap barrier. */
uint32_t mako_sharding_ready(uint32_t participant);
/* Startup only: wait for the native catalog handshake before releasing ordinary
 * workers. No engine, participant, or host ownership lock may span this call. */
uint32_t mako_sharding_wait_ready(uint32_t participant);
/* Replication-log engine admission only, never a loader/ordinary access bypass.
 * replicated is the trusted host's replication configuration (must be 1).
 * Rejects any migration-enabled process, including before/after node start. */
uint32_t mako_sharding_replication_replay(uint32_t replicated);
/* Benchmark-only fixed-live shutdown barrier on the existing shared sync
 * filesystem (UTF-8 path). Call after this owner's ordinary workers join and
 * acknowledge all terminal RPCs; keep every data/control listener alive until
 * it returns OK. Includes pending migration terminal receipts. Not a lease
 * bypass. Per-run marker directories remain until external run cleanup. */
uint32_t mako_sharding_quiesce(uint32_t participant, MakoShardBytes sync_directory);
uint32_t mako_sharding_lease_begin(uint32_t participant, MakoShardTxn transaction);
uint32_t mako_sharding_lease_acquire(uint32_t participant, MakoShardTxn transaction,
                                     uint64_t table, MakoShardBytes coordinate,
                                     MakoShardGrant grant);
uint32_t mako_sharding_lease_acquire_range(uint32_t participant, MakoShardTxn transaction,
                                           uint64_t table, MakoShardBytes lo,
                                           uint32_t has_hi, MakoShardBytes hi,
                                           MakoShardGrant grant);
uint32_t mako_sharding_lease_finish(uint32_t participant, MakoShardTxn transaction);

/* Catalog construction precedes loading/activation. Identity and table kind
 * are explicit; an unknown table never means an unmanaged successful access. */
uint32_t mako_sharding_catalog_tpcc(uint32_t micro);
uint32_t mako_sharding_catalog_register(uint64_t table, MakoShardBytes name,
                                        uint32_t kind, uint32_t initial_owner);
uint32_t mako_sharding_table_id(MakoShardBytes logical_name, uint64_t* table);
uint32_t mako_sharding_table_kind(MakoShardBytes logical_name, uint64_t* table,
                                  uint32_t* kind);
uint32_t mako_sharding_table_coordinate(uint64_t table, uint32_t* coordinates);
uint32_t mako_sharding_warehouse_count(void);

/* Openers return non-owning engine handles, live until native shutdown.
 * proxy=0 selects owner-local bytes; proxy=1 selects an origin-owned proxy.
 * Callbacks must not unwind or reenter the warehouse directory. */
typedef uint32_t (*MakoWarehouseOpen)(void* context, uint64_t table,
                                      uint32_t warehouse, uint32_t owner,
                                      uint32_t proxy, uintptr_t* handle);
uint32_t mako_sharding_warehouse_init(uint32_t warehouses_per_shard, uint32_t total);
uint32_t mako_sharding_warehouse_opener(uint32_t participant, void* context,
                                        MakoWarehouseOpen open);
uint32_t mako_sharding_warehouse_register(uint64_t table, uint32_t warehouse,
                                          uint32_t owner, uint32_t proxy,
                                          uintptr_t handle);
uint32_t mako_sharding_warehouse_resolve(uint64_t table, uint32_t warehouse,
                                         uint32_t participant, uintptr_t* handle,
                                         MakoShardGrant* grant);
uint32_t mako_sharding_warehouse_local(uint64_t table, uint32_t warehouse,
                                       uint32_t owner, uintptr_t* handle);
uint32_t mako_sharding_local_table(uint64_t table, MakoShardBytes coordinate,
                                   uint32_t fixed, uint32_t owner, uintptr_t* handle);
uint32_t mako_sharding_raw_proxy(uint64_t table, uint32_t origin, uintptr_t* handle);
uint32_t mako_sharding_raw_register(uint64_t table, uint32_t owner,
                                    uint32_t proxy, uintptr_t handle);

#ifdef __cplusplus
}
#endif
#endif
