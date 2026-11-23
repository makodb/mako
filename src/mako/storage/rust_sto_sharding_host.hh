#ifndef MAKO_STORAGE_RUST_STO_SHARDING_HOST_HH
#define MAKO_STORAGE_RUST_STO_SHARDING_HOST_HH

// Storage and thread callbacks of the native sharding host for a database
// that runs the Rust transaction engine (rust_sto_tpcc_wrapper).
//
// The three storage verbs are the Rust functions of
// crates/sto-tpcc-ffi/src/shard_host.rs. This file supplies what must be C++:
// attaching the migration thread to the adapter, the fail-closed `open`, and
// the registration that tells both the Rust verbs and the sharding crate
// which engine table holds a catalog table on an owner.
//
// Each owner registers its local raw indexes before its node starts. The host
// supplies a proxy for each registered index and shares the epoch clock with
// ordinary transaction admission. Storage callbacks run on their own thread.
// Explicitly preopened warehouse indexes use four-byte big-endian coordinates.

#include <stdint.h>
#include "native_sharding_host.h"
#include "abstract_ordered_index.h"

namespace mako {

// @unsafe - Storage and thread slots for a Rust-engine database. The context
// stays `&base`, so peer_call, epoch_cut and epoch_drained are base's own.
// The first call closes registration for this owner and starts the epoch clock.
MakoShardHost rust_sharding_callbacks(NativeShardingHost& base);

// @unsafe - Call before activating this owner. The owner-local Rust index was
// opened with its raw catalog name and must outlive the node. An identical
// registration is idempotent until the owner starts.
uint32_t rust_sharding_register_raw(uint64_t table, uint32_t owner,
                                    abstract_ordered_index* index);

// @unsafe - An explicitly preopened growable physical index for a warehouse.
// The caller supplies its catalog identity and keeps it alive through node shutdown.
uint32_t rust_sharding_register_warehouse(uint64_t table, uint32_t owner,
                                         uint32_t warehouse, abstract_ordered_index* index);

// @unsafe - After every local node has stopped and before any registered index
// or the database is destroyed. Bootstrap releases each owner when it stops.
void rust_sharding_release() noexcept;

} // namespace mako
#endif
