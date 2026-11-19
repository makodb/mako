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
    /* Range ownership incarnation; independent of transaction/snapshot versions. */
    uint64_t ownership_version;
} MakoShardGrant;

/* Paired by one immutable route lookup, never refreshed separately on retry.
 * Incoming snapshot numbers are labels, not proof of routing provenance.
 * Point and whole-range admission always validate captured local metadata. */
#define MAKO_SHARD_NO_SNAPSHOT UINT64_MAX
typedef struct MakoShardRoute {
    MakoShardGrant grant;
    uint64_t snapshot_version;
} MakoShardRoute;

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
 * Disabled configuration is detected by C++ and never grants ownership here.
 * Ordinary accesses use the retained immutable read context below. */
uint32_t mako_sharding_route(uint64_t table, MakoShardBytes coordinate,
                             MakoShardRoute* route);
uint32_t mako_sharding_enabled(void);
uint32_t mako_sharding_active(uint32_t participant);
/* Separate from local activation: true only after every fixed peer loaded the
 * same catalog. Ordinary admission fails closed until this bootstrap barrier. */
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
 * it returns OK. Includes pending migration terminal receipts. Not an admission
 * bypass. Per-run marker directories remain until external run cleanup. */
uint32_t mako_sharding_quiesce(uint32_t participant, MakoShardBytes sync_directory);
/* One active logical context per worker. Caller must first safely pin the
 * nonzero STO epoch and retain it across retries, engine effects and cleanup.
 * Begin captures immutable metadata on owner/epoch cache miss; a cache hit
 * allocates nothing, clones no Arc and acquires no participant/registry mutex.
 * Nested operations reuse the active context; another begin returns BUSY.
 * The pointer is thread-bound and valid only until end. The TLS allocation is
 * reused: never retain/use an ended handle, even if its address recurs.
 * End follows actual engine cleanup and precedes leaving the STO guard. No
 * callback may end a context while one of its borrowed calls is in progress.
 * Begin checks readiness/stop/metadata fences even on cache hits. Later changes
 * do not revoke an admitted view. Owners keep engine/host storage alive until
 * admitted work ends; protected calls do not poll readiness or current epochs. */
typedef struct MakoShardRead MakoShardRead;
// @unsafe - caller pins epoch first; out is writable, failure writes null.
uint32_t mako_sharding_read_begin(uint32_t owner, uint64_t epoch, const MakoShardRead** out);
// @unsafe - same-thread terminal cleanup only; null is harmless.
void mako_sharding_read_end(const MakoShardRead* read);
// @unsafe - out is writable (cleared on failure); active same-thread read required.
// Nonzero opaque cache-incarnation ID, stable across transactions/epoch refreshes
// only while the retained Node and full admission view are unchanged. Distinct
// worker caches and A->B->A replacements get distinct IDs. Not an authorization:
// begin-time readiness, transaction-scoped admission and exact wire checks apply.
uint32_t mako_sharding_read_view_id(const MakoShardRead* read, uint64_t* out);
// @unsafe - coordinate and output are borrowed for this call.
uint32_t mako_sharding_read_route(const MakoShardRead* read, uint64_t table,
                                  MakoShardBytes coordinate, MakoShardRoute* route);
// @unsafe - coordinate is borrowed; exact captured metadata authorizes it.
uint32_t mako_sharding_read_point(const MakoShardRead* read, uint64_t table,
                                  MakoShardBytes coordinate, MakoShardRoute route);
// @unsafe - every coordinate of the nonempty half-open range is checked.
uint32_t mako_sharding_read_range(const MakoShardRead* read, uint64_t table,
                                  MakoShardBytes lo, uint32_t has_hi,
                                  MakoShardBytes hi, MakoShardRoute route);

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
// Select from the existing complete grant; never refresh routing or authorize bytes.
uint32_t mako_sharding_warehouse_select(uint64_t table, uint32_t warehouse,
                                        uint32_t participant, MakoShardGrant grant,
                                        uintptr_t* handle);
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
