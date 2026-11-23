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
// Limits of this slice: raw tables only, no proxies, no table creation after
// start-up, non-replicated processes only, and no admission on ordinary
// transactions (nothing here stops an ordinary Rust-engine transaction from
// writing a frozen or moved range).
//
// Order the caller keeps: open every index, rust_sharding_register_raw for
// each, rust_sharding_callbacks and mako_sharding_start_node per owner,
// (migrations), mako_sharding_stop_node per owner, rust_sharding_release, and
// only then destroy the indexes and the database.

#include <stdint.h>
#include "native_sharding_host.h"
#include "abstract_ordered_index.h"

namespace mako {

// @unsafe - Storage and thread slots for a Rust-engine database. The context
// stays `&base`, so peer_call, epoch_cut and epoch_drained are base's own.
// The first call closes registration. Throws std::logic_error when base.db is
// not a rust_sto_tpcc_wrapper or the process is replicated.
MakoShardHost rust_sharding_callbacks(NativeShardingHost& base);

// @unsafe - Startup only, before the first rust_sharding_callbacks(). `index`
// must be the owner-local Rust index that holds raw catalog table `table` for
// `owner`. Registers it with the Rust verbs and with the sharding crate, and
// returns a MakoShardStatus: INVALID when a condition above does not hold or
// the pair is already registered with another index.
uint32_t rust_sharding_register_raw(uint64_t table, uint32_t owner,
                                    abstract_ordered_index* index);

// @unsafe - After every local node has stopped and before any registered index
// or the database is destroyed. Reopens registration.
void rust_sharding_release() noexcept;

} // namespace mako
#endif
