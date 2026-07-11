#pragma once

// The shard's migratable DATA PLANE, engine side (masstree TU) — see
// shard_data_plane.cc. cluster_bootstrap.cc (the rrr side) serves what this
// factory returns over the ShardDataService; the two sides stay in separate
// TUs because rrr/rrr.hpp and the Masstree headers compile pathologically in
// one -O3 TU. This header is the thin seam: strings + the cluster module's
// ShardData port only.

#include <string>

import cluster;   // janus::ShardData / ShardDataCatalog / KvStore (module-owned)

class abstract_db;
class FullOrderedIndex;

namespace mako {

// The production ShardDataCatalog: named migratable tables created on demand as
// STANDALONE engine indexes (fixed ids from 9100 — system tables must not
// consume per-process open_index id slots; see standalone_index.cc), each
// registered in the process table registry (so the non-txn write handler can
// resolve an op's table NAME to query the per-table migration freeze), and each
// behind a per-op engine-registration gate (rrr service handler threads are not
// otherwise Silo/mbta-registered). `own_addr` is THIS shard's data-plane
// service address (host:port): each table self-identifies with it
// (ShardData::service_addr), so a remote migration DESTINATION can pull
// directly from this shard instead of relaying rows through the coordinator.
// Warehouse specs ("wh:<gwid>:<logical>", see tpcc_warehouse_directory.h)
// resolve the REAL workload index through the warehouse directory instead of
// creating a standalone table -- `my_shard` names this shard for the
// destination-side adoption open. Tables and the catalog are process-lifetime
// (leaked). Returns nullptr if db is null.
janus::ShardDataCatalog* make_engine_shard_catalog(abstract_db* db,
                                                   const std::string& own_addr,
                                                   int my_shard);

// Engine-register the calling thread with `db` (idempotent per thread). The
// admin Migrate handler calls this up front so config-store writes (the
// commit's cutover publish) are engine-safe even when no local data op ran
// first.
void engine_register_this_thread(abstract_db* db);

// A KvStore over `idx` whose every op engine-registers the calling thread
// first — the config-store twin of make_engine_shard_data. The ConfigWatcher
// poll thread and the ConfigKvService rrr handler thread both read the
// __mako_config__ mbta index without any prior engine setup; un-gated, those
// reads segfault the shard-0 leader. Returns a process-lifetime (leaked) store.
janus::KvStore* make_engine_kv_store(abstract_db* db, ::FullOrderedIndex* idx);

// Seed `count` demo rows ("d00".."dNN" -> "v<i>") into `sd` from a fresh,
// engine-registered thread (joined). Test/demo helper for the live migration
// bed (gated by env in the bootstrap); the main thread never touches sd.
void seed_shard_data(janus::ShardData* sd, int count);

}  // namespace mako
